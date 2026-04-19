#include "stdafx.h"

#include "MapAnnotationsModule.h"

#include <fstream>
#include <list>
#include <unordered_set>

#include <GWCA/Context/GameplayContext.h>
#include <GWCA/Packets/StoC.h>
#include <GWCA/Managers/StoCMgr.h>
#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/Item.h>
#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/ItemMgr.h>
#include <GWCA/Managers/PartyMgr.h>
#include <GWCA/Managers/GameThreadMgr.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/UIMgr.h>

#include <GWCA/Context/MapContext.h>
#include <GWCA/GameEntities/Map.h>

#include <ImGuiAddons.h>
#include <Modules/Resources.h>
#include <Utils/TextUtils.h>
#include <Utils/ToolboxUtils.h>
#include <Utils/GuiUtils.h>
#include <Utils/Compositor.h>
#include <Widgets/MissionMapWidget.h>
#include <Widgets/WorldMapWidget.h>
#include <Windows/Pathfinding/PathfindingWindow.h>

// Forward declarations for overlay callbacks registered in Initialize
static void DrawWorldMapOverlay(ImDrawList& draw_list);
static void DrawMissionMapOverlay(ImDrawList& draw_list);

namespace {

    ImU32 ApplyDisplayState(ImU32 color, RouteDisplayState state) {
        switch (state) {
        case RouteDisplayState::Highlighted:
            return color | 0xFF000000;
        case RouteDisplayState::Dimmed:
            return (color & 0x00FFFFFF) | (0x40 << 24);
        default:
            return color;
        }
    }

    bool initialized = false;
    bool edit_mode = false;
    bool show_context_menu_edit = true; // Whether to show edit items in annotation context menus

    // Agent death tracking via packet hook
    GW::HookEntry agent_state_hook;
    std::unordered_set<uint32_t> dead_agent_ids; // Populated by AgentState packet, cleared on map change
    std::vector<MapAnnotationLayer> layers;
    uint32_t next_marker_id = 1;
    uint32_t next_route_id = 1;

    // Route editing state (manual waypoint placement)
    bool editing_route = false;
    MapAnnotationRoute pending_route;

    // Route recording state (walk to record)
    bool recording_route = false;
    MapAnnotationRoute recording_route_data;
    float recording_interval = 1.0f; // seconds between samples
    float recording_timer = 0.f;
    GW::Constants::MapID recording_map_id = GW::Constants::MapID::None;



    // Marker/route being hovered on world map (set during DrawOnWorldMap)
    MapAnnotationMarker* hovered_marker = nullptr;
    MapAnnotationLayer* hovered_marker_layer = nullptr;
    MapAnnotationRoute* hovered_route = nullptr;
    MapAnnotationLayer* hovered_route_layer = nullptr;

    // Marker/route being hovered on mission map (set during DrawOnMissionMap)
    MapAnnotationMarker* mm_hovered_marker = nullptr;
    MapAnnotationLayer* mm_hovered_marker_layer = nullptr;
    MapAnnotationRoute* mm_hovered_route = nullptr;
    MapAnnotationLayer* mm_hovered_route_layer = nullptr;
    int mm_hovered_waypoint_idx = -1;
    int mm_hovered_edge_idx = -1;

    // Snapshot when context menu was opened — survives mouse movement
    MapAnnotationMarker* context_menu_marker = nullptr;
    MapAnnotationLayer* context_menu_marker_layer = nullptr;
    MapAnnotationRoute* context_menu_route = nullptr;
    MapAnnotationLayer* context_menu_route_layer = nullptr;
    bool context_menu_snapshotted = false;

    // Route node editing state
    int hovered_waypoint_idx = -1;   // closest waypoint index on hovered route
    int hovered_edge_idx = -1;       // closest edge index on hovered route
    int context_waypoint_idx = -1;   // snapshotted for context menu
    int context_edge_idx = -1;       // snapshotted for context menu

    // "Move node" mode: next right-click places the node
    bool moving_waypoint = false;
    MapAnnotationRoute* moving_route = nullptr;
    MapAnnotationLayer* moving_route_layer = nullptr;
    int moving_waypoint_idx = -1;

    // Active layer index for adding new annotations (into user layers only)
    size_t active_layer_index = 0; // Will resolve to user layer

    // Label input buffer for context menu
    char label_buf[128] = {};

    // Delete confirmation: stores the id of the annotation pending delete confirmation
    uint32_t delete_confirm_id = 0;

    // Icon selection for new markers
    std::string new_marker_icon;

    // Icon presets
    struct IconPreset {
        const char* icon;
        const char* name;
    };
    constexpr IconPreset icon_presets[] = {
        {"", "None"},
        {ICON_FA_MAP_MARKER_ALT, "Pin"},
        {ICON_FA_SKULL, "Skull"},
        {ICON_FA_SKULL_CROSSBONES, "Danger"},
        {ICON_FA_GEM, "Gem"},
        {ICON_FA_COINS, "Coins"},
        {ICON_FA_STAR, "Star"},
        {ICON_FA_FLAG, "Flag"},
        {ICON_FA_FLAG_CHECKERED, "Finish"},
        {ICON_FA_HOME, "Home"},
        {ICON_FA_CROWN, "Crown"},
        {ICON_FA_HEART, "Heart"},
        {ICON_FA_SHIELD_ALT, "Shield"},
        {ICON_FA_BOLT, "Bolt"},
        {ICON_FA_CROSSHAIRS, "Target"},
        {ICON_FA_EXCLAMATION_TRIANGLE, "Warning"},
        {ICON_FA_QUESTION, "Question"},
        {ICON_FA_SCROLL, "Scroll"},
        {ICON_FA_DRAGON, "Dragon"},
        {ICON_FA_DUNGEON, "Dungeon"},
        {ICON_FA_MOUNTAIN, "Mountain"},
        {ICON_FA_CAMPGROUND, "Camp"},
        {ICON_FA_TROPHY, "Trophy"},
        {ICON_FA_BOOKMARK, "Bookmark"},
        {ICON_FA_ROUTE, "Route"},
    };

    struct ColorPreset {
        const char* name;
        ImU32 color;
    };

    constexpr ColorPreset marker_color_presets[] = {
        {"Mission NPC",     IM_COL32(50,  200, 255, 220)},
        {"Enemy",           IM_COL32(255, 50,  50,  220)},
        {"Boss",            IM_COL32(200, 50,  200, 220)},
        {"Vendor",          IM_COL32(50,  255, 50,  220)},
        {"Collector",       IM_COL32(100, 220, 100, 220)},
        {"Chest/Resource",  IM_COL32(255, 200, 50,  220)},
        {"Shrine/Rez",      IM_COL32(255, 255, 200, 220)},
        {"Portal/Gate",     IM_COL32(150, 150, 255, 220)},
        {"Point of Interest", IM_COL32(255, 150, 50, 220)},
    };

    struct RouteColorPreset {
        const char* name;
        ImU32 color;
        float thickness;
    };

    constexpr RouteColorPreset route_color_presets[] = {
        {"Mission (main)",  IM_COL32(50,  200, 255, 220), 4.0f},
        {"Mission (bonus)", IM_COL32(255, 200, 50,  220), 4.0f},
        {"Farming",         IM_COL32(50,  255, 50,  220), 3.0f},
        {"Running",         IM_COL32(255, 100, 50,  220), 3.5f},
        {"Speedclear",      IM_COL32(255, 50,  50,  220), 3.5f},
        {"Vanquish",        IM_COL32(180, 80,  220, 220), 3.0f},
    };

    // Pending enc_name decode results (wstring written by AsyncDecodeStr, converted in Update)
    struct PendingEncDecode {
        uint32_t marker_id = 0;
        std::wstring decoded;
    };
    std::list<PendingEncDecode> pending_enc_decodes;
    std::list<PendingEncDecode> pending_english_decodes;
    GW::Constants::Language last_language = static_cast<GW::Constants::Language>(0xff);

    // Trigger async decode for markers that have enc_name but no decoded_name yet
    void EnsureMarkerDecoded(MapAnnotationMarker& marker)
    {
        if (marker.enc_name.empty() || !marker.decoded_name.empty() || marker.decode_pending) return;
        marker.decode_pending = true;
        auto& pending = pending_enc_decodes.emplace_back();
        pending.marker_id = marker.id;
        GW::UI::AsyncDecodeStr(marker.enc_name.c_str(), &pending.decoded);
    }

    // Trigger async decode of English name for wiki links
    void EnsureMarkerEnglishDecoded(MapAnnotationMarker& marker)
    {
        if (marker.enc_name.empty() || !marker.english_name.empty() || marker.english_decode_pending) return;
        marker.english_decode_pending = true;
        auto& pending = pending_english_decodes.emplace_back();
        pending.marker_id = marker.id;
        GW::UI::AsyncDecodeStr(marker.enc_name.c_str(), &pending.decoded, GW::Constants::Language::English);
    }

    // Get display name for a marker
    const char* GetMarkerDisplayName(const MapAnnotationMarker& marker)
    {
        if (!marker.enc_name.empty() && !marker.decoded_name.empty()) {
            return marker.decoded_name.c_str();
        }
        return marker.label.c_str();
    }

    // Get the wiki-ready name (English, with spaces replaced by underscores)
    std::string GetMarkerWikiName(const MapAnnotationMarker& marker)
    {
        std::string name;
        if (!marker.enc_name.empty() && !marker.english_name.empty()) {
            name = marker.english_name;
        } else {
            name = marker.label;
        }
        // Replace spaces with underscores for wiki URL
        for (auto& c : name) {
            if (c == ' ') c = '_';
        }
        return name;
    }

    void OpenMarkerWiki(MapAnnotationMarker& marker)
    {
        if (!marker.wiki_enabled) return;
        EnsureMarkerEnglishDecoded(marker);
        const auto wiki_name = GetMarkerWikiName(marker);
        if (wiki_name.empty()) return;
        const auto url = std::string("https://wiki.guildwars.com/wiki/") + wiki_name;
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    // --- Spawn group helpers ---

    SpawnGroupDef* FindSpawnGroup(MapAnnotationLayer& layer, const std::string& name)
    {
        for (auto& sg : layer.spawn_groups) {
            if (sg.name == name) return &sg;
        }
        return nullptr;
    }

    std::string SpawnGroupLabel(const SpawnGroupDef& sg)
    {
        auto* mn = Resources::GetMapName(sg.map_id);
        return mn ? std::format("{} / {}", mn->string(), sg.name)
                  : std::format("Map {} / {}", static_cast<uint32_t>(sg.map_id), sg.name);
    }

    const SpawnGroupDef* FindSpawnGroupConst(const std::string& group_name, GW::Constants::MapID map_id)
    {
        for (const auto& layer : layers) {
            for (const auto& sg : layer.spawn_groups) {
                if (sg.name == group_name && sg.map_id == map_id) return &sg;
            }
        }
        return nullptr;
    }

    // Decode spawn group NPC names (same pattern as marker decode)
    struct PendingSpawnNpcDecode {
        const SpawnGroupNpc* npc = nullptr;
        std::wstring decoded;
    };
    std::list<PendingSpawnNpcDecode> pending_spawn_npc_decodes;

    void EnsureSpawnNpcDecoded(const SpawnGroupNpc& npc)
    {
        if (npc.enc_name.empty() || !npc.decoded_name.empty() || npc.decode_pending) return;
        npc.decode_pending = true;
        auto& pending = pending_spawn_npc_decodes.emplace_back();
        pending.npc = &npc;
        GW::UI::AsyncDecodeStr(npc.enc_name.c_str(), &pending.decoded);
    }

    const char* GetSpawnNpcDisplayName(const SpawnGroupNpc& npc)
    {
        if (!npc.enc_name.empty() && !npc.decoded_name.empty()) {
            return npc.decoded_name.c_str();
        }
        return npc.label.c_str();
    }

    // Compare two encoded strings for equality
    bool EncNamesMatch(const wchar_t* a, const std::wstring& b)
    {
        if (!a || b.empty()) return false;
        return b == a;
    }

    // --- Pathed waypoint support ---

    bool pathing_in_progress = false;

    // Douglas-Peucker path simplification
    float PerpendicularDistance(const GW::Vec2f& point, const GW::Vec2f& start, const GW::Vec2f& end)
    {
        const float dx = end.x - start.x;
        const float dy = end.y - start.y;
        const float len_sq = dx * dx + dy * dy;
        if (len_sq == 0.0f) {
            const float px = point.x - start.x;
            const float py = point.y - start.y;
            return sqrtf(px * px + py * py);
        }
        const float t = std::clamp(((point.x - start.x) * dx + (point.y - start.y) * dy) / len_sq, 0.0f, 1.0f);
        const float px = point.x - (start.x + t * dx);
        const float py = point.y - (start.y + t * dy);
        return sqrtf(px * px + py * py);
    }

    void DouglasPeucker(const std::vector<GW::Vec2f>& points, float epsilon,
                         size_t start, size_t end, std::vector<bool>& keep)
    {
        if (end <= start + 1) return;
        float max_dist = 0.0f;
        size_t max_idx = start;
        for (size_t i = start + 1; i < end; i++) {
            const float dist = PerpendicularDistance(points[i], points[start], points[end]);
            if (dist > max_dist) {
                max_dist = dist;
                max_idx = i;
            }
        }
        if (max_dist > epsilon) {
            keep[max_idx] = true;
            DouglasPeucker(points, epsilon, start, max_idx, keep);
            DouglasPeucker(points, epsilon, max_idx, end, keep);
        }
    }

    std::vector<GW::Vec2f> SimplifyPath(const std::vector<GW::Vec2f>& points, float epsilon)
    {
        if (points.size() < 3) return points;
        std::vector<bool> keep(points.size(), false);
        keep.front() = true;
        keep.back() = true;
        DouglasPeucker(points, epsilon, 0, points.size() - 1, keep);
        std::vector<GW::Vec2f> result;
        for (size_t i = 0; i < points.size(); i++) {
            if (keep[i]) result.push_back(points[i]);
        }
        return result;
    }

    void AddPathedWaypoint(const GW::Vec2f& to_world)
    {
        if (!editing_route || pending_route.waypoints.empty()) return;
        if (pathing_in_progress) return;

        if (!PathfindingWindow::ReadyForPathing()) {
            Log::Warning("Pathfinding not available on this map");
            return;
        }

        const auto from_world = pending_route.waypoints.back();

        GW::GamePos from_game, to_game;
        if (!WorldMapWidget::WorldMapToGamePos(from_world, from_game)) return;
        if (!WorldMapWidget::WorldMapToGamePos(to_world, to_game)) return;

        pathing_in_progress = true;
        PathfindingWindow::CalculatePath(from_game, to_game,
            [](std::vector<GW::GamePos>& waypoints, void*) {
                pathing_in_progress = false;
                if (waypoints.empty() || !editing_route) return;

                std::vector<GW::Vec2f> world_points;
                for (const auto& gp : waypoints) {
                    GW::Vec2f wp;
                    if (WorldMapWidget::GamePosToWorldMap(gp, wp)) {
                        world_points.push_back(wp);
                    }
                }

                auto simplified = SimplifyPath(world_points, 0.3f);
                // Skip first point — it's approximately the existing last waypoint
                for (size_t i = 1; i < simplified.size(); i++) {
                    pending_route.waypoints.push_back(simplified[i]);
                }
            });
    }

    // Draw icon picker as a grid of buttons (works inside context menu popups).
    // Returns true if icon changed.
    bool DrawIconPicker(std::string& icon)
    {
        bool changed = false;
        constexpr int icons_per_row = 8;
        int col = 0;

        ImGui::TextDisabled("Icon:");
        ImGui::SameLine();
        // Show current selection
        if (icon.empty()) {
            ImGui::TextDisabled("None");
        }
        else {
            for (const auto& preset : icon_presets) {
                if (icon == preset.icon) {
                    ImGui::Text("%s %s", preset.icon, preset.name);
                    break;
                }
            }
        }

        for (const auto& preset : icon_presets) {
            if (col > 0) ImGui::SameLine();

            const bool selected = (icon == preset.icon);
            const std::string btn_label = preset.icon[0]
                ? std::string(preset.icon) + "##icon" + preset.name
                : std::string("X##icon_none");

            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            }
            if (ImGui::SmallButton(btn_label.c_str())) {
                icon = preset.icon;
                changed = true;
            }
            if (selected) {
                ImGui::PopStyleColor();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", preset.name);
            }

            col++;
            if (col >= icons_per_row) col = 0;
        }

        return changed;
    }

    // Point-to-line-segment distance squared
    float PointToSegmentDistSq(const ImVec2& p, const ImVec2& a, const ImVec2& b)
    {
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float len_sq = dx * dx + dy * dy;
        if (len_sq < 1e-6f) {
            const float ex = p.x - a.x;
            const float ey = p.y - a.y;
            return ex * ex + ey * ey;
        }
        float t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / len_sq;
        t = std::clamp(t, 0.f, 1.f);
        const float proj_x = a.x + t * dx;
        const float proj_y = a.y + t * dy;
        const float ex = p.x - proj_x;
        const float ey = p.y - proj_y;
        return ex * ex + ey * ey;
    }

    // Draw small direction chevrons along a route segment (screen coords)
    // Draw direction arrow if enough distance has accumulated since the last one.
    // `accum` tracks screen-pixel distance since the last arrow; updated in place.
    void DrawDirectionArrow(ImDrawList& draw_list, const ImVec2& p1, const ImVec2& p2, ImU32 color, float scale, float& accum)
    {
        const float dx = p2.x - p1.x;
        const float dy = p2.y - p1.y;
        const float len = sqrtf(dx * dx + dy * dy);
        if (len < 0.5f) return;

        constexpr float arrow_spacing = 80.f; // screen pixels between arrows
        accum += len;
        if (accum < arrow_spacing) return;
        accum -= arrow_spacing;

        // Unit direction and perpendicular
        const float ux = dx / len, uy = dy / len;
        const float px = -uy, py = ux;

        const float size = std::max(10.f, 5.f * scale);
        const uint32_t src_alpha = (color >> 24) & 0xFF;
        const uint32_t arrow_alpha = std::min(src_alpha, 0xD0u);
        const ImU32 arrow_color = (color & 0x00FFFFFF) | (arrow_alpha << 24);

        const ImVec2 mid = {(p1.x + p2.x) * 0.5f, (p1.y + p2.y) * 0.5f};
        const ImVec2 tip = {mid.x + ux * size, mid.y + uy * size};
        const ImVec2 left = {mid.x - ux * size * 0.5f + px * size * 0.6f, mid.y - uy * size * 0.5f + py * size * 0.6f};
        const ImVec2 right = {mid.x - ux * size * 0.5f - px * size * 0.6f, mid.y - uy * size * 0.5f - py * size * 0.6f};
        draw_list.AddTriangleFilled(tip, left, right, arrow_color);
    }

    // Find closest waypoint index to a screen position. Returns -1 if none within threshold.
    int FindClosestWaypoint(const ImVec2& mouse, const std::vector<GW::Vec2f>& waypoints,
        const std::function<ImVec2(const GW::Vec2f&)>& to_screen, float threshold_sq = 12.f * 12.f)
    {
        int best = -1;
        float best_dist_sq = threshold_sq;
        for (size_t i = 0; i < waypoints.size(); i++) {
            const auto sp = to_screen(waypoints[i]);
            const float dx = mouse.x - sp.x;
            const float dy = mouse.y - sp.y;
            const float d = dx * dx + dy * dy;
            if (d < best_dist_sq) {
                best_dist_sq = d;
                best = static_cast<int>(i);
            }
        }
        return best;
    }

    // Find closest edge index (segment from waypoints[i] to waypoints[i+1]). Returns -1 if none.
    int FindClosestEdge(const ImVec2& mouse, const std::vector<GW::Vec2f>& waypoints, bool loop,
        const std::function<ImVec2(const GW::Vec2f&)>& to_screen, float threshold_sq = 10.f * 10.f)
    {
        int best = -1;
        float best_dist_sq = threshold_sq;
        const size_t n = waypoints.size();
        const size_t edges = loop ? n : (n > 0 ? n - 1 : 0);
        for (size_t i = 0; i < edges; i++) {
            const auto p1 = to_screen(waypoints[i]);
            const auto p2 = to_screen(waypoints[(i + 1) % n]);
            const float d = PointToSegmentDistSq(mouse, p1, p2);
            if (d < best_dist_sq) {
                best_dist_sq = d;
                best = static_cast<int>(i);
            }
        }
        return best;
    }

    std::filesystem::path GetAnnotationsDir()
    {
        return Resources::GetSettingFile(L"map_annotations");
    }

    // Find a user layer by index among user (non-base) layers
    MapAnnotationLayer* GetUserLayerByIndex(size_t idx)
    {
        size_t count = 0;
        for (auto& layer : layers) {
            if (layer.is_base_layer) continue;
            if (count == idx) return &layer;
            count++;
        }
        return nullptr;
    }

