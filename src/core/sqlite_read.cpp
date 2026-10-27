#include "sqlite_read.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace nexus {

static uint32_t be16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

// SQLite varint: big-endian, 7 bits per byte, the 9th byte contributes all 8 bits.
static const uint8_t* varint(const uint8_t* p, const uint8_t* end, uint64_t& v) {
    v = 0;
    for (int i = 0; i < 9; i++) {
        if (p >= end) return nullptr;
        uint8_t b = *p++;
        if (i == 8) {
            v = (v << 8) | b;
            return p;
        }
        v = (v << 7) | (b & 0x7f);
        if (!(b & 0x80)) return p;
    }
    return p;
}

bool SqliteFile::open(const std::string& path) {
    data_.clear();
    master_.clear();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) data_.insert(data_.end(), buf, buf + n);
    std::fclose(f);
    if (data_.size() < 100 || std::memcmp(data_.data(), "SQLite format 3", 16) != 0) return false;
    pageSize_ = be16(&data_[16]);
    if (pageSize_ == 1) pageSize_ = 65536;
    if (pageSize_ < 512) return false;
    usable_ = pageSize_ - data_[20];
    pages_ = (uint32_t)(data_.size() / pageSize_);

    // sqlite_schema (page 1): type, name, tbl_name, rootpage, sql
    std::vector<std::vector<SqlValue>> rows;
    if (!walk(1, rows, 0)) return false;
    for (auto& r : rows) {
        if (r.size() < 5) continue;
        Master m;
        m.type = r[0].s;
        m.name = r[1].s;
        m.root = r[3].type == SqlValue::Int ? (uint32_t)r[3].i : 0;
        m.sql = r[4].s;
        master_.push_back(m);
    }
    return true;
}

const uint8_t* SqliteFile::page(uint32_t n) const {
    if (n == 0 || n > pages_) return nullptr;
    return data_.data() + (size_t)(n - 1) * pageSize_;
}

const SqliteFile::Master* SqliteFile::find(const std::string& table) const {
    for (auto& m : master_)
        if (m.type == "table" && m.name == table) return &m;
    return nullptr;
}

std::vector<std::string> SqliteFile::tables() const {
    std::vector<std::string> v;
    for (auto& m : master_)
        if (m.type == "table") v.push_back(m.name);
    return v;
}

std::vector<std::string> SqliteFile::columns(const std::string& table) const {
    std::vector<std::string> cols;
    const Master* m = find(table);
    if (!m) return cols;
    size_t open = m->sql.find('(');
    if (open == std::string::npos) return cols;
    int depth = 0;
    std::string cur;
    auto flush = [&] {
        size_t a = cur.find_first_not_of(" \t\r\n");
        if (a != std::string::npos) {
            size_t b = cur.find_first_of(" \t\r\n", a);
            std::string name = cur.substr(a, b == std::string::npos ? std::string::npos : b - a);
            if (name.size() > 1 && (name[0] == '"' || name[0] == '`' || name[0] == '['))
                name = name.substr(1, name.size() - 2);
            // table constraints are not columns
            if (name != "PRIMARY" && name != "UNIQUE" && name != "CHECK" && name != "FOREIGN" &&
                name != "CONSTRAINT")
                cols.push_back(name);
        }
        cur.clear();
    };
    for (size_t i = open + 1; i < m->sql.size(); i++) {
        char c = m->sql[i];
        if (c == '(') depth++;
        if (c == ')') {
            if (depth == 0) break;
            depth--;
        }
        if (c == ',' && depth == 0) flush();
        else cur += c;
    }
    flush();
    return cols;
}

bool SqliteFile::rows(const std::string& table, std::vector<std::vector<SqlValue>>& out) const {
    const Master* m = find(table);
    return m && walk(m->root, out, 0);
}

