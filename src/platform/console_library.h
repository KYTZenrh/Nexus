#pragma once
// The console's own list of installed titles, read from its app database
// (/system_data/priv/mms/app.db). Kept free of console APIs so it can be tested on PC
// against a copy of the file.
#include <cstdint>
#include <string>
#include <vector>

#include "platform.h"

namespace nexus {

// Installed titles the user can launch, most recently used first. System apps (NPXS...) and
// `selfTitleId` (Nexus itself) are left out. `userId` picks the per-user table that holds the
// home screen order; 0 or a missing table falls back to install order.
// On failure returns empty and sets `error`.
std::vector<Game> readConsoleLibrary(const std::string& dbPath, uint32_t userId,
                                     const std::string& selfTitleId, std::string& error);

}  // namespace nexus