    MapAnnotationLayer& GetActiveLayer()
    {
        auto* layer = GetUserLayerByIndex(active_layer_index);
        if (layer) return *layer;
        // Fallback: create default user layer
        active_layer_index = 0;
        // Check if any user layer exists
        for (auto& l : layers) {
            if (!l.is_base_layer) return l;
        }
        MapAnnotationLayer new_layer;
        new_layer.name = "User Annotations";
        new_layer.filename = "user.json";
        new_layer.visible = true;
        layers.push_back(std::move(new_layer));
        return layers.back();
    }

    std::string SanitizeFilename(const std::string& name)
    {
        std::string result;
        for (char c : name) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ' ')
                result += c;
        }
        if (result.empty()) result = "layer";
        // Replace spaces with underscores for filename
        for (char& c : result) {
            if (c == ' ') c = '_';
        }
        return result + ".json";
    }

    uint32_t next_trigger_id = 1;

    // Helpers for glz::generic value extraction with defaults.
    // glaze struct-based serialization fails to compile on x86 + MSVC for float
    // fields (zmij SIMD path uses 64-bit-only intrinsics), so we use glz::generic
    // dynamic JSON like PathingMapData does.
    bool GenericHas(const glz::generic& j, const char* key)
    {
        return j.is_object() && j.contains(key);
    }
    std::string GenericString(const glz::generic& j, const char* key, const std::string& def)
    {
        if (!GenericHas(j, key) || !j.at(key).is_string()) return def;
        return j.at(key).get<std::string>();
    }
    uint32_t GenericUint(const glz::generic& j, const char* key, uint32_t def)
    {
        if (!GenericHas(j, key) || !j.at(key).is_number()) return def;
        return static_cast<uint32_t>(j.at(key).get<double>());
    }
    int GenericInt(const glz::generic& j, const char* key, int def)
    {
        if (!GenericHas(j, key) || !j.at(key).is_number()) return def;
        return static_cast<int>(j.at(key).get<double>());
    }
    float GenericFloat(const glz::generic& j, const char* key, float def)
    {
        if (!GenericHas(j, key) || !j.at(key).is_number()) return def;
        return static_cast<float>(j.at(key).get<double>());
    }
    bool GenericBool(const glz::generic& j, const char* key, bool def)
    {
        if (!GenericHas(j, key) || !j.at(key).is_boolean()) return def;
        return j.at(key).get<bool>();
    }

    glz::generic MarkerToJson(const MapAnnotationMarker& m)
    {
        glz::generic::array_t world_pos;
        world_pos.emplace_back(m.world_pos.x);
        world_pos.emplace_back(m.world_pos.y);

        glz::generic j;
        j["id"] = static_cast<double>(m.id);
        j["world_pos"] = std::move(world_pos);
        j["map_id"] = static_cast<double>(static_cast<uint32_t>(m.map_id));
        j["color"] = static_cast<double>(m.color);
        j["size"] = m.size;
        j["label"] = m.label;
        j["category"] = m.category;
        j["icon"] = m.icon;
        j["visible"] = m.visible;
        j["wiki_enabled"] = m.wiki_enabled;
        if (!m.enc_name.empty()) {
            glz::generic::array_t enc_arr;
            enc_arr.reserve(m.enc_name.size());
            for (const auto c : m.enc_name) {
                enc_arr.emplace_back(static_cast<double>(static_cast<uint16_t>(c)));
            }
            j["enc_name"] = std::move(enc_arr);
        }
        if (!m.spawn_group.empty()) {
            j["spawn_group"] = m.spawn_group;
        }
        if (m.item_model_id) {
            j["item_model_id"] = static_cast<double>(m.item_model_id);
        }
        return j;
    }

    MapAnnotationMarker MarkerFromJson(const glz::generic& j)
    {
        MapAnnotationMarker m;
        m.id = GenericUint(j, "id", 0u);
        if (GenericHas(j, "world_pos") && j.at("world_pos").is_array()) {
            const auto& wp = j.at("world_pos").get_array();
            if (wp.size() == 2 && wp[0].is_number() && wp[1].is_number()) {
                m.world_pos.x = static_cast<float>(wp[0].get<double>());
                m.world_pos.y = static_cast<float>(wp[1].get<double>());
            }
        }
        m.map_id = static_cast<GW::Constants::MapID>(GenericUint(j, "map_id", 0u));
        m.color = GenericUint(j, "color", IM_COL32(255, 0, 0, 255));
        m.size = GenericFloat(j, "size", 8.0f);
        m.label = GenericString(j, "label", "");
        m.category = GenericString(j, "category", "");
        m.icon = GenericString(j, "icon", "");
        m.visible = GenericBool(j, "visible", true);
        m.wiki_enabled = GenericBool(j, "wiki_enabled", true);
        if (GenericHas(j, "enc_name") && j.at("enc_name").is_array()) {
            for (const auto& c : j.at("enc_name").get_array()) {
                if (c.is_number()) m.enc_name.push_back(static_cast<wchar_t>(static_cast<uint16_t>(c.get<double>())));
            }
        }
        m.spawn_group = GenericString(j, "spawn_group", "");
        m.item_model_id = GenericUint(j, "item_model_id", 0u);
        if (m.id >= next_marker_id) next_marker_id = m.id + 1;
        return m;
    }

    glz::generic RouteToJson(const MapAnnotationRoute& r)
    {
        glz::generic::array_t waypoints;
        waypoints.reserve(r.waypoints.size());
        for (const auto& wp : r.waypoints) {
            glz::generic::array_t pt;
            pt.emplace_back(wp.x);
            pt.emplace_back(wp.y);
            waypoints.emplace_back(std::move(pt));
        }

        glz::generic j;
        j["id"] = static_cast<double>(r.id);
        j["map_id"] = static_cast<double>(static_cast<uint32_t>(r.map_id));
        j["color"] = static_cast<double>(r.color);
        j["thickness"] = r.thickness;
        j["label"] = r.label;
        j["show_label"] = r.show_label;
        j["show_direction"] = r.show_direction;
        j["category"] = r.category;
        j["waypoints"] = std::move(waypoints);
        j["visible"] = r.visible;
        j["loop"] = r.loop;
        j["dimmed_by_default"] = r.dimmed_by_default;
        return j;
    }

    MapAnnotationRoute RouteFromJson(const glz::generic& j)
    {
        MapAnnotationRoute r;
        r.id = GenericUint(j, "id", 0u);
        r.map_id = static_cast<GW::Constants::MapID>(GenericUint(j, "map_id", 0u));
        r.color = GenericUint(j, "color", IM_COL32(0, 255, 0, 255));
        r.thickness = GenericFloat(j, "thickness", 2.0f);
        r.label = GenericString(j, "label", "");
        r.category = GenericString(j, "category", "");
        r.show_label = GenericBool(j, "show_label", true);
        r.show_direction = GenericBool(j, "show_direction", false);
        r.visible = GenericBool(j, "visible", true);
        r.loop = GenericBool(j, "loop", false);
        r.dimmed_by_default = GenericBool(j, "dimmed_by_default", false);
        if (GenericHas(j, "waypoints") && j.at("waypoints").is_array()) {
            for (const auto& wp : j.at("waypoints").get_array()) {
                if (!wp.is_array()) continue;
                const auto& arr = wp.get_array();
                if (arr.size() != 2 || !arr[0].is_number() || !arr[1].is_number()) continue;
                r.waypoints.push_back({static_cast<float>(arr[0].get<double>()),
                                       static_cast<float>(arr[1].get<double>())});
            }
        }
        if (r.id >= next_route_id) next_route_id = r.id + 1;
        return r;
    }

    glz::generic TriggerToJson(const MapAnnotationTrigger& t)
    {
        glz::generic::array_t actions;
        actions.reserve(t.actions.size());
        for (const auto& a : t.actions) {
            glz::generic action_j;
            action_j["route_id"] = static_cast<double>(a.route_id);
            action_j["type"] = static_cast<double>(static_cast<int>(a.type));
            actions.emplace_back(std::move(action_j));
        }

        glz::generic::array_t world_pos;
        world_pos.emplace_back(t.world_pos.x);
        world_pos.emplace_back(t.world_pos.y);

        glz::generic j;
        j["id"] = static_cast<double>(t.id);
        j["map_id"] = static_cast<double>(static_cast<uint32_t>(t.map_id));
        j["condition"] = static_cast<double>(static_cast<int>(t.condition));
        j["world_pos"] = std::move(world_pos);
        j["radius"] = t.radius;
        j["spawn_group"] = t.spawn_group;
        j["marker_id"] = static_cast<double>(t.marker_id);
        j["revert_on_leave"] = t.revert_on_leave;
        j["actions"] = std::move(actions);
        j["label"] = t.label;
        return j;
    }

    MapAnnotationTrigger TriggerFromJson(const glz::generic& j)
    {
        MapAnnotationTrigger t;
        t.id = GenericUint(j, "id", 0u);
        t.map_id = static_cast<GW::Constants::MapID>(GenericUint(j, "map_id", 0u));
        t.condition = static_cast<TriggerConditionType>(GenericInt(j, "condition", 0));
        if (GenericHas(j, "world_pos") && j.at("world_pos").is_array()) {
            const auto& wp = j.at("world_pos").get_array();
            if (wp.size() == 2 && wp[0].is_number() && wp[1].is_number()) {
                t.world_pos.x = static_cast<float>(wp[0].get<double>());
                t.world_pos.y = static_cast<float>(wp[1].get<double>());
            }
        }
        t.radius = GenericFloat(j, "radius", 0.f);
        t.spawn_group = GenericString(j, "spawn_group", "");
        t.marker_id = GenericUint(j, "marker_id", 0u);
        t.revert_on_leave = GenericBool(j, "revert_on_leave", false);
        t.label = GenericString(j, "label", "");
        if (GenericHas(j, "actions") && j.at("actions").is_array()) {
            for (const auto& aj : j.at("actions").get_array()) {
                TriggerAction a;
                a.route_id = GenericUint(aj, "route_id", 0u);
                a.type = static_cast<TriggerActionType>(GenericInt(aj, "type", 0));
                t.actions.push_back(a);
            }
        }
        if (t.id >= next_trigger_id) next_trigger_id = t.id + 1;
        return t;
    }

    glz::generic LayerToJson(const MapAnnotationLayer& layer)
    {
        glz::generic::array_t markers;
        markers.reserve(layer.markers.size());
        for (const auto& m : layer.markers) markers.emplace_back(MarkerToJson(m));

        glz::generic::array_t routes;
        routes.reserve(layer.routes.size());
        for (const auto& r : layer.routes) routes.emplace_back(RouteToJson(r));

        glz::generic::array_t spawn_groups;
        spawn_groups.reserve(layer.spawn_groups.size());
        for (const auto& sg : layer.spawn_groups) {
            glz::generic::array_t npcs;
            npcs.reserve(sg.npcs.size());
            for (const auto& npc : sg.npcs) {
                glz::generic npc_j;
                npc_j["label"] = npc.label;
                if (!npc.enc_name.empty()) {
                    glz::generic::array_t enc_arr;
                    enc_arr.reserve(npc.enc_name.size());
                    for (const auto c : npc.enc_name) {
                        enc_arr.emplace_back(static_cast<double>(static_cast<uint16_t>(c)));
                    }
                    npc_j["enc_name"] = std::move(enc_arr);
                }
                npcs.emplace_back(std::move(npc_j));
            }
            glz::generic sg_j;
            sg_j["name"] = sg.name;
            sg_j["map_id"] = static_cast<double>(static_cast<uint32_t>(sg.map_id));
            sg_j["color"] = static_cast<double>(sg.color);
            sg_j["icon"] = sg.icon;
            sg_j["npcs"] = std::move(npcs);
            spawn_groups.emplace_back(std::move(sg_j));
        }

        glz::generic::array_t triggers;
        triggers.reserve(layer.triggers.size());
        for (const auto& t : layer.triggers) triggers.emplace_back(TriggerToJson(t));

        glz::generic j;
        j["name"] = layer.name;
        j["is_base_layer"] = layer.is_base_layer;
        j["visible"] = layer.visible;
        j["markers"] = std::move(markers);
        j["routes"] = std::move(routes);
        j["spawn_groups"] = std::move(spawn_groups);
        j["triggers"] = std::move(triggers);
        return j;
    }

    MapAnnotationLayer LayerFromJson(const glz::generic& j, const std::string& filename)
    {
        MapAnnotationLayer layer;
        layer.name = GenericString(j, "name", "Unnamed Layer");
        layer.filename = filename;
        layer.is_base_layer = GenericBool(j, "is_base_layer", false);
        layer.visible = GenericBool(j, "visible", true);
        if (GenericHas(j, "markers") && j.at("markers").is_array()) {
            for (const auto& mj : j.at("markers").get_array()) {
                layer.markers.push_back(MarkerFromJson(mj));
            }
        }
        if (GenericHas(j, "routes") && j.at("routes").is_array()) {
            for (const auto& rj : j.at("routes").get_array()) {
                layer.routes.push_back(RouteFromJson(rj));
            }
        }
        if (GenericHas(j, "spawn_groups") && j.at("spawn_groups").is_array()) {
            for (const auto& sgj : j.at("spawn_groups").get_array()) {
                SpawnGroupDef sg;
                sg.name = GenericString(sgj, "name", "");
                sg.map_id = static_cast<GW::Constants::MapID>(GenericUint(sgj, "map_id", 0u));
                sg.color = GenericUint(sgj, "color", IM_COL32(200, 50, 200, 220));
                sg.icon = GenericString(sgj, "icon", "");
                if (GenericHas(sgj, "npcs") && sgj.at("npcs").is_array()) {
                    for (const auto& nj : sgj.at("npcs").get_array()) {
                        SpawnGroupNpc npc;
                        npc.label = GenericString(nj, "label", "");
                        if (GenericHas(nj, "enc_name") && nj.at("enc_name").is_array()) {
                            for (const auto& c : nj.at("enc_name").get_array()) {
                                if (c.is_number()) npc.enc_name.push_back(static_cast<wchar_t>(static_cast<uint16_t>(c.get<double>())));
                            }
                        }
                        sg.npcs.push_back(std::move(npc));
                    }
                }
                layer.spawn_groups.push_back(std::move(sg));
            }
        }
        if (GenericHas(j, "triggers") && j.at("triggers").is_array()) {
            for (const auto& tj : j.at("triggers").get_array()) {
                layer.triggers.push_back(TriggerFromJson(tj));
            }
        }
        return layer;
    }

    void SaveLayer(const MapAnnotationLayer& layer)
    {
        const auto dir = GetAnnotationsDir();
        Resources::EnsureFolderExists(dir);
        const auto path = dir / layer.filename;
        std::ofstream file(path);
        if (!file.is_open()) return;
        file << glz::write<glz::opts{.prettify = true}>(LayerToJson(layer)).value_or(std::string{});
    }

    void LoadAllLayers()
    {
        layers.clear();
        next_marker_id = 1;
        next_route_id = 1;

        const auto dir = GetAnnotationsDir();
        if (!std::filesystem::exists(dir)) return;

        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".json") continue;

            std::ifstream file(entry.path());
            if (!file.is_open()) continue;
            std::stringstream ss;
            ss << file.rdbuf();

            glz::generic j;
            if (auto ec = glz::read_json(j, ss.str()); ec) {
                continue; // Skip malformed files
            }
            layers.push_back(LayerFromJson(j, entry.path().filename().string()));
        }

        // Sort: base layers first, then user layers
        std::ranges::stable_sort(layers, [](const MapAnnotationLayer& a, const MapAnnotationLayer& b) {
            return a.is_base_layer > b.is_base_layer;
        });

        // Default active layer to "User Annotations" (user.json)
        size_t user_idx = 0;
        for (auto& layer : layers) {
            if (layer.is_base_layer) continue;
            if (layer.filename == "user.json") {
                active_layer_index = user_idx;
                break;
            }
            user_idx++;
        }
    }

    void SaveAllUserLayers()
    {
        for (const auto& layer : layers) {
            if (!layer.is_base_layer) {
                SaveLayer(layer);
            }
        }
    }

    // Draw a layer combo selector. Returns true if selection was valid.
    bool DrawLayerCombo()
    {
        auto user_names = MapAnnotationsModule::GetUserLayerNames();
        if (user_names.empty()) return false;

        if (active_layer_index >= user_names.size())
            active_layer_index = 0;

        if (ImGui::BeginCombo("Layer", user_names[active_layer_index].c_str())) {
            for (size_t i = 0; i < user_names.size(); i++) {
                const bool selected = (i == active_layer_index);
                if (ImGui::Selectable(user_names[i].c_str(), selected)) {
                    active_layer_index = i;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        return true;
    }
} // namespace

void MapAnnotationsModule::Initialize()
{
    if (initialized) return;
    initialized = true;
    ToolboxWindow::Initialize();

    GW::StoC::RegisterPacketCallback<GW::Packet::StoC::AgentState>(
        &agent_state_hook,
        [](GW::HookStatus*, const GW::Packet::StoC::AgentState* packet) {
            if (packet && (packet->state & 0x10)) {
                dead_agent_ids.insert(packet->agent_id);
            }
        });

    MissionMapWidget::AddContextMenuCallback(&MapAnnotationsModule::WorldMapContextMenuItems);

    Compositor::RegisterImGuiOverlay(nullptr, DrawWorldMapOverlay, 1);
    Compositor::RegisterImGuiOverlay(L"MapWindow", DrawMissionMapOverlay, 1);
}

void MapAnnotationsModule::Terminate()
{
    MissionMapWidget::RemoveContextMenuCallback(&MapAnnotationsModule::WorldMapContextMenuItems);
    GW::StoC::RemoveCallback<GW::Packet::StoC::AgentState>(&agent_state_hook);
    ToolboxWindow::Terminate();
    initialized = false;
}

void MapAnnotationsModule::SignalTerminate()
{
    ToolboxWindow::SignalTerminate();
    if (recording_route) {
        StopRecordingRoute();
    }
    SaveAllUserLayers();
    layers.clear();
    editing_route = false;
    moving_waypoint = false;
    moving_route = nullptr;
    moving_route_layer = nullptr;
    moving_waypoint_idx = -1;
}

void MapAnnotationsModule::Update(float delta)
{
    if (!initialized) return;

    // Invalidate decoded names on language change
    const auto current_language = GW::UI::GetTextLanguage();
    if (current_language != last_language) {
        last_language = current_language;
        pending_enc_decodes.clear();
        pending_spawn_npc_decodes.clear();
        for (auto& layer : layers) {
            for (auto& marker : layer.markers) {
                if (!marker.enc_name.empty()) {
                    marker.decoded_name.clear();
                    marker.decode_pending = false;
                }
            }
            for (auto& sg : layer.spawn_groups) {
                for (auto& npc : sg.npcs) {
                    if (!npc.enc_name.empty()) {
                        npc.decoded_name.clear();
                        npc.decode_pending = false;
                    }
                }
            }
        }
    }

    // Resolve pending enc_name decodes
    for (auto it = pending_enc_decodes.begin(); it != pending_enc_decodes.end();) {
        if (it->decoded.empty()) {
            ++it;
            continue;
        }
        const auto marker_id = it->marker_id;
        const auto name_str = TextUtils::WStringToString(it->decoded);
        for (auto& layer : layers) {
            for (auto& marker : layer.markers) {
                if (marker.id == marker_id) {
                    marker.decoded_name = name_str;
                    marker.decode_pending = false;
                    goto enc_resolved;
                }
            }
        }
        enc_resolved:
        it = pending_enc_decodes.erase(it);
    }

    // Resolve pending English name decodes (for wiki links)
    for (auto it = pending_english_decodes.begin(); it != pending_english_decodes.end();) {
        if (it->decoded.empty()) {
            ++it;
            continue;
        }
        const auto marker_id = it->marker_id;
        const auto name_str = TextUtils::WStringToString(it->decoded);
        for (auto& layer : layers) {
            for (auto& marker : layer.markers) {
                if (marker.id == marker_id) {
                    marker.english_name = name_str;
                    marker.english_decode_pending = false;
                    goto eng_resolved;
                }
            }
        }
        eng_resolved:
        it = pending_english_decodes.erase(it);
    }

    // Resolve pending spawn group NPC name decodes
    for (auto it = pending_spawn_npc_decodes.begin(); it != pending_spawn_npc_decodes.end();) {
        if (it->decoded.empty()) {
            ++it;
            continue;
        }
        if (it->npc) {
            it->npc->decoded_name = TextUtils::WStringToString(it->decoded);
            it->npc->decode_pending = false;
        }
        it = pending_spawn_npc_decodes.erase(it);
    }

    // Clear detection state on map or instance type change
    {
        static GW::Constants::MapID last_detection_map = GW::Constants::MapID::None;
        static GW::Constants::InstanceType last_instance_type = GW::Constants::InstanceType::Loading;
        const auto now_map = GW::Map::GetMapID();
        const auto now_type = GW::Map::GetInstanceType();
        if (now_map != last_detection_map || now_type != last_instance_type) {
            last_detection_map = now_map;
            last_instance_type = now_type;
            dead_agent_ids.clear();
            LoadAllLayers();
            for (auto& layer : layers) {
                for (auto& marker : layer.markers) {
                    marker.detected_agent_id = 0;
                    marker.detected_npc_idx = -1;
                    marker.detected_dead = false;
                    marker.detected_game_pos = {};
                    marker.detected_last_hp = 1.0f;
                    marker.detected_carrier_agent_id = 0;
                    marker.detected_world_pos = {};
                }
                for (auto& trigger : layer.triggers) {
                    trigger.fired = false;
                }
                for (auto& route : layer.routes) {
                    route.display_state = (route.dimmed_by_default && GW::Map::GetInstanceType() == GW::Constants::InstanceType::Explorable)
                        ? RouteDisplayState::Dimmed : RouteDisplayState::Normal;
                }
            }
        }
    }

    // Detect NPCs near marker locations (only in explorable areas)
    if (GW::Map::GetInstanceType() == GW::Constants::InstanceType::Explorable) {
        const auto current_map_id = GW::Map::GetMapID();
        const auto* agents = GW::Agents::GetAgentArray();

        // Collect all markers across all layers, separated by type
        struct SpawnGroupMarkerRef {
            MapAnnotationMarker* marker;
            GW::GamePos game_pos;
            bool have_game_pos;
        };
        // group name -> list of markers + detected agents for that group
        struct SpawnGroupDetection {
            const SpawnGroupDef* def;
            std::vector<SpawnGroupMarkerRef> markers;
            // Detected agents: agent_id, npc_idx, game_pos
            struct DetectedAgent {
                uint32_t agent_id;
                int npc_idx;
                GW::GamePos game_pos;
                bool dead;
            };
            std::vector<DetectedAgent> new_agents; // Newly found, need assignment
            std::vector<uint32_t> claimed_agent_ids; // Already assigned to a marker, skip in scan
        };
        std::unordered_map<std::string, SpawnGroupDetection> spawn_group_detections;

        // Collect non-spawn-group enc_name markers for proximity-based assignment
        struct EncNameMarkerRef {
            MapAnnotationMarker* marker;
            GW::GamePos game_pos;
            bool have_game_pos;
        };
        struct EncNameDetection {
            std::wstring enc_name;
            std::vector<EncNameMarkerRef> markers;
            std::vector<uint32_t> claimed_agent_ids;
        };
        // Use a vector of pairs since wstring isn't great as unordered_map key
        std::vector<EncNameDetection> enc_name_detections;

        for (auto& layer : layers) {
            if (!layer.visible) continue;
            for (auto& marker : layer.markers) {
                if (marker.map_id != current_map_id) continue;
                const bool has_spawn_group = !marker.spawn_group.empty();
                const bool has_enc_name = !marker.enc_name.empty();


                if (has_spawn_group) {
                    const auto* sg = FindSpawnGroupConst(marker.spawn_group, current_map_id);
                    if (sg && !sg->npcs.empty()) {
                        auto& det = spawn_group_detections[marker.spawn_group];
                        det.def = sg;
                        GW::GamePos gp{};
                        const bool ok = WorldMapWidget::WorldMapToGamePos(marker.world_pos, gp);
                        det.markers.push_back({&marker, gp, ok});
                    } else {
                        marker.detected_npc_idx = -1;
                        marker.detected_agent_id = 0;
                        marker.detected_dead = false;
                    }
                } else if (marker.item_model_id) {
                    // Bundle item tracking: find on ground as AgentItem, or carried by an AgentLiving
                    if (marker.detected_agent_id) {
                        const auto* tracked = GW::Agents::GetAgentByID(marker.detected_agent_id);
                        if (tracked && tracked->GetIsItemType()) {
                            // Still on the ground as an item agent
                            WorldMapWidget::GamePosToWorldMap(tracked->pos, marker.detected_world_pos);
                            marker.detected_carrier_agent_id = 0;
                            continue;
                        }
                        if (tracked && tracked->GetIsLivingType()) {
                            // We were tracking the carrier — check they still have it
                            const auto* living = tracked->GetAsAgentLiving();
                            const auto* held = living ? GW::Items::GetItemById(living->weapon_item_id) : nullptr;
                            if (held && held->model_id == marker.item_model_id) {
                                WorldMapWidget::GamePosToWorldMap(tracked->pos, marker.detected_world_pos);
                                marker.detected_carrier_agent_id = tracked->agent_id;
                                continue;
                            }
                            // Carrier dropped the item — fall through to re-scan
                        }
                        // Lost tracking — reset and re-scan below
                        marker.detected_agent_id = 0;
                        marker.detected_carrier_agent_id = 0;
                    }

                    if (agents) {
                        // First: look for the item on the ground
                        for (const auto* agent : *agents) {
                            if (!agent || !agent->GetIsItemType()) continue;
                            const auto* agent_item = agent->GetAsAgentItem();
                            if (!agent_item) continue;
                            const auto* item = GW::Items::GetItemById(agent_item->item_id);
                            if (item && item->model_id == marker.item_model_id) {
                                marker.detected_agent_id = agent->agent_id;
                                marker.detected_carrier_agent_id = 0;
                                WorldMapWidget::GamePosToWorldMap(agent->pos, marker.detected_world_pos);
                                goto bundle_found;
                            }
                        }
                        // Second: look for a living agent carrying this bundle
                        for (const auto* agent : *agents) {
                            if (!agent || !agent->GetIsLivingType()) continue;
                            const auto* living = agent->GetAsAgentLiving();
                            if (!living) continue;
                            const auto* held = GW::Items::GetItemById(living->weapon_item_id);
                            if (held && held->model_id == marker.item_model_id) {
                                marker.detected_agent_id = agent->agent_id;
                                marker.detected_carrier_agent_id = agent->agent_id;
                                WorldMapWidget::GamePosToWorldMap(agent->pos, marker.detected_world_pos);
                                goto bundle_found;
                            }
                        }
                    }
                    bundle_found:;
                } else if (has_enc_name) {
                    // Collect for proximity-based assignment below
                    EncNameDetection* det = nullptr;
                    for (auto& d : enc_name_detections) {
                        if (d.enc_name == marker.enc_name) { det = &d; break; }
                    }
                    if (!det) {
                        enc_name_detections.push_back({marker.enc_name, {}, {}});
                        det = &enc_name_detections.back();
                    }
                    GW::GamePos gp{};
                    const bool ok = WorldMapWidget::WorldMapToGamePos(marker.world_pos, gp);
                    det->markers.push_back({&marker, gp, ok});
                } else {
                    marker.detected_npc_idx = -1;
                    marker.detected_agent_id = 0;
                    marker.detected_dead = false;
                }
            }
        }

        // For each spawn group: find matching agents, then assign each to closest marker
        if (agents) {
            for (auto& [group_name, det] : spawn_group_detections) {
                // First check if already-tracked agents are still valid
                for (auto& mref : det.markers) {
                    auto& marker = *mref.marker;
                    if (marker.detected_dead) {
                        if (marker.detected_agent_id) {
                            const auto* tracked = GW::Agents::GetAgentByID(marker.detected_agent_id);
                            if (tracked && tracked->GetIsLivingType()) {
                                // Verify enc_name still matches before reviving
                                const auto* enc = GW::Agents::GetAgentEncName(tracked);
                                bool name_matches = false;
                                if (enc) {
                                    for (const auto& npc : det.def->npcs) {
                                        if (EncNamesMatch(enc, npc.enc_name)) { name_matches = true; break; }
                                    }
                                }
                                if (name_matches) {
                                    const auto* living = tracked->GetAsAgentLiving();
                                    if (living && !living->GetIsDead() && !living->GetIsDeadByTypeMap() && living->hp > 0.0f) {
                                        marker.detected_dead = false;
                                        marker.detected_game_pos = tracked->pos;
                                        marker.detected_last_hp = living->hp;
                                        WorldMapWidget::GamePosToWorldMap(tracked->pos, marker.detected_world_pos);
                                    }
                                } else {
                                    marker.detected_agent_id = 0;
                                }
                            }
                            if (marker.detected_agent_id)
                                det.claimed_agent_ids.push_back(marker.detected_agent_id);
                        }
                        continue;
                    }
                    if (marker.detected_agent_id) {
                        const auto* tracked = GW::Agents::GetAgentByID(marker.detected_agent_id);
                        if (tracked && tracked->GetIsLivingType()) {
                            const auto* living = tracked->GetAsAgentLiving();
                            if (living && (living->GetIsDead() || living->GetIsDeadByTypeMap() || living->hp <= 0.0f)) {
                                marker.detected_dead = true;
                                det.claimed_agent_ids.push_back(marker.detected_agent_id);
                                continue;
                            }
                            marker.detected_last_hp = living ? living->hp : 1.0f;
                            marker.detected_game_pos = tracked->pos;
                            WorldMapWidget::GamePosToWorldMap(tracked->pos, marker.detected_world_pos);
                            det.claimed_agent_ids.push_back(marker.detected_agent_id);
                        } else {
                            if (dead_agent_ids.contains(marker.detected_agent_id)) {
                                marker.detected_dead = true;
                            }
                            det.claimed_agent_ids.push_back(marker.detected_agent_id);
                            continue;
                        }
                    }
                }

                // Scan agents for new matches (including dead ones)
                for (const auto* agent : *agents) {
                    if (!agent || !agent->GetIsLivingType()) continue;
                    // Skip if this agent is already claimed by a marker in this group
                    bool already_claimed = false;
                    for (const auto id : det.claimed_agent_ids) {
                        if (id == agent->agent_id) { already_claimed = true; break; }
                    }
                    if (already_claimed) continue;

                    const auto* enc = GW::Agents::GetAgentEncName(agent);
                    if (!enc) continue;
                    for (size_t i = 0; i < det.def->npcs.size(); i++) {
                        if (EncNamesMatch(enc, det.def->npcs[i].enc_name)) {
                            const auto* living = agent->GetAsAgentLiving();
                            const bool is_dead = living && living->GetIsDead();
                            det.new_agents.push_back({
                                agent->agent_id, static_cast<int>(i), agent->pos, is_dead
                            });
                            break;
                        }
                    }
                }

                // Clear markers that had no prior detection (or were invalidated above)
                for (auto& mref : det.markers) {
                    if (mref.marker->detected_dead) continue; // Preserve dead state
                    if (mref.marker->detected_agent_id) continue; // Preserve out-of-range frozen state
                    mref.marker->detected_npc_idx = -1;
                }

                // Assign each new agent to the closest unassigned (non-dead) marker
                for (const auto& da : det.new_agents) {
                    float best_dist = FLT_MAX;
                    SpawnGroupMarkerRef* best_mref = nullptr;
                    for (auto& mref : det.markers) {
                        if (mref.marker->detected_agent_id != 0) continue; // already assigned
                        if (mref.marker->detected_dead) continue; // frozen dead
                        if (!mref.have_game_pos) continue;
                        const float dx = da.game_pos.x - mref.game_pos.x;
                        const float dy = da.game_pos.y - mref.game_pos.y;
                        const float dist = dx * dx + dy * dy;
                        if (dist < best_dist) {
                            best_dist = dist;
                            best_mref = &mref;
                        }
                    }
                    if (best_mref) {
                        best_mref->marker->detected_agent_id = da.agent_id;
                        best_mref->marker->detected_npc_idx = da.npc_idx;
                        best_mref->marker->detected_dead = da.dead;
                        WorldMapWidget::GamePosToWorldMap(da.game_pos, best_mref->marker->detected_world_pos);
                    }
                }
            }

            // For each enc_name group: same proximity-based assignment
            for (auto& det : enc_name_detections) {
                // Update already-tracked agents
                for (auto& mref : det.markers) {
                    auto& marker = *mref.marker;
                    if (marker.detected_dead) {
                        if (marker.detected_agent_id) {
                            const auto* tracked = GW::Agents::GetAgentByID(marker.detected_agent_id);
                            if (tracked && tracked->GetIsLivingType()) {
                                // Verify enc_name still matches before reviving
                                const auto* enc = GW::Agents::GetAgentEncName(tracked);
                                if (enc && EncNamesMatch(enc, det.enc_name)) {
                                    const auto* living = tracked->GetAsAgentLiving();
                                    if (living && !living->GetIsDead() && !living->GetIsDeadByTypeMap() && living->hp > 0.0f) {
                                        marker.detected_dead = false;
                                        marker.detected_game_pos = tracked->pos;
                                        marker.detected_last_hp = living->hp;
                                        WorldMapWidget::GamePosToWorldMap(tracked->pos, marker.detected_world_pos);
                                    }
                                } else {
                                    // Name mismatch — drop the association
                                    marker.detected_agent_id = 0;
                                }
                            }
                            if (marker.detected_agent_id)
                                det.claimed_agent_ids.push_back(marker.detected_agent_id);
                        }
                        continue;
                    }
                    if (marker.detected_agent_id) {
                        const auto* tracked = GW::Agents::GetAgentByID(marker.detected_agent_id);
                        if (tracked && tracked->GetIsLivingType()) {
                            const auto* living = tracked->GetAsAgentLiving();
                            if (living && (living->GetIsDead() || living->GetIsDeadByTypeMap() || living->hp <= 0.0f)) {
                                marker.detected_dead = true;
                                det.claimed_agent_ids.push_back(marker.detected_agent_id);
                                continue;
                            }
                            marker.detected_last_hp = living ? living->hp : 1.0f;
                            marker.detected_game_pos = tracked->pos;
                            WorldMapWidget::GamePosToWorldMap(tracked->pos, marker.detected_world_pos);
                            det.claimed_agent_ids.push_back(marker.detected_agent_id);
                        } else {
                            // Agent disappeared — check packet-based death signal
                            if (dead_agent_ids.contains(marker.detected_agent_id)) {
                                marker.detected_dead = true;
                            }
                            det.claimed_agent_ids.push_back(marker.detected_agent_id);
                            continue;
                        }
                    }
                }

                // Scan for new matching agents
                struct NewAgent {
                    uint32_t agent_id;
                    GW::GamePos game_pos;
                    bool dead;
                };
                std::vector<NewAgent> new_agents;
                for (const auto* agent : *agents) {
                    if (!agent || !agent->GetIsLivingType()) continue;
                    bool already_claimed = false;
                    for (const auto id : det.claimed_agent_ids) {
                        if (id == agent->agent_id) { already_claimed = true; break; }
                    }
                    if (already_claimed) continue;
                    const auto* enc = GW::Agents::GetAgentEncName(agent);
                    if (enc && EncNamesMatch(enc, det.enc_name)) {
                        const auto* living = agent->GetAsAgentLiving();
                        new_agents.push_back({
                            agent->agent_id, agent->pos,
                            living && (living->GetIsDead() || living->hp <= 0.0f)
                        });
                    }
                }

                // Assign each new agent to closest unassigned marker
                for (const auto& na : new_agents) {
                    float best_dist = FLT_MAX;
                    EncNameMarkerRef* best_mref = nullptr;
                    for (auto& mref : det.markers) {
                        if (mref.marker->detected_agent_id != 0) continue;
                        if (mref.marker->detected_dead) continue;
                        if (!mref.have_game_pos) continue;
                        const float dx = na.game_pos.x - mref.game_pos.x;
                        const float dy = na.game_pos.y - mref.game_pos.y;
                        const float dist = dx * dx + dy * dy;
                        if (dist < best_dist) {
                            best_dist = dist;
                            best_mref = &mref;
                        }
                    }
                    if (best_mref) {
                        best_mref->marker->detected_agent_id = na.agent_id;
                        best_mref->marker->detected_dead = na.dead;
                        best_mref->marker->detected_last_hp = 1.0f;
                        best_mref->marker->detected_game_pos = na.game_pos;
                        WorldMapWidget::GamePosToWorldMap(na.game_pos, best_mref->marker->detected_world_pos);
                    }
                }
            }
        }
    }

    // Evaluate triggers
    {
        const auto current_map_id = GW::Map::GetMapID();
        GW::Vec2f player_world_pos{};
        const auto* me = GW::Agents::GetControlledCharacter();
        const bool have_player = me && WorldMapWidget::GamePosToWorldMap(me->pos, player_world_pos);

        auto apply_actions = [](const MapAnnotationTrigger& trigger) {
            for (const auto& action : trigger.actions) {
                for (auto& layer : layers) {
                    for (auto& route : layer.routes) {
                        if (route.id != action.route_id) continue;
                        switch (action.type) {
                        case TriggerActionType::Highlight:
                            route.display_state = RouteDisplayState::Highlighted;
                            break;
                        case TriggerActionType::Dim:
                            route.display_state = RouteDisplayState::Dimmed;
                            break;
                        case TriggerActionType::Hide:
                            route.display_state = RouteDisplayState::Hidden;
                            break;
                        }
                    }
                }
            }
        };

        auto revert_actions = [](const MapAnnotationTrigger& trigger) {
            for (const auto& action : trigger.actions) {
                for (auto& layer : layers) {
                    for (auto& route : layer.routes) {
                        if (route.id == action.route_id)
                            route.display_state = (route.dimmed_by_default && GW::Map::GetInstanceType() == GW::Constants::InstanceType::Explorable)
                        ? RouteDisplayState::Dimmed : RouteDisplayState::Normal;
                    }
                }
            }
        };

        for (auto& layer : layers) {
            if (!layer.visible) continue;
            for (auto& trigger : layer.triggers) {
                if (trigger.map_id != current_map_id) continue;

                switch (trigger.condition) {
                case TriggerConditionType::PlayerInZone: {
                    if (!have_player) break;
                    const float dx = player_world_pos.x - trigger.world_pos.x;
                    const float dy = player_world_pos.y - trigger.world_pos.y;
                    const bool in_zone = dx * dx + dy * dy <= trigger.radius * trigger.radius;
                    if (in_zone && !trigger.fired) {
                        trigger.fired = true;
                        apply_actions(trigger);
                    } else if (!in_zone && trigger.fired && trigger.revert_on_leave) {
                        trigger.fired = false;
                        revert_actions(trigger);
                    }
                    break;
                }
                case TriggerConditionType::NpcDetected: {
                    if (trigger.fired) break;
                    for (const auto& other_layer : layers) {
                        for (const auto& marker : other_layer.markers) {
                            bool matches = false;
                            if (trigger.marker_id && marker.id == trigger.marker_id)
                                matches = true;
                            else if (!trigger.spawn_group.empty() && marker.spawn_group == trigger.spawn_group)
                                matches = true;
                            if (matches && marker.detected_agent_id != 0 && !marker.detected_dead) {
                                trigger.fired = true;
                                apply_actions(trigger);
                                goto trigger_done;
                            }
                        }
                    }
                    break;
                }
                case TriggerConditionType::NpcDied: {
                    if (trigger.fired) break;
                    for (const auto& other_layer : layers) {
                        for (const auto& marker : other_layer.markers) {
                            bool matches = false;
                            if (trigger.marker_id && marker.id == trigger.marker_id)
                                matches = true;
                            else if (!trigger.spawn_group.empty() && marker.spawn_group == trigger.spawn_group)
                                matches = true;
                            if (matches && marker.detected_dead) {
                                trigger.fired = true;
                                apply_actions(trigger);
                                goto trigger_done;
                            }
                        }
                    }
                    break;
                }
                }
                trigger_done:;
            }
        }

        // Debug: dump marker/trigger state every 5 seconds
        {
            static clock_t last_dump = 0;
            if (clock() - last_dump > 5 * CLOCKS_PER_SEC) {
                last_dump = clock();
            }
        }
    }

    if (!recording_route) return;

    const auto current_map = GW::Map::GetMapID();

    // Detect map change: save current route segment and start a new one
    if (current_map != recording_map_id && current_map != GW::Constants::MapID::None) {
        // Save the segment from the previous map (if it has enough points)
        if (recording_route_data.waypoints.size() >= 2) {
            auto& layer = GetActiveLayer();
            layer.routes.push_back(std::move(recording_route_data));
            SaveLayer(layer);
        }
        // Start fresh segment for the new map
        recording_route_data = {};
        recording_route_data.id = next_route_id++;
        recording_route_data.map_id = current_map;
        recording_route_data.color = IM_COL32(50, 200, 255, 220);
        recording_route_data.thickness = 2.0f;
        recording_map_id = current_map;
        recording_timer = 0.f;
        return;
    }

    recording_timer += delta;
    if (recording_timer < recording_interval) return;
    recording_timer = 0.f;

    const auto me = GW::Agents::GetControlledCharacter();
    if (!me) return;

    GW::Vec2f world_pos;
    if (!WorldMapWidget::GamePosToWorldMap(me->pos, world_pos)) return;

    // Skip if too close to last waypoint
    if (!recording_route_data.waypoints.empty()) {
        const auto& last = recording_route_data.waypoints.back();
        const float dx = world_pos.x - last.x;
        const float dy = world_pos.y - last.y;
        if (dx * dx + dy * dy < 4.f) return; // ~4 world map units minimum
    }

    recording_route_data.waypoints.push_back(world_pos);
}

void MapAnnotationsModule::LoadSettings(ToolboxIni* ini)
{
    ToolboxWindow::LoadSettings(ini);
    show_context_menu_edit = ini->GetBoolValue(Name(), "show_context_menu_edit", show_context_menu_edit);
    LoadAllLayers();
}

void MapAnnotationsModule::SaveSettings(ToolboxIni* ini)
{
    ToolboxWindow::SaveSettings(ini);
    ini->SetBoolValue(Name(), "show_context_menu_edit", show_context_menu_edit);
    SaveAllUserLayers();
}

void MapAnnotationsModule::DrawSettingsInternal()
{
    ImGui::Text("Annotation Layers:");
    ImGui::Separator();

    if (layers.empty()) {
        ImGui::TextDisabled("No layers loaded. Open the Map Annotations window to manage layers.");
    }

    for (size_t i = 0; i < layers.size(); i++) {
        auto& layer = layers[i];
        ImGui::PushID(static_cast<int>(i));
        bool changed = ImGui::Checkbox(layer.name.c_str(), &layer.visible);
        ImGui::SameLine();
        ImGui::TextDisabled("(%zu markers, %zu routes%s)",
            layer.markers.size(), layer.routes.size(),
            layer.is_base_layer ? ", base" : "");
        if (changed && !layer.is_base_layer) {
            SaveLayer(layer);
        }
        ImGui::PopID();
    }

    ImGui::Separator();
    ImGui::Checkbox("Show edit items in context menus", &show_context_menu_edit);
}

void MapAnnotationsModule::Draw(IDirect3DDevice9*)
{
    if (!visible || !initialized) return;

    ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(Name(), GetVisiblePtr(), GetWinFlags())) {
        return ImGui::End();
    }

    // Layer tabs
    if (ImGui::BeginTabBar("##LayerTabs")) {
        for (size_t li = 0; li < layers.size(); li++) {
            auto& layer = layers[li];
            ImGui::PushID(static_cast<int>(li));

            const auto tab_label = std::format("{}##layer{}", layer.name, li);
            if (ImGui::BeginTabItem(tab_label.c_str())) {
                // Layer controls
                ImGui::Checkbox("Visible", &layer.visible);
                if (layer.is_base_layer) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(base layer - read only)");
                }

                ImGui::Separator();

                // Collect unique map IDs
                std::vector<GW::Constants::MapID> map_ids;
                for (const auto& marker : layer.markers) {
                    if (std::find(map_ids.begin(), map_ids.end(), marker.map_id) == map_ids.end())
                        map_ids.push_back(marker.map_id);
                }
                for (const auto& route : layer.routes) {
                    if (std::find(map_ids.begin(), map_ids.end(), route.map_id) == map_ids.end())
                        map_ids.push_back(route.map_id);
                }

                if (map_ids.empty()) {
                    ImGui::TextDisabled("No annotations in this layer.");
                }

                bool layer_modified = false;
                for (const auto map_id : map_ids) {
                    auto* mn = Resources::GetMapName(map_id);
                    const auto map_label = mn ? mn->string() : std::format("Map {}", static_cast<uint32_t>(map_id));
                    ImGui::PushID(static_cast<int>(map_id));
                    if (ImGui::TreeNode("##map", "%s", map_label.c_str())) {
                        // Markers for this map
                        for (size_t mi = 0; mi < layer.markers.size(); mi++) {
                            auto& marker = layer.markers[mi];
                            if (marker.map_id != map_id) continue;
                            ImGui::PushID(static_cast<int>(mi) + 10000);

                            ImGui::Checkbox("##vis", &marker.visible);
                            ImGui::SameLine();

                            EnsureMarkerDecoded(marker);
                            const auto* dn = GetMarkerDisplayName(marker);
                            const char* display = dn[0] ? dn : "(unnamed)";
                            const bool open = !layer.is_base_layer && ImGui::TreeNode("##details", "%s%s%s",
                                marker.icon.empty() ? "" : marker.icon.c_str(),
                                marker.icon.empty() ? "" : " ",
                                display);

                            if (open) {
                                if (!marker.enc_name.empty()) {
                                    ImGui::TextDisabled("Name: %s (localized)", display);
                                    ImGui::SameLine();
                                    if (ImGui::SmallButton("Clear##enc")) {
                                        marker.enc_name.clear();
                                        marker.decoded_name.clear();
                                        marker.decode_pending = false;
                                        SaveLayer(layer);
                                    }
                                }
                                static char edit_buf[128];
                                strncpy(edit_buf, marker.label.c_str(), sizeof(edit_buf) - 1);
                                edit_buf[sizeof(edit_buf) - 1] = '\0';
                                ImGui::SetNextItemWidth(200.0f);
                                if (ImGui::InputText(marker.enc_name.empty() ? "Label##m" : "Label (fallback)##m", edit_buf, sizeof(edit_buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                                    marker.label = edit_buf;
                                    SaveLayer(layer);
                                }

                                if (DrawIconPicker(marker.icon)) {
                                    SaveLayer(layer);
                                }

                                ImVec4 col = ImGui::ColorConvertU32ToFloat4(marker.color);
                                if (ImGui::ColorEdit4("Color##m", &col.x, ImGuiColorEditFlags_NoInputs)) {
                                    marker.color = ImGui::ColorConvertFloat4ToU32(col);
                                    SaveLayer(layer);
                                }

                                ImGui::SetNextItemWidth(120.0f);
                                if (ImGui::SliderFloat("Size##m", &marker.size, 2.0f, 20.0f, "%.0f")) {
                                    SaveLayer(layer);
                                }
                                if (ImGui::Checkbox("Open Wiki on Click##m", &marker.wiki_enabled)) {
                                    SaveLayer(layer);
                                }

                                // Spawn group assignment
                                {
                                    const char* preview = marker.spawn_group.empty() ? "(none)" : marker.spawn_group.c_str();
                                    if (ImGui::BeginCombo("Spawn Group##m", preview)) {
                                        if (ImGui::Selectable("(none)", marker.spawn_group.empty())) {
                                            marker.spawn_group.clear();
                                            SaveLayer(layer);
                                        }
                                        for (auto& sg : layer.spawn_groups) {
                                            if (ImGui::Selectable(SpawnGroupLabel(sg).c_str(), marker.spawn_group == sg.name)) {
                                                marker.spawn_group = sg.name;
                                                // Auto-add marker's enc_name to the group's NPC list if not already present
                                                if (!marker.enc_name.empty()) {
                                                    bool already_in_group = false;
                                                    for (const auto& npc : sg.npcs) {
                                                        if (npc.enc_name == marker.enc_name) { already_in_group = true; break; }
                                                    }
                                                    if (!already_in_group) {
                                                        SpawnGroupNpc npc;
                                                        npc.enc_name = marker.enc_name;
                                                        sg.npcs.push_back(std::move(npc));
                                                    }
                                                }
                                                SaveLayer(layer);
                                            }
                                        }
                                        ImGui::EndCombo();
                                    }
                                }

                                if (ImGui::BeginCombo("Move to##m", nullptr, ImGuiComboFlags_NoPreview)) {
                                    for (auto& dst : layers) {
                                        if (dst.is_base_layer || &dst == &layer) continue;
                                        if (ImGui::Selectable(dst.name.c_str())) {
                                            dst.markers.push_back(std::move(marker));
                                            layer.markers.erase(layer.markers.begin() + mi);
                                            SaveLayer(layer);
                                            SaveLayer(dst);
                                            layer_modified = true;
                                        }
                                    }
                                    ImGui::EndCombo();
                                }
                                if (layer_modified) {
                                    ImGui::TreePop();
                                    ImGui::PopID();
                                    break;
                                }
                                if (ImGui::SmallButton("Delete##m")) {
                                    layer.markers.erase(layer.markers.begin() + mi);
                                    SaveLayer(layer);
                                    layer_modified = true;
                                    ImGui::TreePop();
                                    ImGui::PopID();
                                    break;
                                }

                                ImGui::TreePop();
                            }
                            ImGui::PopID();
                            if (layer_modified) break;
                        }

                        // Routes for this map
                        if (!layer_modified) {
                            for (size_t ri = 0; ri < layer.routes.size(); ri++) {
                                auto& route = layer.routes[ri];
                                if (route.map_id != map_id) continue;
                                ImGui::PushID(static_cast<int>(ri) + 20000);

                                ImGui::Checkbox("##vis", &route.visible);
                                ImGui::SameLine();

                                const char* rdisplay = route.label.empty() ? "(unnamed route)" : route.label.c_str();
                                const bool open = !layer.is_base_layer && ImGui::TreeNode("##details", ICON_FA_ROUTE " %s (%zu pts)",
                                    rdisplay, route.waypoints.size());

                                if (open) {
                                    static char edit_buf[128];
                                    strncpy(edit_buf, route.label.c_str(), sizeof(edit_buf) - 1);
                                    edit_buf[sizeof(edit_buf) - 1] = '\0';
                                    ImGui::SetNextItemWidth(200.0f);
                                    if (ImGui::InputText("Label##r", edit_buf, sizeof(edit_buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                                        route.label = edit_buf;
                                        SaveLayer(layer);
                                    }

                                    ImVec4 col = ImGui::ColorConvertU32ToFloat4(route.color);
                                    if (ImGui::ColorEdit4("Color##r", &col.x, ImGuiColorEditFlags_NoInputs)) {
                                        route.color = ImGui::ColorConvertFloat4ToU32(col);
                                        SaveLayer(layer);
                                    }

                                    ImGui::SetNextItemWidth(120.0f);
                                    if (ImGui::SliderFloat("Thickness##r", &route.thickness, 1.0f, 8.0f, "%.1f")) {
                                        SaveLayer(layer);
                                    }

                                    if (ImGui::Checkbox("Show Label##r", &route.show_label)) {
                                        SaveLayer(layer);
                                    }
                                    if (ImGui::Checkbox("Show Direction##r", &route.show_direction)) {
                                        SaveLayer(layer);
                                    }
                                    if (route.show_direction) {
                                        ImGui::SameLine();
                                        if (ImGui::Button("Reverse##r")) {
                                            std::ranges::reverse(route.waypoints);
                                            SaveLayer(layer);
                                        }
                                    }
                                    if (ImGui::Checkbox("Loop##r", &route.loop)) {
                                        SaveLayer(layer);
                                    }
                                    if (ImGui::Checkbox("Dimmed by Default##r", &route.dimmed_by_default)) {
                                        SaveLayer(layer);
                                    }

                                    if (ImGui::BeginCombo("Move to##r", nullptr, ImGuiComboFlags_NoPreview)) {
                                        for (auto& dst : layers) {
                                            if (dst.is_base_layer || &dst == &layer) continue;
                                            if (ImGui::Selectable(dst.name.c_str())) {
                                                dst.routes.push_back(std::move(route));
                                                layer.routes.erase(layer.routes.begin() + ri);
                                                SaveLayer(layer);
                                                SaveLayer(dst);
                                                layer_modified = true;
                                            }
                                        }
                                        ImGui::EndCombo();
                                    }
                                    if (layer_modified) {
                                        ImGui::TreePop();
                                        ImGui::PopID();
                                        break;
                                    }
                                    if (ImGui::SmallButton("Delete##r")) {
                                        layer.routes.erase(layer.routes.begin() + ri);
                                        SaveLayer(layer);
                                        layer_modified = true;
                                        ImGui::TreePop();
                                        ImGui::PopID();
                                        break;
                                    }

                                    ImGui::TreePop();
                                }
                                ImGui::PopID();
                                if (layer_modified) break;
                            }
                        }

                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                    if (layer_modified) break;
                }

                // Spawn groups section
                if (!layer.is_base_layer && ImGui::CollapsingHeader("Spawn Groups")) {
                    for (size_t sgi = 0; sgi < layer.spawn_groups.size(); sgi++) {
                        auto& sg = layer.spawn_groups[sgi];
                        ImGui::PushID(static_cast<int>(sgi));
                        {
                            auto* mn = Resources::GetMapName(sg.map_id);
                            const auto sg_tree_label = mn
                                ? std::format("{} ({})", sg.name, mn->string())
                                : std::format("{} (Map {})", sg.name, static_cast<uint32_t>(sg.map_id));
                        if (ImGui::TreeNode(sg_tree_label.c_str())) {
                            // Group settings
                            static char sg_name_buf[64] = {};
                            strncpy(sg_name_buf, sg.name.c_str(), sizeof(sg_name_buf) - 1);
                            ImGui::SetNextItemWidth(200.0f);
                            if (ImGui::InputText("Name##sg", sg_name_buf, sizeof(sg_name_buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                                // Update all markers referencing this group
                                const auto old_name = sg.name;
                                sg.name = sg_name_buf;
                                for (auto& m : layer.markers) {
                                    if (m.spawn_group == old_name) m.spawn_group = sg.name;
                                }
                                SaveLayer(layer);
                            }
                            ImVec4 sgcol = ImGui::ColorConvertU32ToFloat4(sg.color);
                            if (ImGui::ColorEdit4("Color##sg", &sgcol.x, ImGuiColorEditFlags_NoInputs)) {
                                sg.color = ImGui::ColorConvertFloat4ToU32(sgcol);
                                SaveLayer(layer);
                            }
                            {
                                auto* sg_mn = Resources::GetMapName(sg.map_id);
                                const auto map_label = sg_mn ? sg_mn->string() : std::format("Map {}", static_cast<uint32_t>(sg.map_id));
                                ImGui::TextDisabled("Map: %s", map_label.c_str());
                            }
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Set to Current Map##sg")) {
                                sg.map_id = GW::Map::GetMapID();
                                SaveLayer(layer);
                            }

                            // NPC list
                            ImGui::Text("Possible NPCs (%zu):", sg.npcs.size());
                            for (size_t ni = 0; ni < sg.npcs.size(); ni++) {
                                auto& npc = sg.npcs[ni];
                                ImGui::PushID(static_cast<int>(ni));
                                EnsureSpawnNpcDecoded(npc);
                                const auto* npc_display = GetSpawnNpcDisplayName(npc);
                                ImGui::BulletText("%s", npc_display[0] ? npc_display : npc.label.c_str());
                                ImGui::SameLine();
                                if (ImGui::SmallButton("Remove##npc")) {
                                    sg.npcs.erase(sg.npcs.begin() + ni);
                                    SaveLayer(layer);
                                    ImGui::PopID();
                                    break;
                                }
                                ImGui::PopID();
                            }

                            // Add current target as NPC to this group
                            if (ImGui::SmallButton("Add Current Target##sg")) {
                                const auto* target = GW::Agents::GetTarget();
                                if (target) {
                                    const auto* enc = GW::Agents::GetAgentEncName(target);
                                    if (enc && enc[0]) {
                                        // Check for duplicate enc_name
                                        bool duplicate = false;
                                        for (const auto& existing : sg.npcs) {
                                            if (existing.enc_name == enc) { duplicate = true; break; }
                                        }
                                        if (!duplicate) {
                                            SpawnGroupNpc npc;
                                            npc.enc_name = enc;
                                            sg.npcs.push_back(std::move(npc));
                                            if (sg.map_id == GW::Constants::MapID::None) {
                                                sg.map_id = GW::Map::GetMapID();
                                            }
                                            SaveLayer(layer);
                                        }
                                    }
                                }
                            }

                            // Count markers assigned to this group
                            int marker_count = 0;
                            for (const auto& m : layer.markers) {
                                if (m.spawn_group == sg.name) marker_count++;
                            }
                            ImGui::TextDisabled("%d spawn location(s)", marker_count);
                            // Debug: show detection state for each marker in this group
                            for (const auto& m : layer.markers) {
                                if (m.spawn_group != sg.name) continue;
                                ImGui::TextDisabled("  Marker %u: agent=%u npc_idx=%d dead=%d pos=(%.0f,%.0f) vis=%d",
                                    m.id, m.detected_agent_id, m.detected_npc_idx,
                                    m.detected_dead, m.world_pos.x, m.world_pos.y, m.visible);
                            }

                            if (ImGui::SmallButton("Delete Group##sg")) {
                                // Unassign markers
                                for (auto& m : layer.markers) {
                                    if (m.spawn_group == sg.name) m.spawn_group.clear();
                                }
                                layer.spawn_groups.erase(layer.spawn_groups.begin() + sgi);
                                SaveLayer(layer);
                                ImGui::TreePop();
                                ImGui::PopID();
                                break;
                            }

                            ImGui::TreePop();
                        }
                        } // sg_tree_label scope
                        ImGui::PopID();
                    }

                    // New spawn group
                    static char new_sg_buf[64] = {};
                    ImGui::SetNextItemWidth(150.0f);
                    ImGui::InputText("##newsg", new_sg_buf, sizeof(new_sg_buf));
                    ImGui::SameLine();
                    if (ImGui::SmallButton("New Spawn Group") && new_sg_buf[0]) {
                        SpawnGroupDef sg;
                        sg.name = new_sg_buf;
                        sg.map_id = GW::Map::GetMapID();
                        layer.spawn_groups.push_back(std::move(sg));
                        SaveLayer(layer);
                        new_sg_buf[0] = '\0';
                    }
                }

                if (!layer.is_base_layer && ImGui::CollapsingHeader("Triggers")) {
                    // Group triggers by map
                    std::vector<GW::Constants::MapID> trigger_map_ids;
                    for (const auto& trigger : layer.triggers) {
                        if (std::find(trigger_map_ids.begin(), trigger_map_ids.end(), trigger.map_id) == trigger_map_ids.end())
                            trigger_map_ids.push_back(trigger.map_id);
                    }
                    std::sort(trigger_map_ids.begin(), trigger_map_ids.end(), [](GW::Constants::MapID a, GW::Constants::MapID b) {
                        auto* an = Resources::GetMapName(a);
                        auto* bn = Resources::GetMapName(b);
                        std::string as = an ? an->string() : "";
                        std::string bs = bn ? bn->string() : "";
                        if (as != bs) return as < bs;
                        return a < b;
                    });

                    for (const auto map_id : trigger_map_ids) {
                        auto* mn = Resources::GetMapName(map_id);
                        const auto map_name = mn ? mn->string() : std::format("Map {}", static_cast<uint32_t>(map_id));
                        const auto map_label = std::format("{}##trigmap{}", map_name, static_cast<int>(map_id));
                        if (!ImGui::TreeNode(map_label.c_str())) continue;

                    for (size_t ti = 0; ti < layer.triggers.size(); ti++) {
                        auto& trigger = layer.triggers[ti];
                        if (trigger.map_id != map_id) continue;
                        ImGui::PushID(static_cast<int>(ti));

                        const char* condition_names[] = {"Player In Zone", "NPC Detected", "NPC Died"};
                        const char* trigger_label = trigger.label.empty()
                            ? condition_names[static_cast<int>(trigger.condition)]
                            : trigger.label.c_str();

                        if (ImGui::TreeNode(std::format("{}##trigger{}", trigger_label, ti).c_str())) {
                            static char trigger_label_buf[64] = {};
                            strncpy(trigger_label_buf, trigger.label.c_str(), sizeof(trigger_label_buf) - 1);
                            ImGui::SetNextItemWidth(150.0f);
                            if (ImGui::InputText("Label##trig", trigger_label_buf, sizeof(trigger_label_buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                                trigger.label = trigger_label_buf;
                                SaveLayer(layer);
                            }

                            int cond = static_cast<int>(trigger.condition);
                            if (ImGui::Combo("Condition", &cond, condition_names, 3)) {
                                trigger.condition = static_cast<TriggerConditionType>(cond);
                                SaveLayer(layer);
                            }

                            if (trigger.condition == TriggerConditionType::PlayerInZone) {
                                ImGui::DragFloat("Radius", &trigger.radius, 0.1f, 0.f, 100.f);
                                ImGui::Checkbox("Revert on leave", &trigger.revert_on_leave);
                                ImGui::TextDisabled("Pos: (%.1f, %.1f)", trigger.world_pos.x, trigger.world_pos.y);
                            } else {
                                // NPC-based: select spawn group or specific marker
                                const char* target_preview = "(none)";
                                if (trigger.marker_id) {
                                    for (const auto& m : layer.markers) {
                                        if (m.id == trigger.marker_id) {
                                            target_preview = GetMarkerDisplayName(m);
                                            if (!target_preview[0]) target_preview = "(marker)";
                                            break;
                                        }
                                    }
                                } else if (!trigger.spawn_group.empty()) {
                                    target_preview = trigger.spawn_group.c_str();
                                }

                                if (ImGui::BeginCombo("Target##trig", target_preview)) {
                                    if (ImGui::Selectable("(none)", !trigger.marker_id && trigger.spawn_group.empty())) {
                                        trigger.marker_id = 0;
                                        trigger.spawn_group.clear();
                                        SaveLayer(layer);
                                    }
                                    // Spawn groups (same map only)
                                    for (const auto& sg : layer.spawn_groups) {
                                        if (sg.map_id != trigger.map_id) continue;
                                        auto label = std::format("[Group] {}", SpawnGroupLabel(sg));
                                        if (ImGui::Selectable(label.c_str(), trigger.spawn_group == sg.name && !trigger.marker_id)) {
                                            trigger.spawn_group = sg.name;
                                            trigger.marker_id = 0;
                                            SaveLayer(layer);
                                        }
                                    }
                                    // Individual markers with enc_names (same map only)
                                    for (auto& m : layer.markers) {
                                        if (m.enc_name.empty()) continue;
                                        if (m.map_id != trigger.map_id) continue;
                                        EnsureMarkerDecoded(m);
                                        const char* mname = GetMarkerDisplayName(m);
                                        if (!mname[0]) mname = "(unnamed)";
                                        auto label = std::format("[Marker] {}##m{}", mname, m.id);
                                        if (ImGui::Selectable(label.c_str(), trigger.marker_id == m.id)) {
                                            trigger.marker_id = m.id;
                                            trigger.spawn_group.clear();
                                            SaveLayer(layer);
                                        }
                                    }
                                    ImGui::EndCombo();
                                }
                            }

                            // Actions
                            ImGui::TextDisabled("Actions:");
                            for (size_t ai = 0; ai < trigger.actions.size(); ai++) {
                                auto& action = trigger.actions[ai];
                                ImGui::PushID(static_cast<int>(ai));
                                // Find route label
                                const char* route_label = "(unknown)";
                                for (const auto& r : layer.routes) {
                                    if (r.id == action.route_id) {
                                        route_label = r.label.empty() ? "(no label)" : r.label.c_str();
                                        break;
                                    }
                                }
                                const char* action_names[] = {"Highlight", "Dim", "Hide"};
                                int act = static_cast<int>(action.type);
                                ImGui::SetNextItemWidth(80.0f);
                                if (ImGui::Combo("##acttype", &act, action_names, 3)) {
                                    action.type = static_cast<TriggerActionType>(act);
                                    SaveLayer(layer);
                                }
                                ImGui::SameLine();
                                ImGui::Text("%s", route_label);
                                ImGui::SameLine();
                                if (ImGui::SmallButton("X##rmaction")) {
                                    trigger.actions.erase(trigger.actions.begin() + ai);
                                    SaveLayer(layer);
                                    ImGui::PopID();
                                    break;
                                }
                                ImGui::PopID();
                            }

                            // Add action
                            if (ImGui::BeginCombo("Add Action...", nullptr, ImGuiComboFlags_NoPreview)) {
                                for (const auto& r : layer.routes) {
                                    if (r.map_id != trigger.map_id) continue;
                                    const char* rl = r.label.empty() ? "(no label)" : r.label.c_str();
                                    if (ImGui::Selectable(rl)) {
                                        TriggerAction a;
                                        a.route_id = r.id;
                                        a.type = TriggerActionType::Highlight;
                                        trigger.actions.push_back(a);
                                        SaveLayer(layer);
                                    }
                                }
                                ImGui::EndCombo();
                            }

                            if (ImGui::BeginCombo("Move to##trig", nullptr, ImGuiComboFlags_NoPreview)) {
                                for (auto& dst : layers) {
                                    if (dst.is_base_layer || &dst == &layer) continue;
                                    if (ImGui::Selectable(dst.name.c_str())) {
                                        dst.triggers.push_back(std::move(trigger));
                                        layer.triggers.erase(layer.triggers.begin() + ti);
                                        SaveLayer(layer);
                                        SaveLayer(dst);
                                        ImGui::EndCombo();
                                        ImGui::TreePop();
                                        ImGui::PopID();
                                        goto triggers_modified;
                                    }
                                }
                                ImGui::EndCombo();
                            }
                            if (ImGui::SmallButton("Delete Trigger")) {
                                layer.triggers.erase(layer.triggers.begin() + ti);
                                SaveLayer(layer);
                                ImGui::TreePop();
                                ImGui::PopID();
                                break;
                            }
                            ImGui::TreePop();
                        }
                        ImGui::PopID();
                    }

                    ImGui::TreePop(); // map group
                    } // for map_id
                    triggers_modified:

                    // New trigger
                    if (ImGui::SmallButton("New Trigger")) {
                        MapAnnotationTrigger t;
                        t.id = next_trigger_id++;
                        t.map_id = GW::Map::GetMapID();
                        t.condition = TriggerConditionType::PlayerInZone;
                        // Set position to player's current location
                        if (const auto* me = GW::Agents::GetControlledCharacter()) {
                            GW::Vec2f wp;
                            if (WorldMapWidget::GamePosToWorldMap(me->pos, wp))
                                t.world_pos = wp;
                        }
                        t.radius = 5.0f;
                        layer.triggers.push_back(std::move(t));
                        SaveLayer(layer);
                    }
                }

                if (!layer.is_base_layer) {
                    ImGui::Separator();
                    static char rename_buf[64] = {};
                    static size_t renaming_layer = SIZE_MAX;
                    if (renaming_layer == li) {
                        ImGui::SetNextItemWidth(200.0f);
                        if (ImGui::InputText("##rename", rename_buf, sizeof(rename_buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                            if (rename_buf[0]) {
                                const auto old_path = GetAnnotationsDir() / layer.filename;
                                layer.name = rename_buf;
                                layer.filename = SanitizeFilename(rename_buf);
                                std::filesystem::remove(old_path);
                                SaveLayer(layer);
                            }
                            renaming_layer = SIZE_MAX;
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Cancel##rename")) {
                            renaming_layer = SIZE_MAX;
                        }
                    }
                    else {
                        if (ImGui::SmallButton("Rename")) {
                            renaming_layer = li;
                            strncpy(rename_buf, layer.name.c_str(), sizeof(rename_buf) - 1);
                            rename_buf[sizeof(rename_buf) - 1] = '\0';
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Delete Layer")) {
                            ImGui::OpenPopup("ConfirmDeleteLayer");
                        }
                        if (ImGui::BeginPopup("ConfirmDeleteLayer")) {
                            ImGui::Text("Delete layer \"%s\" with %zu markers and %zu routes?",
                                layer.name.c_str(), layer.markers.size(), layer.routes.size());
                            if (ImGui::Button("Yes, delete")) {
                                const auto path = GetAnnotationsDir() / layer.filename;
                                std::filesystem::remove(path);
                                layers.erase(layers.begin() + li);
                                ImGui::EndPopup();
                                ImGui::EndTabItem();
                                ImGui::PopID();
                                ImGui::EndTabBar();
                                ImGui::End();
                                return;
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("Cancel")) {
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::EndPopup();
                        }
                    }
                }

                ImGui::EndTabItem();
            }

            ImGui::PopID();
        }
        ImGui::EndTabBar();
    }

    if (layers.empty()) {
        ImGui::TextDisabled("No layers loaded. Add markers via the world map context menu.");
    }

    ImGui::Separator();
    static char new_layer_buf[64] = {};
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("##newlayer", new_layer_buf, sizeof(new_layer_buf));
    ImGui::SameLine();
    if (ImGui::Button("New Layer") && new_layer_buf[0] != '\0') {
        MapAnnotationLayer new_layer;
        new_layer.name = new_layer_buf;
        new_layer.filename = SanitizeFilename(new_layer_buf);
        new_layer.visible = true;
        SaveLayer(new_layer);
        layers.push_back(std::move(new_layer));
        new_layer_buf[0] = '\0';
    }

    ImGui::End();
}

bool MapAnnotationsModule::IsAgentAnnotated(uint32_t agent_id)
{
    if (!agent_id) return false;
    for (const auto& layer : layers) {
        if (!layer.visible) continue;
        for (const auto& marker : layer.markers) {
            if (marker.detected_agent_id == agent_id) return true;
        }
    }
    return false;
}

static void DrawWorldMapOverlay(ImDrawList& draw_list)
{
    if (!initialized) return;
    const auto world_map_context = GW::Map::GetWorldMapContext();
    if (!world_map_context) return;
    if (world_map_context->zoom != 1.0f && world_map_context->zoom != 0.0f) return;

    const auto viewport = ImGui::GetMainViewport();
    const auto* wm_frame = GW::UI::GetFrameById(world_map_context->frame_id);
    if (!wm_frame) return;

    const auto ui_scale = wm_frame->position.GetViewportScale(GW::UI::GetRootFrame());
    const auto& top_left = world_map_context->top_left;
    const auto viewport_offset = viewport->Pos;

    float world_map_scale = 1.f;
    if (world_map_context->zoom != 1.0f) {
        const GW::Vec2f world_map_size_in_coords = {(float)world_map_context->h004c[5], (float)world_map_context->h004c[6]};
        const GW::Vec2f world_map_zoomed_out_size = {world_map_context->h0030, world_map_context->h0034};
        if (world_map_context->top_left.y == 0.f) {
            world_map_scale = world_map_zoomed_out_size.y / world_map_size_in_coords.y;
        }
        else {
            world_map_scale = world_map_zoomed_out_size.x / world_map_size_in_coords.x;
        }
    }

    auto to_screen = [&](const GW::Vec2f& world_pos) -> ImVec2 {
        return {
            ui_scale.x * world_map_scale * (world_pos.x - top_left.x) + viewport_offset.x,
            ui_scale.y * world_map_scale * (world_pos.y - top_left.y) + viewport_offset.y
        };
    };

    hovered_marker = nullptr;
    hovered_marker_layer = nullptr;
    hovered_route = nullptr;
    hovered_route_layer = nullptr;
    const bool mouse_held = ImGui::IsMouseDown(ImGuiMouseButton_Right);

    const bool zoomed_in = world_map_context->zoom == 1.0f;
    const auto visible_continent = world_map_context->continent;
    auto is_on_continent = [&](GW::Constants::MapID map_id) -> bool {
        if (map_id == GW::Constants::MapID::None) return true;
        const auto info = GW::Map::GetMapInfo(map_id);
        return info && info->continent == visible_continent;
    };

    const auto mouse = ImGui::GetMousePos();
    constexpr float route_hover_dist_sq = 8.f * 8.f; // 8px hover tolerance

    for (auto& layer : layers) {
        if (!layer.visible) continue;
        if (!zoomed_in) continue;

        // Draw routes + hover detection
        for (auto& route : layer.routes) {
            if (!route.visible || route.display_state == RouteDisplayState::Hidden) continue;
            if (route.waypoints.size() < 2) continue;
            if (!is_on_continent(route.map_id)) continue;

            const ImU32 route_draw_color = ApplyDisplayState(route.color, route.display_state);
            float route_draw_thickness = route.thickness;
            if (route.display_state == RouteDisplayState::Highlighted) route_draw_thickness *= 1.5f;

            bool route_hovered = false;
            float arrow_accum = 0.f;
            for (size_t i = 0; i < route.waypoints.size() - 1; i++) {
                const auto p1 = to_screen(route.waypoints[i]);
                const auto p2 = to_screen(route.waypoints[i + 1]);
                draw_list.AddLine(p1, p2, route_draw_color, route_draw_thickness);
                if (route.show_direction) {
                    DrawDirectionArrow(draw_list, p1, p2, route_draw_color, ui_scale.x, arrow_accum);
                }
                if (!route_hovered
                    && PointToSegmentDistSq(mouse, p1, p2) <= route_hover_dist_sq) {
                    route_hovered = true;
                }
            }
            if (route.loop && route.waypoints.size() > 2) {
                const auto p1 = to_screen(route.waypoints.back());
                const auto p2 = to_screen(route.waypoints.front());
                draw_list.AddLine(p1, p2, route_draw_color, route_draw_thickness);
                if (route.show_direction) {
                    DrawDirectionArrow(draw_list, p1, p2, route_draw_color, ui_scale.x, arrow_accum);
                }
                if (!route_hovered
                    && PointToSegmentDistSq(mouse, p1, p2) <= route_hover_dist_sq) {
                    route_hovered = true;
                }
            }

            if (route.show_label && !route.label.empty() && !route.waypoints.empty()) {
                const auto label_pos = to_screen(route.waypoints[0]);
                const ImVec2 text_pos = {label_pos.x + 6.f, label_pos.y - 6.f};
                const auto text_size = ImGui::CalcTextSize(route.label.c_str());
                draw_list.AddRectFilled({text_pos.x - 2.f, text_pos.y - 1.f}, {text_pos.x + text_size.x + 2.f, text_pos.y + text_size.y + 1.f}, IM_COL32(0, 0, 0, 180));
                draw_list.AddText(text_pos, route.color, route.label.c_str());
            }

            if (route_hovered && show_context_menu_edit && !mouse_held) {
                hovered_route = &route;
                hovered_route_layer = &layer;

                if (!layer.is_base_layer && zoomed_in) {
                    // Draw waypoint nodes and find closest node/edge
                    hovered_waypoint_idx = FindClosestWaypoint(mouse, route.waypoints, to_screen);
                    hovered_edge_idx = FindClosestEdge(mouse, route.waypoints, route.loop, to_screen);

                    for (size_t wi = 0; wi < route.waypoints.size(); wi++) {
                        const float node_radius = 4.0f * ui_scale.x;
                        const bool is_hovered_node = (static_cast<int>(wi) == hovered_waypoint_idx);
                        const auto wp_screen = to_screen(route.waypoints[wi]);
                        draw_list.AddCircleFilled(wp_screen, node_radius, is_hovered_node ? IM_COL32(255, 255, 0, 255) : IM_COL32(255, 255, 255, 200));
                        draw_list.AddCircle(wp_screen, node_radius, IM_COL32(0, 0, 0, 200), 0, 1.0f);
                    }

                    if (hovered_waypoint_idx >= 0) {
                        ImGui::SetTooltip("Node %d (right-click to edit)", hovered_waypoint_idx);
                    }
                    else {
                        ImGui::SetTooltip("%s (right-click to edit)", route.label.empty() ? "Route" : route.label.c_str());
                    }
                }
                else {
                    hovered_waypoint_idx = -1;
                    hovered_edge_idx = -1;
                    ImGui::SetTooltip("%s", route.label.empty() ? "Annotation Route" : route.label.c_str());
                }
            }
        }

        // Draw markers + hover detection (markers take priority over routes)
        for (auto& marker : layer.markers) {
            if (!marker.visible) continue;
            if (!is_on_continent(marker.map_id)) continue;

            // Resolve spawn group display properties
            const SpawnGroupDef* sg = nullptr;
            if (!marker.spawn_group.empty()) {
                sg = FindSpawnGroupConst(marker.spawn_group, marker.map_id);
            }

            // Use detected NPC position if available, else static spawn pos
            const bool has_detection = marker.detected_agent_id != 0;
            const auto& draw_pos = has_detection ? marker.detected_world_pos : marker.world_pos;
            const auto screen_pos = to_screen(draw_pos);


            const auto use_color = sg ? sg->color : marker.color;
            const auto& use_icon = sg && !sg->icon.empty() ? sg->icon : marker.icon;
            const float radius = marker.size * ui_scale.x;
            float hit_radius = radius + 4.f;

            if (!use_icon.empty()) {
                const auto icon_size = ImGui::CalcTextSize(use_icon.c_str());
                const ImVec2 icon_pos = {screen_pos.x - icon_size.x * 0.5f, screen_pos.y - icon_size.y * 0.5f};
                draw_list.AddRectFilled(
                    {icon_pos.x - 2.f, icon_pos.y - 1.f},
                    {icon_pos.x + icon_size.x + 2.f, icon_pos.y + icon_size.y + 1.f},
                    IM_COL32(0, 0, 0, 180));
                draw_list.AddText(icon_pos, use_color, use_icon.c_str());
                hit_radius = std::max(icon_size.x, icon_size.y) * 0.5f + 4.f;
            }
            else {
                draw_list.AddCircleFilled(screen_pos, radius, use_color);
                draw_list.AddCircle(screen_pos, radius, IM_COL32(0, 0, 0, 180), 0, 1.0f);
            }

            // Determine display name: detected NPC > decoded enc_name > group name > label
            const char* display_name = "";
            if (sg && marker.detected_npc_idx >= 0 && marker.detected_npc_idx < static_cast<int>(sg->npcs.size())) {
                auto& npc = sg->npcs[marker.detected_npc_idx];
                EnsureSpawnNpcDecoded(npc);
                display_name = GetSpawnNpcDisplayName(npc);
            } else if (sg) {
                display_name = sg->name.c_str();
            } else {
                EnsureMarkerDecoded(marker);
                display_name = GetMarkerDisplayName(marker);
            }


            bool label_hovered = false;
            float label_right_x = screen_pos.x + hit_radius;
            if (display_name[0]) {
                const ImVec2 text_pos = {screen_pos.x + hit_radius, screen_pos.y - 6.f};
                const auto text_size = ImGui::CalcTextSize(display_name);
                const ImVec2 label_min = {text_pos.x - 2.f, text_pos.y - 1.f};
                const ImVec2 label_max = {text_pos.x + text_size.x + 2.f, text_pos.y + text_size.y + 1.f};
                label_right_x = label_max.x;
                draw_list.AddRectFilled(label_min, label_max, IM_COL32(0, 0, 0, 180));
                draw_list.AddText(text_pos, use_color, display_name);
                if (marker.detected_dead) {
                    const float strike_y = text_pos.y + text_size.y * 0.5f;
                    draw_list.AddLine({text_pos.x, strike_y}, {text_pos.x + text_size.x, strike_y}, use_color, 1.0f);
                }
                label_hovered = mouse.x >= label_min.x && mouse.x <= label_max.x
                    && mouse.y >= label_min.y && mouse.y <= label_max.y;
            }

            // Health bar for detected living agents (not for bundle items)
            if (has_detection && !marker.detected_dead && !marker.item_model_id) {
                const auto* agent = GW::Agents::GetAgentByID(marker.detected_agent_id);
                if (agent && agent->GetIsLivingType()) {
                    const auto* living = agent->GetAsAgentLiving();
                    if (living && living->hp >= 0.f) {
                        const float bar_h = 3.f * ui_scale.x;
                        const float bar_y = screen_pos.y + hit_radius + 1.f;
                        const float bar_left = screen_pos.x - hit_radius;
                        const float bar_w = label_right_x - bar_left;
                        const ImVec2 bar_min = {bar_left, bar_y};
                        const ImVec2 bar_max = {label_right_x, bar_y + bar_h};
                        const ImVec2 fill_max = {bar_left + bar_w * living->hp, bar_y + bar_h};
                        draw_list.AddRectFilled(bar_min, bar_max, IM_COL32(0, 0, 0, 160));
                        const ImU32 hp_color = living->hp > 0.5f
                            ? IM_COL32(0, 200, 0, 200)
                            : (living->hp > 0.25f ? IM_COL32(200, 200, 0, 200) : IM_COL32(200, 0, 0, 200));
                        draw_list.AddRectFilled(bar_min, fill_max, hp_color);
                        draw_list.AddRect(bar_min, bar_max, IM_COL32(0, 0, 0, 200));
                    }
                }
            }

            const float dx = mouse.x - screen_pos.x;
            const float dy = mouse.y - screen_pos.y;
            if (!mouse_held && (label_hovered || dx * dx + dy * dy <= hit_radius * hit_radius)) {
                hovered_marker = &marker;
                hovered_marker_layer = &layer;

                // Build tooltip
                if (sg) {
                    std::string tip = sg->name + "\nPossible NPCs:";
                    for (size_t ni = 0; ni < sg->npcs.size(); ni++) {
                        auto& npc = sg->npcs[ni];
                        EnsureSpawnNpcDecoded(npc);
                        const auto* npc_name = GetSpawnNpcDisplayName(npc);
                        tip += "\n  ";
                        if (marker.detected_npc_idx == static_cast<int>(ni)) tip += "> ";
                        tip += npc_name[0] ? npc_name : "(unnamed)";
                    }
                    if (marker.detected_agent_id) tip += "\n(click to select, shift+click to flag heroes)";
                    else tip += "\n(shift+click to flag heroes)";
                    ImGui::SetTooltip("%s", tip.c_str());
                } else {
                    const char* name = display_name[0] ? display_name : "Map Annotation";
                    if (marker.detected_carrier_agent_id) {
                        ImGui::SetTooltip("%s\n(click to select carrier, shift+click to flag heroes)", name);
                    } else if (marker.detected_agent_id) {
                        ImGui::SetTooltip("%s\n(click to select, shift+click to flag heroes)", name);
                    } else {
                        ImGui::SetTooltip("%s\n(shift+click to flag heroes)", name);
                    }
                }
                if (ImGui::IsMouseClicked(0)) {
                    const bool shift = ImGui::GetIO().KeyShift;
                    if (shift) {
                        // Flag heroes to marker position (only in explorable, only on current map)
                        if (GW::Map::GetInstanceType() == GW::Constants::InstanceType::Explorable
                            && (marker.map_id == GW::Constants::MapID::None || marker.map_id == GW::Map::GetMapID())) {
                            const auto& flag_pos = has_detection ? marker.detected_world_pos : marker.world_pos;
                            GW::GamePos game_pos{};
                            if (WorldMapWidget::WorldMapToGamePos(flag_pos, game_pos)) {
                                GW::GameThread::Enqueue([game_pos] {
                                    GW::PartyMgr::FlagAll(game_pos);
                                });
                            }
                        }
                    } else {
                        // Click: select detected agent
                        const auto agent_id = marker.detected_carrier_agent_id ? marker.detected_carrier_agent_id : marker.detected_agent_id;
                        GW::GameThread::Enqueue([agent_id] {
                            if (GW::Agents::GetAgentByID(agent_id)) {
                                GW::Agents::ChangeTarget(agent_id);
                            }
                        });
                    }
                }
            }
        }
    }

    // Draw trigger zones in edit mode
    if (show_context_menu_edit && zoomed_in) {
        for (const auto& layer : layers) {
            if (!layer.visible) continue;
            for (const auto& trigger : layer.triggers) {
                if (!is_on_continent(trigger.map_id)) continue;
                if (trigger.condition != TriggerConditionType::PlayerInZone) continue;
                if (trigger.radius <= 0.f) continue;

                const auto center = to_screen(trigger.world_pos);
                const auto edge_screen = to_screen({trigger.world_pos.x + trigger.radius, trigger.world_pos.y});
                const float screen_radius = std::abs(edge_screen.x - center.x);

                const ImU32 zone_color = trigger.fired
                    ? IM_COL32(100, 255, 100, 40)
                    : IM_COL32(255, 200, 50, 40);
                const ImU32 border_color = trigger.fired
                    ? IM_COL32(100, 255, 100, 120)
                    : IM_COL32(255, 200, 50, 120);

                draw_list.AddCircleFilled(center, screen_radius, zone_color, 32);
                draw_list.AddCircle(center, screen_radius, border_color, 32, 1.5f);

                if (!trigger.label.empty()) {
                    const auto label_size = ImGui::CalcTextSize(trigger.label.c_str());
                    draw_list.AddText(
                        {center.x - label_size.x * 0.5f, center.y - label_size.y * 0.5f},
                        border_color, trigger.label.c_str());
                }
            }
        }
    }

    // Draw pending route being edited
    if (editing_route && !pending_route.waypoints.empty()) {
        for (size_t i = 0; i < pending_route.waypoints.size() - 1; i++) {
            const auto p1 = to_screen(pending_route.waypoints[i]);
            const auto p2 = to_screen(pending_route.waypoints[i + 1]);
            draw_list.AddLine(p1, p2, pending_route.color, pending_route.thickness);
        }
        for (const auto& wp : pending_route.waypoints) {
            const auto sp = to_screen(wp);
            draw_list.AddCircleFilled(sp, 4.0f * ui_scale.x, pending_route.color);
        }
    }

    // Draw route being recorded
    if (recording_route && recording_route_data.waypoints.size() >= 2) {
        for (size_t i = 0; i < recording_route_data.waypoints.size() - 1; i++) {
            const auto p1 = to_screen(recording_route_data.waypoints[i]);
            const auto p2 = to_screen(recording_route_data.waypoints[i + 1]);
            draw_list.AddLine(p1, p2, recording_route_data.color, recording_route_data.thickness);
        }
    }

    // Show "move node" indicator
    if (moving_waypoint && moving_route && moving_waypoint_idx >= 0 &&
        moving_waypoint_idx < static_cast<int>(moving_route->waypoints.size())) {
        // Draw the node being moved as a pulsing circle at its current position
        const auto node_screen = to_screen(moving_route->waypoints[moving_waypoint_idx]);
        draw_list.AddCircleFilled(node_screen, 6.0f * ui_scale.x, IM_COL32(255, 255, 0, 180));
        draw_list.AddCircle(node_screen, 6.0f * ui_scale.x, IM_COL32(255, 255, 0, 255), 0, 2.0f);
        // Draw a line from the node to the cursor
        draw_list.AddLine(node_screen, mouse, IM_COL32(255, 255, 0, 150), 1.0f);
    }
}

static void DrawMissionMapOverlay(ImDrawList& draw_list)
{
    if (!initialized) return;
    const auto mission_map_context = GW::Map::GetMissionMapContext();
    if (!mission_map_context) return;
    const auto gameplay_context = GW::GetGameplayContext();
    const auto mission_map_frame = mission_map_context ? GW::UI::GetFrameById(mission_map_context->frame_id) : nullptr;
    if (!(gameplay_context && mission_map_frame && mission_map_frame->IsVisible())) return;

    const auto root = GW::UI::GetRootFrame();
    const auto mm_top_left = mission_map_frame->position.GetContentTopLeft(root);
    const auto mm_bottom_right = mission_map_frame->position.GetContentBottomRight(root);
    const auto mm_scale = mission_map_frame->position.GetViewportScale(root);
    const float mm_zoom = gameplay_context->mission_map_zoom;
    const auto mm_center_pos = mm_top_left + (mm_bottom_right - mm_top_left) / 2;
    const auto mm_pan_offset = mission_map_context->h003c->mission_map_pan_offset;

    // On underground maps (DoA, Gate of Madness, etc.) the mission map's pan_offset
    // is not in world map coordinates. Compute a correction by comparing where the
    // player's world map position would land vs where the mission map actually places them.
    const auto mm_player_pos = mission_map_context->h003c->player_mission_map_pos;
    const auto* me = GW::Agents::GetControlledCharacter();
    GW::Vec2f player_world_pos{};
    const bool have_player_world = me && WorldMapWidget::GamePosToWorldMap(me->pos, player_world_pos);
    // Effective pan offset in world map coordinates: adjusted so that the player's
    // world map position maps to the same screen point as their mission map position.
    const auto effective_pan_offset = have_player_world
        ? mm_pan_offset - mm_player_pos + player_world_pos
        : mm_pan_offset;

    auto world_to_mm_screen = [&](const GW::Vec2f& world_pos) -> ImVec2 {
        const auto offset = world_pos - effective_pan_offset;
        const auto scaled = GW::Vec2f(offset.x * mm_scale.x, offset.y * mm_scale.y);
        const auto result = scaled * mm_zoom + mm_center_pos;
        return {result.x, result.y};
    };

    auto is_on_mm = [&](const ImVec2& p) -> bool {
        return p.x >= mm_top_left.x && p.x <= mm_bottom_right.x
            && p.y >= mm_top_left.y && p.y <= mm_bottom_right.y;
    };

    mm_hovered_marker = nullptr;
    mm_hovered_marker_layer = nullptr;
    mm_hovered_route = nullptr;
    mm_hovered_route_layer = nullptr;
    const bool mouse_held = ImGui::IsMouseDown(ImGuiMouseButton_Right);

    const auto current_map_id = GW::Map::GetMapID();
    const auto current_info = GW::Map::GetMapInfo(current_map_id);
    auto is_nearby_map = [&](GW::Constants::MapID map_id) -> bool {
        if (map_id == GW::Constants::MapID::None || map_id == current_map_id) return true;
        const auto info = GW::Map::GetMapInfo(map_id);
        return info && current_info && info->region == current_info->region;
    };

    const auto mouse = ImGui::GetMousePos();
    constexpr float route_hover_dist_sq = 8.f * 8.f;

    for (auto& layer : layers) {
        if (!layer.visible) continue;

        for (auto& route : layer.routes) {
            if (!route.visible || route.display_state == RouteDisplayState::Hidden) continue;
            if (route.waypoints.size() < 2) continue;
            if (!is_nearby_map(route.map_id)) continue;

            const ImU32 mm_route_color = ApplyDisplayState(route.color, route.display_state);
            float mm_route_thickness = route.thickness;
            if (route.display_state == RouteDisplayState::Highlighted) mm_route_thickness *= 1.5f;

            bool route_hovered = false;
            float mm_arrow_accum = 0.f;
            for (size_t i = 0; i < route.waypoints.size() - 1; i++) {
                const auto p1 = world_to_mm_screen(route.waypoints[i]);
                const auto p2 = world_to_mm_screen(route.waypoints[i + 1]);
                draw_list.AddLine(p1, p2, mm_route_color, mm_route_thickness);
                if (route.show_direction) {
                    DrawDirectionArrow(draw_list, p1, p2, mm_route_color, mm_scale.x, mm_arrow_accum);
                }
                if (!route_hovered && is_on_mm(mouse) && PointToSegmentDistSq(mouse, p1, p2) <= route_hover_dist_sq) {
                    route_hovered = true;
                }
            }
            if (route.loop && route.waypoints.size() > 2) {
                const auto p1 = world_to_mm_screen(route.waypoints.back());
                const auto p2 = world_to_mm_screen(route.waypoints.front());
                draw_list.AddLine(p1, p2, mm_route_color, mm_route_thickness);
                if (route.show_direction) {
                    DrawDirectionArrow(draw_list, p1, p2, mm_route_color, mm_scale.x, mm_arrow_accum);
                }
                if (!route_hovered && is_on_mm(mouse) && PointToSegmentDistSq(mouse, p1, p2) <= route_hover_dist_sq) {
                    route_hovered = true;
                }
            }

            if (route.show_label && !route.label.empty() && !route.waypoints.empty()) {
                const auto label_pos = world_to_mm_screen(route.waypoints[0]);
                const ImVec2 text_pos = {label_pos.x + 6.f, label_pos.y - 6.f};
                const auto text_size = ImGui::CalcTextSize(route.label.c_str());
                draw_list.AddRectFilled({text_pos.x - 2.f, text_pos.y - 1.f}, {text_pos.x + text_size.x + 2.f, text_pos.y + text_size.y + 1.f}, IM_COL32(0, 0, 0, 180));
                draw_list.AddText(text_pos, route.color, route.label.c_str());
            }

            if (route_hovered && show_context_menu_edit && !mouse_held) {
                mm_hovered_route = &route;
                mm_hovered_route_layer = &layer;

                if (!layer.is_base_layer) {
                    mm_hovered_waypoint_idx = FindClosestWaypoint(mouse, route.waypoints, world_to_mm_screen);
                    mm_hovered_edge_idx = FindClosestEdge(mouse, route.waypoints, route.loop, world_to_mm_screen);

                    for (size_t wi = 0; wi < route.waypoints.size(); wi++) {
                        const auto wp_screen = world_to_mm_screen(route.waypoints[wi]);
                        const float node_radius = 4.0f * mm_scale.x;
                        const bool is_hovered_node = (static_cast<int>(wi) == mm_hovered_waypoint_idx);
                        draw_list.AddCircleFilled(wp_screen, node_radius, is_hovered_node ? IM_COL32(255, 255, 0, 255) : IM_COL32(255, 255, 255, 200));
                        draw_list.AddCircle(wp_screen, node_radius, IM_COL32(0, 0, 0, 200), 0, 1.0f);
                    }

                    if (mm_hovered_waypoint_idx >= 0) {
                        ImGui::SetTooltip("Node %d (right-click to edit)", mm_hovered_waypoint_idx);
                    }
                    else {
                        ImGui::SetTooltip("%s (right-click to edit)", route.label.empty() ? "Route" : route.label.c_str());
                    }
                }
                else {
                    mm_hovered_waypoint_idx = -1;
                    mm_hovered_edge_idx = -1;
                    ImGui::SetTooltip("%s", route.label.empty() ? "Annotation Route" : route.label.c_str());
                }
            }
        }

        for (auto& marker : layer.markers) {
            if (!marker.visible) continue;
            if (!is_nearby_map(marker.map_id)) continue;

            const SpawnGroupDef* sg = nullptr;
            if (!marker.spawn_group.empty()) {
                sg = FindSpawnGroupConst(marker.spawn_group, marker.map_id);
            }

            const bool has_detection = marker.detected_agent_id != 0;
            const auto& draw_pos = has_detection ? marker.detected_world_pos : marker.world_pos;
            const auto screen_pos = world_to_mm_screen(draw_pos);

            const auto base_color = sg ? sg->color : marker.color;
            const auto use_color = marker.detected_dead
                ? (base_color & 0x00FFFFFF) | (0x50 << 24)
                : base_color;
            const auto& use_icon = sg && !sg->icon.empty() ? sg->icon : marker.icon;
            const float radius = marker.size * mm_scale.x;
            float hit_radius = radius + 4.f;

            if (!use_icon.empty()) {
                const auto icon_size = ImGui::CalcTextSize(use_icon.c_str());
                const ImVec2 icon_pos = {screen_pos.x - icon_size.x * 0.5f, screen_pos.y - icon_size.y * 0.5f};
                draw_list.AddRectFilled(
                    {icon_pos.x - 2.f, icon_pos.y - 1.f},
                    {icon_pos.x + icon_size.x + 2.f, icon_pos.y + icon_size.y + 1.f},
                    IM_COL32(0, 0, 0, 180));
                draw_list.AddText(icon_pos, use_color, use_icon.c_str());
                hit_radius = std::max(icon_size.x, icon_size.y) * 0.5f + 4.f;
            }
            else {
                draw_list.AddCircleFilled(screen_pos, radius, use_color);
                draw_list.AddCircle(screen_pos, radius, IM_COL32(0, 0, 0, 180), 0, 1.0f);
            }

            const char* display_name = "";
            if (sg && marker.detected_npc_idx >= 0 && marker.detected_npc_idx < static_cast<int>(sg->npcs.size())) {
                auto& npc = sg->npcs[marker.detected_npc_idx];
                EnsureSpawnNpcDecoded(npc);
                display_name = GetSpawnNpcDisplayName(npc);
            } else if (sg) {
                display_name = sg->name.c_str();
            } else {
                EnsureMarkerDecoded(marker);
                display_name = GetMarkerDisplayName(marker);
            }


            bool label_hovered = false;
            float label_right_x = screen_pos.x + hit_radius;
            if (display_name[0]) {
                const ImVec2 text_pos = {screen_pos.x + hit_radius, screen_pos.y - 6.f};
                const auto text_size = ImGui::CalcTextSize(display_name);
                const ImVec2 label_min = {text_pos.x - 2.f, text_pos.y - 1.f};
                const ImVec2 label_max = {text_pos.x + text_size.x + 2.f, text_pos.y + text_size.y + 1.f};
                label_right_x = label_max.x;
                draw_list.AddRectFilled(label_min, label_max, IM_COL32(0, 0, 0, 180));
                draw_list.AddText(text_pos, use_color, display_name);
                if (marker.detected_dead) {
                    const float strike_y = text_pos.y + text_size.y * 0.5f;
                    draw_list.AddLine({text_pos.x, strike_y}, {text_pos.x + text_size.x, strike_y}, use_color, 1.0f);
                }
                label_hovered = mouse.x >= label_min.x && mouse.x <= label_max.x
                    && mouse.y >= label_min.y && mouse.y <= label_max.y;
            }

            // Health bar for detected living agents (not for bundle items)
            if (has_detection && !marker.detected_dead && !marker.item_model_id) {
                const auto* agent = GW::Agents::GetAgentByID(marker.detected_agent_id);
                if (agent && agent->GetIsLivingType()) {
                    const auto* living = agent->GetAsAgentLiving();
                    if (living && living->hp >= 0.f) {
                        const float bar_h = 3.f * mm_scale.x;
                        const float bar_y = screen_pos.y + hit_radius + 1.f;
                        const float bar_left = screen_pos.x - hit_radius;
                        const float bar_w = label_right_x - bar_left;
                        const ImVec2 bar_min = {bar_left, bar_y};
                        const ImVec2 bar_max = {label_right_x, bar_y + bar_h};
                        const ImVec2 fill_max = {bar_left + bar_w * living->hp, bar_y + bar_h};
                        draw_list.AddRectFilled(bar_min, bar_max, IM_COL32(0, 0, 0, 160));
                        const ImU32 hp_color = living->hp > 0.5f
                            ? IM_COL32(0, 200, 0, 200)
                            : (living->hp > 0.25f ? IM_COL32(200, 200, 0, 200) : IM_COL32(200, 0, 0, 200));
                        draw_list.AddRectFilled(bar_min, fill_max, hp_color);
                        draw_list.AddRect(bar_min, bar_max, IM_COL32(0, 0, 0, 200));
                    }
                }
            }

            if (is_on_mm(screen_pos)) {
                const float dx = mouse.x - screen_pos.x;
                const float dy = mouse.y - screen_pos.y;
                if (!mouse_held && (label_hovered || dx * dx + dy * dy <= hit_radius * hit_radius)) {
                    mm_hovered_marker = &marker;
                    mm_hovered_marker_layer = &layer;
                    if (sg) {
                        std::string tip = sg->name + "\nPossible NPCs:";
                        for (size_t ni = 0; ni < sg->npcs.size(); ni++) {
                            auto& npc = sg->npcs[ni];
                            EnsureSpawnNpcDecoded(npc);
                            const auto* npc_name = GetSpawnNpcDisplayName(npc);
                            tip += "\n  ";
                            if (marker.detected_npc_idx == static_cast<int>(ni)) tip += "> ";
                            tip += npc_name[0] ? npc_name : "(unnamed)";
                        }
                        if (marker.map_id == current_map_id) {
                            if (marker.detected_agent_id) tip += "\n(click to select, shift+click to flag heroes)";
                            else tip += "\n(shift+click to flag heroes)";
                        }
                        ImGui::SetTooltip("%s", tip.c_str());
                    } else {
                        const char* name = display_name[0] ? display_name : "Map Annotation";
                        if (marker.map_id != current_map_id) {
                            ImGui::SetTooltip("%s", name);
                        } else if (marker.detected_carrier_agent_id) {
                            ImGui::SetTooltip("%s\n(click to select carrier, shift+click to flag heroes)", name);
                        } else if (marker.detected_agent_id) {
                            ImGui::SetTooltip("%s\n(click to select, shift+click to flag heroes)", name);
                        } else {
                            ImGui::SetTooltip("%s\n(shift+click to flag heroes)", name);
                        }
                    }
                    if (ImGui::IsMouseClicked(0)) {
                        const bool shift = ImGui::GetIO().KeyShift;
                        if (shift) {
                            const auto& flag_pos = has_detection ? marker.detected_world_pos : marker.world_pos;
                            GW::GamePos game_pos{};
                            if (WorldMapWidget::WorldMapToGamePos(flag_pos, game_pos)) {
                                GW::GameThread::Enqueue([game_pos] {
                                    GW::PartyMgr::FlagAll(game_pos);
                                });
                            }
                        } else if (marker.detected_agent_id) {
                            const auto agent_id = marker.detected_carrier_agent_id ? marker.detected_carrier_agent_id : marker.detected_agent_id;
                            GW::GameThread::Enqueue([agent_id] {
                                if (GW::Agents::GetAgentByID(agent_id)) {
                                    GW::Agents::ChangeTarget(agent_id);
                                }
                            });
                        }
                    }
                }
            }
        }
    }

    // Draw trigger zones in edit mode (mission map)
    if (show_context_menu_edit) {
        const auto mm_trigger_map_id = GW::Map::GetMapID();
        for (const auto& layer : layers) {
            if (!layer.visible) continue;
            for (const auto& trigger : layer.triggers) {
                if (trigger.map_id != mm_trigger_map_id) continue;
                if (trigger.condition != TriggerConditionType::PlayerInZone) continue;
                if (trigger.radius <= 0.f) continue;

                const auto center = world_to_mm_screen(trigger.world_pos);
                const auto edge_screen = world_to_mm_screen({trigger.world_pos.x + trigger.radius, trigger.world_pos.y});
                const float screen_radius = std::abs(edge_screen.x - center.x);

                const ImU32 zone_color = trigger.fired
                    ? IM_COL32(100, 255, 100, 40)
                    : IM_COL32(255, 200, 50, 40);
                const ImU32 border_color = trigger.fired
                    ? IM_COL32(100, 255, 100, 120)
                    : IM_COL32(255, 200, 50, 120);

                draw_list.AddCircleFilled(center, screen_radius, zone_color, 32);
                draw_list.AddCircle(center, screen_radius, border_color, 32, 1.5f);

                if (!trigger.label.empty()) {
                    const auto label_size = ImGui::CalcTextSize(trigger.label.c_str());
                    draw_list.AddText(
                        {center.x - label_size.x * 0.5f, center.y - label_size.y * 0.5f},
                        border_color, trigger.label.c_str());
                }
            }
        }
    }

    // Draw pending route being edited
    if (editing_route && !pending_route.waypoints.empty()) {
        for (size_t i = 0; i < pending_route.waypoints.size() - 1; i++) {
            const auto p1 = world_to_mm_screen(pending_route.waypoints[i]);
            const auto p2 = world_to_mm_screen(pending_route.waypoints[i + 1]);
            draw_list.AddLine(p1, p2, pending_route.color, pending_route.thickness);
        }
        for (const auto& wp : pending_route.waypoints) {
            const auto sp = world_to_mm_screen(wp);
            draw_list.AddCircleFilled(sp, 4.0f * mm_scale.x, pending_route.color);
        }
    }

    // Draw route being recorded
    if (recording_route && recording_route_data.waypoints.size() >= 2) {
        for (size_t i = 0; i < recording_route_data.waypoints.size() - 1; i++) {
            const auto p1 = world_to_mm_screen(recording_route_data.waypoints[i]);
            const auto p2 = world_to_mm_screen(recording_route_data.waypoints[i + 1]);
            draw_list.AddLine(p1, p2, recording_route_data.color, recording_route_data.thickness);
        }
    }
}


bool MapAnnotationsModule::ShowAnnotationContextMenu()
{
    // Clear snapshots so context menu callbacks pick up fresh state
    context_menu_marker = nullptr;
    context_menu_marker_layer = nullptr;
    context_menu_route = nullptr;
    context_menu_route_layer = nullptr;

    // Markers take priority over routes
    if (hovered_marker) {
        ImGui::SetContextMenu(HoveredMarkerContextMenu);
        return true;
    }
    if (hovered_route) {
        ImGui::SetContextMenu(HoveredRouteContextMenu);
        return true;
    }
    return false;
}

bool MapAnnotationsModule::ShowMissionMapAnnotationContextMenu()
{
    context_menu_marker = nullptr;
    context_menu_marker_layer = nullptr;
    context_menu_route = nullptr;
    context_menu_route_layer = nullptr;

    if (mm_hovered_marker) {
        // Copy mission map hover state into world map hover vars so the context menu callbacks can snapshot them
        hovered_marker = mm_hovered_marker;
        hovered_marker_layer = mm_hovered_marker_layer;
        ImGui::SetContextMenu(HoveredMarkerContextMenu);
        return true;
    }
    if (mm_hovered_route) {
        hovered_route = mm_hovered_route;
        hovered_route_layer = mm_hovered_route_layer;
        ImGui::SetContextMenu(HoveredRouteContextMenu);
        return true;
    }
    return false;
}

bool MapAnnotationsModule::HoveredMarkerContextMenu(void*)
{
    // Snapshot the hovered marker on the first frame the context menu opens
    if (!context_menu_marker) {
        context_menu_marker = hovered_marker;
        context_menu_marker_layer = hovered_marker_layer;
        if (context_menu_marker) {
            strncpy(label_buf, context_menu_marker->label.c_str(), sizeof(label_buf) - 1);
            label_buf[sizeof(label_buf) - 1] = '\0';
        }
    }

    if (!context_menu_marker || !context_menu_marker_layer) {
        context_menu_marker = nullptr;
        context_menu_marker_layer = nullptr;
        return false;
    }

    if (context_menu_marker_layer->is_base_layer) {
        ImGui::TextDisabled("Base layer marker (read-only)");
        if (!context_menu_marker->label.empty()) {
            ImGui::Text("%s", context_menu_marker->label.c_str());
        }
        if (context_menu_marker->wiki_enabled) {
            EnsureMarkerEnglishDecoded(*context_menu_marker);
            if (ImGui::MenuItem(ICON_FA_GLOBE " Open Wiki")) {
                OpenMarkerWiki(*context_menu_marker);
                return false;
            }
        }
        // Spawn group assignment — available even on base layers
        if (!context_menu_marker_layer->spawn_groups.empty()) {
            const char* preview = context_menu_marker->spawn_group.empty() ? "(none)" : context_menu_marker->spawn_group.c_str();
            if (ImGui::BeginCombo("Spawn Group", preview)) {
                if (ImGui::Selectable("(none)", context_menu_marker->spawn_group.empty())) {
                    context_menu_marker->spawn_group.clear();
                    SaveLayer(*context_menu_marker_layer);
                }
                for (auto& sg : context_menu_marker_layer->spawn_groups) {
                    if (ImGui::Selectable(SpawnGroupLabel(sg).c_str(), context_menu_marker->spawn_group == sg.name)) {
                        context_menu_marker->spawn_group = sg.name;
                        if (!context_menu_marker->enc_name.empty()) {
                            bool already_in_group = false;
                            for (const auto& npc : sg.npcs) {
                                if (npc.enc_name == context_menu_marker->enc_name) { already_in_group = true; break; }
                            }
                            if (!already_in_group) {
                                SpawnGroupNpc npc;
                                npc.enc_name = context_menu_marker->enc_name;
                                sg.npcs.push_back(std::move(npc));
                            }
                        }
                        SaveLayer(*context_menu_marker_layer);
                    }
                }
                ImGui::EndCombo();
            }
        }
        return true;
    }

    const auto* marker_display = GetMarkerDisplayName(*context_menu_marker);
    ImGui::TextDisabled("Map Annotation: %s", marker_display[0] ? marker_display : "(no label)");
    ImGui::TextDisabled("Layer: %s", context_menu_marker_layer->name.c_str());

    // Wiki link — always available
    if (context_menu_marker->wiki_enabled) {
        EnsureMarkerEnglishDecoded(*context_menu_marker);
        if (ImGui::MenuItem(ICON_FA_GLOBE " Open Wiki")) {
            OpenMarkerWiki(*context_menu_marker);
            return false;
        }
    }

    if (!show_context_menu_edit) return true;

    ImGui::Separator();

    // Edit label
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputText("Label", label_buf, sizeof(label_buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        context_menu_marker->label = label_buf;
        SaveLayer(*context_menu_marker_layer);
        context_menu_marker = nullptr;
        context_menu_marker_layer = nullptr;
        return false;
    }

    // Icon picker
    if (DrawIconPicker(context_menu_marker->icon)) {
        SaveLayer(*context_menu_marker_layer);
    }

    // Spawn group assignment
    if (!context_menu_marker_layer->spawn_groups.empty()) {
        const char* preview = context_menu_marker->spawn_group.empty() ? "(none)" : context_menu_marker->spawn_group.c_str();
        if (ImGui::BeginCombo("Spawn Group", preview)) {
            if (ImGui::Selectable("(none)", context_menu_marker->spawn_group.empty())) {
                context_menu_marker->spawn_group.clear();
                SaveLayer(*context_menu_marker_layer);
            }
            for (auto& sg : context_menu_marker_layer->spawn_groups) {
                if (ImGui::Selectable(SpawnGroupLabel(sg).c_str(), context_menu_marker->spawn_group == sg.name)) {
                    context_menu_marker->spawn_group = sg.name;
                    if (!context_menu_marker->enc_name.empty()) {
                        bool already_in_group = false;
                        for (const auto& npc : sg.npcs) {
                            if (npc.enc_name == context_menu_marker->enc_name) { already_in_group = true; break; }
                        }
                        if (!already_in_group) {
                            SpawnGroupNpc npc;
                            npc.enc_name = context_menu_marker->enc_name;
                            sg.npcs.push_back(std::move(npc));
                        }
                    }
                    SaveLayer(*context_menu_marker_layer);
                }
            }
            ImGui::EndCombo();
        }
    }

    // Bundle item association
    {
        const auto* inventory = GW::Items::GetInventory();
        const auto* held_bundle = inventory ? inventory->bundle : nullptr;
        if (context_menu_marker->item_model_id) {
            ImGui::TextDisabled("Tracking bundle (model %u)", context_menu_marker->item_model_id);
            if (context_menu_marker->detected_carrier_agent_id) {
                ImGui::TextDisabled("Status: being carried");
            } else if (context_menu_marker->detected_agent_id) {
                ImGui::TextDisabled("Status: on ground");
            } else {
                ImGui::TextDisabled("Status: not found");
            }
            if (ImGui::MenuItem(ICON_FA_UNLINK " Remove Bundle Tracking")) {
                context_menu_marker->item_model_id = 0;
                context_menu_marker->detected_agent_id = 0;
                context_menu_marker->detected_carrier_agent_id = 0;
                SaveLayer(*context_menu_marker_layer);
            }
        }
        if (held_bundle && held_bundle->type == GW::Constants::ItemType::Bundle) {
            char bundle_label[128];
            snprintf(bundle_label, sizeof(bundle_label), ICON_FA_LINK " Associate Carried Bundle (model %u)", held_bundle->model_id);
            if (ImGui::MenuItem(bundle_label)) {
                context_menu_marker->item_model_id = held_bundle->model_id;
                context_menu_marker->detected_agent_id = 0;
                context_menu_marker->detected_carrier_agent_id = 0;
                SaveLayer(*context_menu_marker_layer);
            }
        } else {
            ImGui::BeginDisabled();
            ImGui::MenuItem(ICON_FA_LINK " Associate Carried Bundle (none held)");
            ImGui::EndDisabled();
        }
    }

    // Delete with confirmation
    if (delete_confirm_id == context_menu_marker->id) {
        if (ImGui::Button("Confirm Delete Marker?")) {
            const auto marker_id = context_menu_marker->id;
            auto* layer = context_menu_marker_layer;
            std::erase_if(layer->markers, [marker_id](const MapAnnotationMarker& m) { return m.id == marker_id; });
            SaveLayer(*layer);
            context_menu_marker = nullptr;
            context_menu_marker_layer = nullptr;
            delete_confirm_id = 0;
            return false;
        }
    } else {
        if (ImGui::Button("Delete Marker")) {
            delete_confirm_id = context_menu_marker->id;
        }
    }

    return true;
}

bool MapAnnotationsModule::HoveredRouteContextMenu(void*)
{
    // Snapshot on first frame
    if (!context_menu_route) {
        context_menu_route = hovered_route;
        context_menu_route_layer = hovered_route_layer;
        if (context_menu_route) {
            strncpy(label_buf, context_menu_route->label.c_str(), sizeof(label_buf) - 1);
            label_buf[sizeof(label_buf) - 1] = '\0';
        }
    }

    if (!context_menu_route || !context_menu_route_layer) {
        context_menu_route = nullptr;
        context_menu_route_layer = nullptr;
        return false;
    }

    if (context_menu_route_layer->is_base_layer) {
        ImGui::TextDisabled("Base layer route (read-only)");
        if (!context_menu_route->label.empty()) {
            ImGui::Text("%s", context_menu_route->label.c_str());
        }
        return true;
    }

    ImGui::TextDisabled("Annotation Route (%zu pts)", context_menu_route->waypoints.size());
    if (!context_menu_route->label.empty()) {
        ImGui::Text("%s", context_menu_route->label.c_str());
    }
    ImGui::TextDisabled("Layer: %s", context_menu_route_layer->name.c_str());
    ImGui::Separator();

    // Edit label
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputText("Label", label_buf, sizeof(label_buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        context_menu_route->label = label_buf;
        SaveLayer(*context_menu_route_layer);
        context_menu_route = nullptr;
        context_menu_route_layer = nullptr;
        return false;
    }

    // Toggle loop
    if (ImGui::Checkbox("Loop", &context_menu_route->loop)) {
        SaveLayer(*context_menu_route_layer);
    }

    // Delete with confirmation
    if (delete_confirm_id == context_menu_route->id) {
        if (ImGui::Button("Confirm Delete Route?")) {
            const auto route_id = context_menu_route->id;
            auto* layer = context_menu_route_layer;
            std::erase_if(layer->routes, [route_id](const MapAnnotationRoute& r) { return r.id == route_id; });
            SaveLayer(*layer);
            context_menu_route = nullptr;
            context_menu_route_layer = nullptr;
            delete_confirm_id = 0;
            return false;
        }
    } else {
        if (ImGui::Button("Delete Route")) {
            delete_confirm_id = context_menu_route->id;
        }
    }

    return true;
}

void MapAnnotationsModule::ResetContextMenuState()
{
    context_menu_snapshotted = false;
}

bool MapAnnotationsModule::WorldMapContextMenuItems(const GW::Vec2f& click_pos)
{
    if (!initialized) return true;

    // Snapshot hovered annotation on first frame of context menu
    if (!context_menu_snapshotted) {
        context_menu_snapshotted = true;
        context_menu_marker = nullptr;
        context_menu_marker_layer = nullptr;
        context_menu_route = nullptr;
        context_menu_route_layer = nullptr;
        delete_confirm_id = 0;
        context_waypoint_idx = -1;
        context_edge_idx = -1;
        if (hovered_marker) {
            context_menu_marker = hovered_marker;
            context_menu_marker_layer = hovered_marker_layer;
            strncpy(label_buf, context_menu_marker->label.c_str(), sizeof(label_buf) - 1);
            label_buf[sizeof(label_buf) - 1] = '\0';
        }
        else if (mm_hovered_marker) {
            context_menu_marker = mm_hovered_marker;
            context_menu_marker_layer = mm_hovered_marker_layer;
            strncpy(label_buf, context_menu_marker->label.c_str(), sizeof(label_buf) - 1);
            label_buf[sizeof(label_buf) - 1] = '\0';
        }
        else if (hovered_route) {
            context_menu_route = hovered_route;
            context_menu_route_layer = hovered_route_layer;
            context_waypoint_idx = hovered_waypoint_idx;
            context_edge_idx = hovered_edge_idx;
            strncpy(label_buf, context_menu_route->label.c_str(), sizeof(label_buf) - 1);
            label_buf[sizeof(label_buf) - 1] = '\0';
        }
        else if (mm_hovered_route) {
            context_menu_route = mm_hovered_route;
            context_menu_route_layer = mm_hovered_route_layer;
            context_waypoint_idx = mm_hovered_waypoint_idx;
            context_edge_idx = mm_hovered_edge_idx;
            strncpy(label_buf, context_menu_route->label.c_str(), sizeof(label_buf) - 1);
            label_buf[sizeof(label_buf) - 1] = '\0';
        }
    }

    // Handle "move node" mode — place the node at click position
    if (moving_waypoint && moving_route && moving_route_layer) {
        ImGui::Separator();
        ImGui::TextDisabled("Moving node %d — click to place", moving_waypoint_idx);
        if (ImGui::Button("Place Node Here")) {
            if (moving_waypoint_idx >= 0 && moving_waypoint_idx < static_cast<int>(moving_route->waypoints.size())) {
                moving_route->waypoints[moving_waypoint_idx] = click_pos;
                SaveLayer(*moving_route_layer);
            }
            moving_waypoint = false;
            moving_route = nullptr;
            moving_route_layer = nullptr;
            moving_waypoint_idx = -1;
            return false;
        }
        if (ImGui::Button("Cancel Move")) {
            moving_waypoint = false;
            moving_route = nullptr;
            moving_route_layer = nullptr;
            moving_waypoint_idx = -1;
            return false;
        }
        return true;
    }

    // Hovered marker actions
    if (context_menu_marker && context_menu_marker_layer) {
        ImGui::Separator();
        const auto* marker_dn = GetMarkerDisplayName(*context_menu_marker);
        const char* marker_display = marker_dn[0] ? marker_dn : "(no label)";

        // Spawn group assignment — available for all layers
        if (!context_menu_marker_layer->spawn_groups.empty()) {
            const char* sg_preview = context_menu_marker->spawn_group.empty() ? "(none)" : context_menu_marker->spawn_group.c_str();
            if (ImGui::BeginCombo("Spawn Group##wm", sg_preview)) {
                if (ImGui::Selectable("(none)", context_menu_marker->spawn_group.empty())) {
                    context_menu_marker->spawn_group.clear();
                    SaveLayer(*context_menu_marker_layer);
                }
                for (auto& sg : context_menu_marker_layer->spawn_groups) {
                    if (ImGui::Selectable(SpawnGroupLabel(sg).c_str(), context_menu_marker->spawn_group == sg.name)) {
                        context_menu_marker->spawn_group = sg.name;
                        if (!context_menu_marker->enc_name.empty()) {
                            bool already_in_group = false;
                            for (const auto& npc : sg.npcs) {
                                if (npc.enc_name == context_menu_marker->enc_name) { already_in_group = true; break; }
                            }
                            if (!already_in_group) {
                                SpawnGroupNpc npc;
                                npc.enc_name = context_menu_marker->enc_name;
                                sg.npcs.push_back(std::move(npc));
                            }
                        }
                        SaveLayer(*context_menu_marker_layer);
                    }
                }
                ImGui::EndCombo();
            }
        }

        if (context_menu_marker_layer->is_base_layer) {
            ImGui::TextDisabled("Marker: %s (base layer)", marker_display);
        }
        else {
            ImGui::TextDisabled("Marker: %s", marker_display);
            if (ImGui::BeginMenu("Edit Marker")) {
                if (!context_menu_marker->enc_name.empty()) {
                    ImGui::TextDisabled("Name: %s (localized)", marker_display);
                    if (ImGui::MenuItem("Clear localized name")) {
                        context_menu_marker->enc_name.clear();
                        context_menu_marker->decoded_name.clear();
                        context_menu_marker->decode_pending = false;
                        SaveLayer(*context_menu_marker_layer);
                    }
                }
                ImGui::SetNextItemWidth(200.0f);
                if (ImGui::InputText("Label##edit", label_buf, sizeof(label_buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                    context_menu_marker->label = label_buf;
                    SaveLayer(*context_menu_marker_layer);
                    context_menu_marker = nullptr;
                    context_menu_marker_layer = nullptr;
                    return false;
                }
                if (!context_menu_marker->enc_name.empty()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(fallback)");
                }
                if (DrawIconPicker(context_menu_marker->icon)) {
                    SaveLayer(*context_menu_marker_layer);
                }
                ImVec4 col = ImGui::ColorConvertU32ToFloat4(context_menu_marker->color);
                if (ImGui::ColorEdit4("Color##marker", &col.x, ImGuiColorEditFlags_NoInputs)) {
                    context_menu_marker->color = ImGui::ColorConvertFloat4ToU32(col);
                    SaveLayer(*context_menu_marker_layer);
                }
                if (ImGui::BeginMenu("Presets##markercolor")) {
                    for (const auto& preset : marker_color_presets) {
                        const auto pcol = ImGui::ColorConvertU32ToFloat4(preset.color);
                        ImGui::PushStyleColor(ImGuiCol_Text, pcol);
                        if (ImGui::MenuItem(preset.name)) {
                            context_menu_marker->color = preset.color;
                            SaveLayer(*context_menu_marker_layer);
                        }
                        ImGui::PopStyleColor();
                    }
                    ImGui::EndMenu();
                }
                ImGui::SetNextItemWidth(120.0f);
                if (ImGui::SliderFloat("Size##marker", &context_menu_marker->size, 2.0f, 20.0f, "%.0f")) {
                    SaveLayer(*context_menu_marker_layer);
                }
                if (ImGui::Checkbox("Open Wiki on Click##marker", &context_menu_marker->wiki_enabled)) {
                    SaveLayer(*context_menu_marker_layer);
                }
                // Bundle item association
                ImGui::Separator();
                if (context_menu_marker->item_model_id) {
                    ImGui::TextDisabled("Tracking bundle (model %u)", context_menu_marker->item_model_id);
                    if (context_menu_marker->detected_carrier_agent_id) {
                        ImGui::TextDisabled("Status: being carried");
                    } else if (context_menu_marker->detected_agent_id) {
                        ImGui::TextDisabled("Status: on ground");
                    } else {
                        ImGui::TextDisabled("Status: not found");
                    }
                    if (ImGui::MenuItem(ICON_FA_UNLINK " Remove Bundle Tracking##edit")) {
                        context_menu_marker->item_model_id = 0;
                        context_menu_marker->detected_agent_id = 0;
                        context_menu_marker->detected_carrier_agent_id = 0;
                        SaveLayer(*context_menu_marker_layer);
                    }
                }
                {
                    const auto* inventory = GW::Items::GetInventory();
                    const auto* held_bundle = inventory ? inventory->bundle : nullptr;
                    if (held_bundle && held_bundle->type == GW::Constants::ItemType::Bundle) {
                        char bundle_label[128];
                        snprintf(bundle_label, sizeof(bundle_label), ICON_FA_LINK " Associate Carried Bundle (model %u)##edit", held_bundle->model_id);
                        if (ImGui::MenuItem(bundle_label)) {
                            context_menu_marker->item_model_id = held_bundle->model_id;
                            context_menu_marker->detected_agent_id = 0;
                            context_menu_marker->detected_carrier_agent_id = 0;
                            SaveLayer(*context_menu_marker_layer);
                        }
                    } else {
                        ImGui::BeginDisabled();
                        ImGui::MenuItem(ICON_FA_LINK " Associate Carried Bundle (none held)##edit");
                        ImGui::EndDisabled();
                    }
                }
                // Trigger creation from marker
                if (!context_menu_marker->enc_name.empty()) {
                    ImGui::Separator();
                    if (ImGui::BeginMenu("Add Trigger...")) {
                        const char* trigger_types[] = {"On Detected", "On Death"};
                        for (int ti = 0; ti < 2; ti++) {
                            if (ImGui::BeginMenu(trigger_types[ti])) {
                                std::vector<const MapAnnotationRoute*> sorted_routes;
                                for (const auto& r : context_menu_marker_layer->routes) {
                                    if (r.map_id == context_menu_marker->map_id)
                                        sorted_routes.push_back(&r);
                                }
                                std::sort(sorted_routes.begin(), sorted_routes.end(),
                                    [](const MapAnnotationRoute* a, const MapAnnotationRoute* b) { return a->label < b->label; });
                                bool any = false;
                                for (const auto* rp : sorted_routes) {
                                    const auto& r = *rp;
                                    const char* rl = r.label.empty() ? "(no label)" : r.label.c_str();
                                    const char* action_names[] = {"Highlight", "Dim", "Hide"};
                                    for (int ai = 0; ai < 3; ai++) {
                                        auto item_label = std::format("{} -> {}##t{}r{}a{}", action_names[ai], rl, ti, r.id, ai);
                                        if (ImGui::MenuItem(item_label.c_str())) {
                                            MapAnnotationTrigger t;
                                            t.id = next_trigger_id++;
                                            t.map_id = context_menu_marker->map_id;
                                            t.condition = ti == 0 ? TriggerConditionType::NpcDetected : TriggerConditionType::NpcDied;
                                            t.marker_id = context_menu_marker->id;
                                            TriggerAction a;
                                            a.route_id = r.id;
                                            a.type = static_cast<TriggerActionType>(ai);
                                            t.actions.push_back(a);
                                            context_menu_marker_layer->triggers.push_back(std::move(t));
                                            SaveLayer(*context_menu_marker_layer);
                                        }
                                    }
                                    any = true;
                                    ImGui::Separator();
                                }
                                if (!any) ImGui::TextDisabled("No routes on this map");
                                ImGui::EndMenu();
                            }
                        }
                        ImGui::EndMenu();
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Move to Layer##marker")) {
                for (auto& layer : layers) {
                    if (layer.is_base_layer || &layer == context_menu_marker_layer) continue;
                    if (ImGui::MenuItem(layer.name.c_str())) {
                        auto marker_copy = *context_menu_marker;
                        const auto marker_id = context_menu_marker->id;
                        auto* src_layer = context_menu_marker_layer;
                        layer.markers.push_back(std::move(marker_copy));
                        std::erase_if(src_layer->markers, [marker_id](const MapAnnotationMarker& m) { return m.id == marker_id; });
                        SaveLayer(*src_layer);
                        SaveLayer(layer);
                        context_menu_marker = nullptr;
                        context_menu_marker_layer = nullptr;
                        ImGui::EndMenu();
                        return false;
                    }
                }
                ImGui::EndMenu();
            }
            if (delete_confirm_id == context_menu_marker->id) {
                if (ImGui::MenuItem("Confirm Delete Marker?")) {
                    const auto marker_id = context_menu_marker->id;
                    auto* layer = context_menu_marker_layer;
                    std::erase_if(layer->markers, [marker_id](const MapAnnotationMarker& m) { return m.id == marker_id; });
                    SaveLayer(*layer);
                    context_menu_marker = nullptr;
                    context_menu_marker_layer = nullptr;
                    delete_confirm_id = 0;
                    return false;
                }
            } else {
                if (ImGui::Selectable("Delete Marker", false, ImGuiSelectableFlags_NoAutoClosePopups)) {
                    delete_confirm_id = context_menu_marker->id;
                }
            }
        }
    }

    // Hovered route actions
    if (context_menu_route && context_menu_route_layer) {
        ImGui::Separator();
        if (context_menu_route_layer->is_base_layer) {
            ImGui::TextDisabled("Route: %s (base layer)", context_menu_route->label.empty() ? "(no label)" : context_menu_route->label.c_str());
        }
        else {
            ImGui::TextDisabled("Route: %s (%zu pts)",
                context_menu_route->label.empty() ? "(no label)" : context_menu_route->label.c_str(),
                context_menu_route->waypoints.size());

            // Node editing — top-level actions for quick access
            if (!context_menu_route->waypoints.empty()) {
                if (context_edge_idx >= 0) {
                    const int insert_after = context_edge_idx;
                    if (ImGui::MenuItem("Insert Node")) {
                        context_menu_route->waypoints.insert(
                            context_menu_route->waypoints.begin() + insert_after + 1, click_pos);
                        SaveLayer(*context_menu_route_layer);
                        context_menu_route = nullptr;
                        context_menu_route_layer = nullptr;
                        return false;
                    }
                }
                if (context_waypoint_idx >= 0 && context_waypoint_idx < static_cast<int>(context_menu_route->waypoints.size())) {
                    if (ImGui::MenuItem("Move Node")) {
                        moving_waypoint = true;
                        moving_route = context_menu_route;
                        moving_route_layer = context_menu_route_layer;
                        moving_waypoint_idx = context_waypoint_idx;
                        context_menu_route = nullptr;
                        context_menu_route_layer = nullptr;
                        return false;
                    }
                    if (context_waypoint_idx > 0 && context_waypoint_idx < static_cast<int>(context_menu_route->waypoints.size()) - 1) {
                        if (ImGui::MenuItem("Split Route Here")) {
                            // Create new route from waypoints after the split point
                            MapAnnotationRoute new_route;
                            new_route.id = next_route_id++;
                            new_route.map_id = context_menu_route->map_id;
                            new_route.color = context_menu_route->color;
                            new_route.thickness = context_menu_route->thickness;
                            new_route.label = context_menu_route->label;
                            new_route.show_label = context_menu_route->show_label;
                            new_route.show_direction = context_menu_route->show_direction;
                            new_route.visible = context_menu_route->visible;
                            new_route.waypoints.assign(
                                context_menu_route->waypoints.begin() + context_waypoint_idx,
                                context_menu_route->waypoints.end());
                            // Truncate original route at the split point (inclusive)
                            context_menu_route->waypoints.resize(context_waypoint_idx + 1);
                            context_menu_route_layer->routes.push_back(std::move(new_route));
                            SaveLayer(*context_menu_route_layer);
                            context_menu_route = nullptr;
                            context_menu_route_layer = nullptr;
                            return false;
                        }
                    }
                    if (context_menu_route->waypoints.size() > 2) {
                        if (ImGui::MenuItem("Delete Node")) {
                            context_menu_route->waypoints.erase(
                                context_menu_route->waypoints.begin() + context_waypoint_idx);
                            SaveLayer(*context_menu_route_layer);
                            context_menu_route = nullptr;
                            context_menu_route_layer = nullptr;
                            return false;
                        }
                    }
                    if (ImGui::BeginMenu("Add Trigger Here")) {
                        const auto& wp = context_menu_route->waypoints[context_waypoint_idx];
                        std::vector<const MapAnnotationRoute*> sorted_routes;
                        for (const auto& r : context_menu_route_layer->routes) {
                            if (r.map_id == context_menu_route->map_id)
                                sorted_routes.push_back(&r);
                        }
                        std::sort(sorted_routes.begin(), sorted_routes.end(),
                            [](const MapAnnotationRoute* a, const MapAnnotationRoute* b) { return a->label < b->label; });
                        const char* action_names[] = {"Highlight", "Dim", "Hide"};
                        bool any = false;
                        for (const auto* rp : sorted_routes) {
                            const auto& r = *rp;
                            const char* rl = r.label.empty() ? "(no label)" : r.label.c_str();
                            for (int ai = 0; ai < 3; ai++) {
                                auto item_label = std::format("{} -> {}##pz_r{}a{}", action_names[ai], rl, r.id, ai);
                                if (ImGui::MenuItem(item_label.c_str())) {
                                    MapAnnotationTrigger t;
                                    t.id = next_trigger_id++;
                                    t.map_id = context_menu_route->map_id;
                                    t.condition = TriggerConditionType::PlayerInZone;
                                    t.world_pos = wp;
                                    t.radius = 20.0f;
                                    TriggerAction a;
                                    a.route_id = r.id;
                                    a.type = static_cast<TriggerActionType>(ai);
                                    t.actions.push_back(a);
                                    context_menu_route_layer->triggers.push_back(std::move(t));
                                    SaveLayer(*context_menu_route_layer);
                                }
                            }
                            any = true;
                            ImGui::Separator();
                        }
                        if (!any) ImGui::TextDisabled("No routes on this map");
                        ImGui::EndMenu();
                    }
                }
            }

            if (ImGui::BeginMenu("Edit Route")) {
                ImGui::SetNextItemWidth(200.0f);
                if (ImGui::InputText("Label##editroute", label_buf, sizeof(label_buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                    context_menu_route->label = label_buf;
                    SaveLayer(*context_menu_route_layer);
                    context_menu_route = nullptr;
                    context_menu_route_layer = nullptr;
                    return false;
                }
                ImVec4 rcol = ImGui::ColorConvertU32ToFloat4(context_menu_route->color);
                if (ImGui::ColorEdit4("Color##route", &rcol.x, ImGuiColorEditFlags_NoInputs)) {
                    context_menu_route->color = ImGui::ColorConvertFloat4ToU32(rcol);
                    SaveLayer(*context_menu_route_layer);
                }
                if (ImGui::BeginMenu("Presets##routecolor")) {
                    for (const auto& preset : route_color_presets) {
                        const auto col = ImGui::ColorConvertU32ToFloat4(preset.color);
                        ImGui::PushStyleColor(ImGuiCol_Text, col);
                        if (ImGui::MenuItem(preset.name)) {
                            context_menu_route->color = preset.color;
                            context_menu_route->thickness = preset.thickness;
                            SaveLayer(*context_menu_route_layer);
                        }
                        ImGui::PopStyleColor();
                    }
                    ImGui::EndMenu();
                }
                ImGui::SetNextItemWidth(120.0f);
                if (ImGui::SliderFloat("Thickness##route", &context_menu_route->thickness, 1.0f, 8.0f, "%.1f")) {
                    SaveLayer(*context_menu_route_layer);
                }
                if (ImGui::Checkbox("Show Label##route", &context_menu_route->show_label)) {
                    SaveLayer(*context_menu_route_layer);
                }
                if (ImGui::Checkbox("Show Direction##route", &context_menu_route->show_direction)) {
                    SaveLayer(*context_menu_route_layer);
                }
                if (context_menu_route->show_direction) {
                    ImGui::SameLine();
                    if (ImGui::Button("Reverse##route")) {
                        std::ranges::reverse(context_menu_route->waypoints);
                        SaveLayer(*context_menu_route_layer);
                    }
                }
                if (ImGui::Checkbox("Loop", &context_menu_route->loop)) {
                    SaveLayer(*context_menu_route_layer);
                }
                if (ImGui::Checkbox("Dimmed by Default", &context_menu_route->dimmed_by_default)) {
                    SaveLayer(*context_menu_route_layer);
                }
                // Merge with another route in the same layer
                if (ImGui::BeginMenu("Merge With...")) {
                    // Build sorted list of candidate routes
                    std::vector<MapAnnotationRoute*> candidates;
                    for (auto& other : context_menu_route_layer->routes) {
                        if (&other != context_menu_route)
                            candidates.push_back(&other);
                    }
                    std::sort(candidates.begin(), candidates.end(), [](const MapAnnotationRoute* a, const MapAnnotationRoute* b) {
                        auto* ma = Resources::GetMapName(a->map_id);
                        auto* mb = Resources::GetMapName(b->map_id);
                        const auto na = ma ? ma->string() : std::string();
                        const auto nb = mb ? mb->string() : std::string();
                        if (na != nb) return na < nb;
                        return a->label < b->label;
                    });
                    bool any = false;
                    for (auto* other_ptr : candidates) {
                        auto& other = *other_ptr;
                        const char* route_name = other.label.empty() ? "(no label)" : other.label.c_str();
                        auto* mn = Resources::GetMapName(other.map_id);
                        const auto map_name = mn ? mn->string() : std::format("Map {}", static_cast<uint32_t>(other.map_id));
                        char buf[256];
                        snprintf(buf, sizeof(buf), "%s / %s (%zu pts)##merge%u", map_name.c_str(), route_name, other.waypoints.size(), other.id);
                        if (ImGui::MenuItem(buf)) {
                            context_menu_route->waypoints.insert(
                                context_menu_route->waypoints.end(),
                                other.waypoints.begin(), other.waypoints.end());
                            const auto other_id = other.id;
                            std::erase_if(context_menu_route_layer->routes,
                                [other_id](const MapAnnotationRoute& r) { return r.id == other_id; });
                            SaveLayer(*context_menu_route_layer);
                            context_menu_route = nullptr;
                            context_menu_route_layer = nullptr;
                            ImGui::EndMenu();
                            return false;
                        }
                        any = true;
                    }
                    if (!any) {
                        ImGui::TextDisabled("No other routes in this layer");
                    }
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
            if (!recording_route && !editing_route && ImGui::MenuItem("Continue Recording")) {
                recording_route = true;
                recording_timer = 0.f;
                recording_map_id = GW::Map::GetMapID();
                // Move the route data into recording state so new points append
                recording_route_data = std::move(*context_menu_route);
                // Remove the original from the layer — it will be re-added on stop
                const auto route_id = recording_route_data.id;
                auto* layer = context_menu_route_layer;
                std::erase_if(layer->routes, [route_id](const MapAnnotationRoute& r) { return r.id == route_id; });
                SaveLayer(*layer);
                context_menu_route = nullptr;
                context_menu_route_layer = nullptr;
                return false;
            }
            if (ImGui::BeginMenu("Move to Layer##route")) {
                for (auto& layer : layers) {
                    if (layer.is_base_layer || &layer == context_menu_route_layer) continue;
                    if (ImGui::MenuItem(layer.name.c_str())) {
                        auto route_copy = *context_menu_route;
                        const auto route_id = context_menu_route->id;
                        auto* src_layer = context_menu_route_layer;
                        layer.routes.push_back(std::move(route_copy));
                        std::erase_if(src_layer->routes, [route_id](const MapAnnotationRoute& r) { return r.id == route_id; });
                        SaveLayer(*src_layer);
                        SaveLayer(layer);
                        context_menu_route = nullptr;
                        context_menu_route_layer = nullptr;
                        ImGui::EndMenu();
                        return false;
                    }
                }
                ImGui::EndMenu();
            }
            if (delete_confirm_id == context_menu_route->id) {
                if (ImGui::MenuItem("Confirm Delete Route?")) {
                    const auto route_id = context_menu_route->id;
                    auto* layer = context_menu_route_layer;
                    std::erase_if(layer->routes, [route_id](const MapAnnotationRoute& r) { return r.id == route_id; });
                    SaveLayer(*layer);
                    context_menu_route = nullptr;
                    context_menu_route_layer = nullptr;
                    delete_confirm_id = 0;
                    return false;
                }
            } else {
                if (ImGui::Selectable("Delete Route", false, ImGuiSelectableFlags_NoAutoClosePopups)) {
                    delete_confirm_id = context_menu_route->id;
                }
            }
        }
    }

    ImGui::Separator();

    // In-progress editing/recording controls
    if (editing_route) {
        if (pathing_in_progress) {
            ImGui::TextDisabled("Computing path...");
            return true;
        }
        ImGui::TextDisabled("Route editing (%zu pts)", pending_route.waypoints.size());
        if (ImGui::MenuItem("Add Waypoint")) {
            AddRouteWaypoint(click_pos);
            return false;
        }
        if (ImGui::MenuItem("Add Pathed Waypoint")) {
            AddPathedWaypoint(click_pos);
            return false;
        }
        if (pending_route.waypoints.size() > 1 && ImGui::MenuItem("Replace Last Waypoint")) {
            pending_route.waypoints.back() = click_pos;
            return false;
        }
        if (pending_route.waypoints.size() > 1 && ImGui::MenuItem("Remove Last Waypoint")) {
            pending_route.waypoints.pop_back();
            return false;
        }
        if (ImGui::MenuItem("Finish Route")) {
            FinishRoute();
            return false;
        }
        if (ImGui::MenuItem("Cancel Route")) {
            CancelRoute();
            return false;
        }
        return true;
    }

    if (recording_route) {
        ImGui::TextDisabled("Recording route... (%zu pts)", recording_route_data.waypoints.size());
        if (ImGui::MenuItem("Stop Recording")) {
            StopRecordingRoute();
            return false;
        }
    }

    if (ImGui::BeginMenu("Annotations")) {
        if (ImGui::MenuItem("Add Marker Here")) {
            auto& layer = GetActiveLayer();
            MapAnnotationMarker marker;
            marker.id = next_marker_id++;
            marker.world_pos = click_pos;
            marker.map_id = WorldMapWidget::GetMapIdForLocation(click_pos);
            marker.color = IM_COL32(255, 50, 50, 220);
            marker.size = 6.0f;
            layer.markers.push_back(marker);
            SaveLayer(layer);
            return false;
        }
        if (ImGui::BeginMenu("Start Route Here")) {
            for (const auto& preset : route_color_presets) {
                const auto col = ImGui::ColorConvertU32ToFloat4(preset.color);
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                if (ImGui::MenuItem(preset.name)) {
                    StartRouteAtWorldPos(click_pos);
                    pending_route.color = preset.color;
                    pending_route.thickness = preset.thickness;
                    ImGui::PopStyleColor();
                    ImGui::EndMenu();
                    return false;
                }
                ImGui::PopStyleColor();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Custom")) {
                StartRouteAtWorldPos(click_pos);
                ImGui::EndMenu();
                return false;
            }
            ImGui::EndMenu();
        }
        if (GW::Agents::GetTarget()) {
            if (ImGui::MenuItem("Mark Target")) {
                AddTargetAsMarker();
                return false;
            }
        }
        if (!recording_route) {
            if (ImGui::MenuItem("Record Route (Walk)")) {
                StartRecordingRoute();
                return false;
            }
        }
        ImGui::EndMenu();
    }

    return true;
}

void MapAnnotationsModule::StartRouteAtWorldPos(const GW::Vec2f& world_pos)
{
    editing_route = true;
    pending_route = {};
    pending_route.id = next_route_id++;
    pending_route.map_id = WorldMapWidget::GetMapIdForLocation(world_pos);
    pending_route.color = IM_COL32(50, 255, 50, 220);
    pending_route.thickness = 2.0f;
    pending_route.waypoints.push_back(world_pos);
}

void MapAnnotationsModule::AddRouteWaypoint(const GW::Vec2f& world_pos)
{
    if (!editing_route) return;
    pending_route.waypoints.push_back(world_pos);
}

void MapAnnotationsModule::FinishRoute()
{
    if (!editing_route) return;
    editing_route = false;
    if (pending_route.waypoints.size() < 2) return;

    auto& layer = GetActiveLayer();
    layer.routes.push_back(std::move(pending_route));
    SaveLayer(layer);
    pending_route = {};
}

void MapAnnotationsModule::CancelRoute()
{
    editing_route = false;
    pending_route = {};
}

bool MapAnnotationsModule::IsEditingRoute()
{
    return editing_route;
}

void MapAnnotationsModule::AddTargetAsMarker()
{
    if (!initialized) return;
    const auto target = GW::Agents::GetTarget();
    if (!target) return;

    GW::Vec2f world_pos;
    if (!WorldMapWidget::GamePosToWorldMap(target->pos, world_pos)) return;

    // Get the encoded name for localization support
    const auto enc_name = GW::Agents::GetAgentEncName(target);

    // Determine icon, color, and item tracking based on target type
    auto icon = new_marker_icon;
    auto color = IM_COL32(255, 50, 50, 220);
    uint32_t item_model_id = 0;
    if (target->GetIsItemType()) {
        const auto* agent_item = target->GetAsAgentItem();
        if (agent_item) {
            const auto* item = GW::Items::GetItemById(agent_item->item_id);
            if (item && item->type == GW::Constants::ItemType::Bundle) {
                item_model_id = item->model_id;
                icon = ICON_FA_BOX;
                color = IM_COL32(255, 200, 50, 220);
            } else {
                icon = ICON_FA_CUBE;
                color = IM_COL32(200, 200, 200, 220);
            }
        }
    } else if (target->GetIsLivingType()) {
        const auto living = target->GetAsAgentLiving();
        if (living) {
            switch (living->allegiance) {
            case GW::Constants::Allegiance::Enemy:
                icon = ICON_FA_SKULL;
                color = IM_COL32(255, 50, 50, 220);
                break;
            case GW::Constants::Allegiance::Ally_NonAttackable:
            case GW::Constants::Allegiance::Npc_Minipet:
                icon = ICON_FA_MAP_MARKER_ALT;
                color = IM_COL32(50, 200, 255, 220);
                break;
            default:
                break;
            }
        }
    }

    auto& layer = GetActiveLayer();
    MapAnnotationMarker marker;
    marker.id = next_marker_id++;
    marker.world_pos = world_pos;
    marker.map_id = GW::Map::GetMapID();
    marker.color = color;
    marker.size = 6.0f;
    marker.icon = icon;
    marker.item_model_id = item_model_id;
    if (enc_name && *enc_name) {
        marker.enc_name = enc_name;
    }

    layer.markers.push_back(marker);
    SaveLayer(layer);
}

void MapAnnotationsModule::StartRecordingRoute()
{
    if (!initialized || recording_route) return;
    recording_route = true;
    recording_timer = 0.f;
    recording_map_id = GW::Map::GetMapID();
    recording_route_data = {};
    recording_route_data.id = next_route_id++;
    recording_route_data.map_id = recording_map_id;
    recording_route_data.color = IM_COL32(50, 200, 255, 220);
    recording_route_data.thickness = 2.0f;

    // Add starting position immediately
    const auto me = GW::Agents::GetControlledCharacter();
    if (me) {
        GW::Vec2f world_pos;
        if (WorldMapWidget::GamePosToWorldMap(me->pos, world_pos)) {
            recording_route_data.waypoints.push_back(world_pos);
        }
    }
}

void MapAnnotationsModule::StopRecordingRoute()
{
    if (!recording_route) return;
    recording_route = false;

    if (recording_route_data.waypoints.size() >= 2) {
        auto& layer = GetActiveLayer();
        layer.routes.push_back(std::move(recording_route_data));
        SaveLayer(layer);
    }
    recording_route_data = {};
}

bool MapAnnotationsModule::IsRecordingRoute()
{
    return recording_route;
}

std::vector<std::string> MapAnnotationsModule::GetUserLayerNames()
{
    std::vector<std::string> names;
    for (const auto& layer : layers) {
        if (!layer.is_base_layer) {
            names.push_back(layer.name);
        }
    }
    return names;
}