bool SqliteFile::walk(uint32_t pg, std::vector<std::vector<SqlValue>>& out, int depth) const {
    const uint8_t* p = page(pg);
    if (!p || depth > 32) return false;
    const uint8_t* end = p + pageSize_;
    const uint8_t* h = pg == 1 ? p + 100 : p;
    uint8_t type = h[0];
    uint32_t ncell = be16(h + 3);
    if (type == 0x05) {  // interior table page: child pointers, then the right-most child
        const uint8_t* ptrs = h + 12;
        for (uint32_t i = 0; i < ncell; i++) {
            uint32_t off = be16(ptrs + 2 * i);
            if (off + 4 > pageSize_) return false;
            if (!walk(be32(p + off), out, depth + 1)) return false;
        }
        return walk(be32(h + 8), out, depth + 1);
    }
    if (type != 0x0D) return false;  // only table b-trees are read
    const uint8_t* ptrs = h + 8;
    for (uint32_t i = 0; i < ncell; i++) {
        uint32_t off = be16(ptrs + 2 * i);
        if (off >= pageSize_) return false;
        std::vector<SqlValue> row;
        if (!cell(p + off, end, row)) return false;
        out.push_back(std::move(row));
    }
    return true;
}

// One leaf cell: payload size, rowid, the locally stored part of the payload, and (for big
// rows) a chain of overflow pages holding the rest.
bool SqliteFile::cell(const uint8_t* c, const uint8_t* end, std::vector<SqlValue>& row) const {
    uint64_t size = 0, rowid = 0;
    c = varint(c, end, size);
    if (!c) return false;
    c = varint(c, end, rowid);
    if (!c) return false;
    const uint64_t U = usable_, X = U - 35;
    uint64_t local = size;
    if (size > X) {
        uint64_t M = ((U - 12) * 32 / 255) - 23;
        uint64_t K = M + ((size - M) % (U - 4));
        local = K <= X ? K : M;
    }
    if (c + local > end) return false;
    std::string payload((const char*)c, (size_t)local);
    if (local < size) {
        if (c + local + 4 > end) return false;
        uint32_t next = be32(c + local);
        int guard = 0;
        while (payload.size() < size && next && guard++ < 100000) {
            const uint8_t* op = page(next);
            if (!op) return false;
            size_t take = (size_t)std::min<uint64_t>(U - 4, size - payload.size());
            payload.append((const char*)op + 4, take);
            next = be32(op);
        }
        if (payload.size() < size) return false;
    }
    decode(payload, row);
    return true;
}

void SqliteFile::decode(const std::string& payload, std::vector<SqlValue>& row) const {
    const uint8_t* base = (const uint8_t*)payload.data();
    const uint8_t* end = base + payload.size();
    uint64_t hdr = 0;
    const uint8_t* t = varint(base, end, hdr);
    if (!t || hdr > payload.size()) return;
    const uint8_t* hend = base + hdr;
    const uint8_t* body = hend;
    while (t && t < hend) {
        uint64_t st = 0;
        t = varint(t, hend, st);
        if (!t) break;
        SqlValue v;
        size_t len = 0;
        if (st == 0) {
            v.type = SqlValue::Null;
        } else if (st <= 6 || st == 8 || st == 9) {
            static const size_t sizes[] = {0, 1, 2, 3, 4, 6, 8};
            v.type = SqlValue::Int;
            if (st == 8 || st == 9) {
                v.i = st == 9;
            } else {
                len = sizes[st];
                if (body + len > end) break;
                int64_t x = (int8_t)body[0];  // sign-extend from the first byte
                for (size_t k = 1; k < len; k++) x = (x << 8) | body[k];
                v.i = x;
            }
        } else if (st == 7) {
            len = 8;
            if (body + len > end) break;
            uint64_t bits = 0;
            for (size_t k = 0; k < 8; k++) bits = (bits << 8) | body[k];
            v.type = SqlValue::Real;
            std::memcpy(&v.r, &bits, sizeof bits);
        } else {
            len = (size_t)((st - 12) / 2);
            if (body + len > end) break;
            v.type = (st & 1) ? SqlValue::Text : SqlValue::Blob;
            v.s.assign((const char*)body, len);
        }
        body += len;
        row.push_back(std::move(v));
    }
}

}  // namespace nexus
