#pragma once
// Minimal read-only reader for SQLite database files: enough to list the rows of a table.
// The console keeps its title list in SQLite files (/system_data/priv/mms/app.db), and the
// console SDK ships no SQLite library, so this walks the file format directly
// (https://www.sqlite.org/fileformat.html). No writes, no indexes, no WAL.
#include <cstdint>
#include <string>
#include <vector>

namespace nexus {

struct SqlValue {
    enum Type { Null, Int, Real, Text, Blob } type = Null;
    int64_t i = 0;
    double r = 0;
    std::string s;  // text or blob bytes
};

class SqliteFile {
public:
    bool open(const std::string& path);  // reads the whole file; false if missing or not SQLite
    // Column names of a table, from its CREATE TABLE statement (empty if the table is missing).
    std::vector<std::string> columns(const std::string& table) const;
    // Every row of a table, columns in declaration order. False if the table is missing or damaged.
    bool rows(const std::string& table, std::vector<std::vector<SqlValue>>& out) const;
    std::vector<std::string> tables() const;

private:
    struct Master {
        std::string type, name, sql;
        uint32_t root = 0;
    };
    const uint8_t* page(uint32_t n) const;
    bool walk(uint32_t pg, std::vector<std::vector<SqlValue>>& out, int depth) const;
    bool cell(const uint8_t* p, const uint8_t* end, std::vector<SqlValue>& row) const;
    void decode(const std::string& payload, std::vector<SqlValue>& row) const;
    const Master* find(const std::string& table) const;

    std::vector<uint8_t> data_;
    uint32_t pageSize_ = 0, usable_ = 0, pages_ = 0;
    std::vector<Master> master_;
};

}  // namespace nexus
