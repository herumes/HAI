#pragma once
#include <string>

namespace Knowledge {

    // Load hytale_mobs.json + hytale_items.json from DLL directory (once).
    bool Load(bool force = false);

    // Mob tools
    std::string ToolLookupMob(const std::string& query);
    std::string ToolSearchMobs(
        const std::string& query,
        const std::string& dropsItem,
        const std::string& trait,
        int minHealth,
        int maxHealth,
        int limit);

    // Item tools
    std::string ToolLookupItem(const std::string& query);
    std::string ToolSearchItems(
        const std::string& query,
        const std::string& quality,
        const std::string& category,
        int limit);

    std::string Status();

} // namespace Knowledge
