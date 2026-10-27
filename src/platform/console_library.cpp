#include "console_library.h"

#include <algorithm>
#include <cstdio>
#include <map>

#include "../core/sqlite_read.h"

namespace nexus {

static int colIndex(const std::vector<std::string>& cols, const char* name) {
    for (size_t i = 0; i < cols.size(); i++)
        if (cols[i] == name) return (int)i;
    return -1;
}

static std::string textAt(const std::vector<SqlValue>& row, int i) {
    return i >= 0 && i < (int)row.size() && row[i].type == SqlValue::Text ? row[i].s : std::string();
}

static int64_t intAt(const std::vector<SqlValue>& row, int i, int64_t def) {
    return i >= 0 && i < (int)row.size() && row[i].type == SqlValue::Int ? row[i].i : def;
}

std::vector<Game> readConsoleLibrary(const std::string& dbPath, uint32_t userId,
                                     const std::string& selfTitleId, std::string& error) {
    std::vector<Game> games;
    SqliteFile db;
    if (!db.open(dbPath)) {
        error = "Could not open the console's title database (" + dbPath + ").";
        return games;
    }

    // Installed titles: name and icon.
    struct Info {
        std::string name, icon;
        int64_t order = -1;  // home screen recency from the user's table, -1 if unknown
        size_t seq = 0;      // position in the install table, used as a tie-breaker
    };
    std::map<std::string, Info> info;
    std::vector<std::vector<SqlValue>> rows;
    auto cols = db.columns("tbl_contentinfo");
    if (cols.empty() || !db.rows("tbl_contentinfo", rows)) {
        error = "The console's title database has no readable title table.";
        return games;
    }
    int cId = colIndex(cols, "titleId"), cName = colIndex(cols, "titleName"),
        cMeta = colIndex(cols, "metaDataPath"), cIcon = colIndex(cols, "icon0Info");
    size_t seq = 0;
    for (auto& r : rows) {
        std::string id = textAt(r, cId);
        if (id.size() != 9 || id.rfind("NPXS", 0) == 0 || id == selfTitleId) continue;
        Info in;
        in.name = textAt(r, cName);
        in.icon = textAt(r, cIcon);
        size_t q = in.icon.find('?');  // icon paths carry a "?ts=..." cache-buster
        if (q != std::string::npos) in.icon.resize(q);
        if (in.icon.empty()) {
            std::string meta = textAt(r, cMeta);
            if (!meta.empty()) in.icon = meta + "/icon0.png";
        }
        if (in.name.empty()) in.name = id;
        in.seq = seq++;
        info[id] = in;
    }

    // The signed-in user's home screen table gives the "recently used" order.
    if (userId) {
        char tbl[48];
        std::snprintf(tbl, sizeof tbl, "tbl_iconinfo_%010u", userId);
        auto ucols = db.columns(tbl);
        std::vector<std::vector<SqlValue>> urows;
        if (!ucols.empty() && db.rows(tbl, urows)) {
            int uId = colIndex(ucols, "titleId"), uIdx = colIndex(ucols, "lastAccessIndex"),
                uVis = colIndex(ucols, "visible");
            for (auto& r : urows) {
                auto it = info.find(textAt(r, uId));
                if (it == info.end()) continue;
                if (intAt(r, uVis, 1) == 0) {  // hidden from the home screen
                    info.erase(it);
                    continue;
                }
                it->second.order = intAt(r, uIdx, -1);
            }
        }
    }

    std::vector<std::pair<std::string, Info>> list(info.begin(), info.end());
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
        if (a.second.order != b.second.order) return a.second.order > b.second.order;
        return a.second.seq > b.second.seq;
    });
    for (auto& [id, in] : list) {
        Game g;
        g.titleId = id;
        g.title = in.name;
        g.iconPath = in.icon;
        games.push_back(g);
    }
    if (games.empty()) error = "No installed games or apps were found.";
    return games;
}

}  // namespace nexus
