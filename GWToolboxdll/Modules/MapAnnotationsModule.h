#pragma once

#include <IconsFontAwesome5.h>

#include <ToolboxWindow.h>

#include <GWCA/GameContainers/GamePos.h>
#include <GWCA/Constants/Maps.h>

struct ImDrawList;

// A possible NPC in a spawn group (e.g., one of several bosses that can appear)
struct SpawnGroupNpc {
    std::wstring enc_name;
    std::string label; // Fallback display name

    // Transient
    mutable std::string decoded_name;
    mutable bool decode_pending = false;
};

// Definition of a spawn group: a set of NPCs that randomly appear across spawn locations
struct SpawnGroupDef {
    std::string name; // Display name (e.g., "Fort Ranik Charr Bosses")
    GW::Constants::MapID map_id = GW::Constants::MapID::None;
    ImU32 color = IM_COL32(200, 50, 200, 220);
    std::string icon;
    std::vector<SpawnGroupNpc> npcs;
};

struct MapAnnotationMarker {
    uint32_t id = 0;
    GW::Vec2f world_pos{};
    GW::Constants::MapID map_id = GW::Constants::MapID::None;
    ImU32 color = IM_COL32(255, 0, 0, 255);
    float size = 8.0f;
    std::string label;
    std::string category;
    std::string icon; // FontAwesome icon string, e.g. ICON_FA_SKULL
    std::wstring enc_name; // Encoded string name — decoded at runtime for localization
    std::string spawn_group; // Non-empty: this marker is a spawn location for the named group
    uint32_t item_model_id = 0; // Non-zero: track a bundle item with this model_id
    bool visible = true;
    bool wiki_enabled = true; // Whether clicking opens the GW wiki

    // Transient (not serialized)
    mutable std::string decoded_name;
    mutable bool decode_pending = false;
    mutable std::string english_name; // Always decoded as English for wiki URLs
    mutable bool english_decode_pending = false;
    mutable int detected_npc_idx = -1; // Index into spawn group's NPC list, or -1
    mutable uint32_t detected_agent_id = 0; // Agent ID of detected NPC (0 = none)
    mutable GW::Vec2f detected_world_pos{}; // World map pos of detected agent (updated each frame)
    mutable bool detected_dead = false; // True if detected NPC has died
    mutable GW::GamePos detected_game_pos{}; // Last known game pos (for proximity checks)
    mutable float detected_last_hp = 1.0f; // Last known HP (0-1) before agent disappeared
    mutable uint32_t detected_carrier_agent_id = 0; // Agent carrying the tracked bundle (0 = on ground or not found)
};

enum class TriggerConditionType { PlayerInZone, NpcDetected, NpcDied };
enum class TriggerActionType { Highlight, Dim, Hide };
enum class RouteDisplayState { Normal, Highlighted, Dimmed, Hidden };

struct MapAnnotationRoute {
    uint32_t id = 0;
    GW::Constants::MapID map_id = GW::Constants::MapID::None;
    ImU32 color = IM_COL32(0, 255, 0, 255);
    float thickness = 2.0f;
    std::string label;
    bool show_label = true;
    bool show_direction = false;
    std::string category;
    std::vector<GW::Vec2f> waypoints;
    bool visible = true;
    bool loop = false;
    bool dimmed_by_default = false; // Start dimmed when on this map (explorable only)

    // Transient (not serialized)
    mutable RouteDisplayState display_state = RouteDisplayState::Normal;
};

struct TriggerAction {
    uint32_t route_id = 0;
    TriggerActionType type = TriggerActionType::Highlight;
};

struct MapAnnotationTrigger {
    uint32_t id = 0;
    GW::Constants::MapID map_id = GW::Constants::MapID::None;

    TriggerConditionType condition = TriggerConditionType::PlayerInZone;
    GW::Vec2f world_pos{};       // PlayerInZone: center
    float radius = 0.f;          // PlayerInZone: activation radius in world map units
    std::string spawn_group;     // NpcDetected/NpcDied: spawn group name (optional)
    uint32_t marker_id = 0;      // NpcDetected/NpcDied: specific marker (optional, 0 = use spawn_group)
    bool revert_on_leave = false; // PlayerInZone only

    std::vector<TriggerAction> actions;
    std::string label;

    // Transient
    mutable bool fired = false;
};

struct MapAnnotationLayer {
    std::string name;
    std::string filename;
    bool is_base_layer = false;
    bool visible = true;
    std::vector<MapAnnotationMarker> markers;
    std::vector<MapAnnotationRoute> routes;
    std::vector<SpawnGroupDef> spawn_groups;
    std::vector<MapAnnotationTrigger> triggers;
};

class MapAnnotationsModule : public ToolboxWindow {
public:
    static MapAnnotationsModule& Instance()
    {
        static MapAnnotationsModule instance;
        return instance;
    }

    [[nodiscard]] const char* Name() const override { return "Map Annotations"; }
    [[nodiscard]] const char* Icon() const override { return ICON_FA_MAP_MARKED_ALT; }
    [[nodiscard]] const char* Description() const override { return "Annotate maps with custom markers, routes, and labels"; }

    void Initialize() override;
    void Terminate() override;
    void SignalTerminate() override;
    void Update(float delta) override;
    void Draw(IDirect3DDevice9*) override;
    void LoadSettings(ToolboxIni*) override;
    void SaveSettings(ToolboxIni*) override;
    void DrawSettingsInternal() override;

    // Check if an agent is being tracked by a map annotation
    static bool IsAgentAnnotated(uint32_t agent_id);


    // Check if an annotation is hovered and show its context menu if so.
    // Returns true if a context menu was shown (caller should break).
    static bool ShowAnnotationContextMenu();
    static bool ShowMissionMapAnnotationContextMenu();

    // Context menu callbacks (public for ImGui::SetContextMenu)
    static bool HoveredMarkerContextMenu(void*);
    static bool HoveredRouteContextMenu(void*);
    // Call before opening a context menu to reset annotation snapshot state
    static void ResetContextMenuState();
    // Context menu items for the default world map context menu
    static bool WorldMapContextMenuItems(const GW::Vec2f& click_pos);

    // Route editing
    static void StartRouteAtWorldPos(const GW::Vec2f& world_pos);
    static void AddRouteWaypoint(const GW::Vec2f& world_pos);
    static void FinishRoute();
    static void CancelRoute();
    static bool IsEditingRoute();

    // Add current target as marker
    static void AddTargetAsMarker();

    // Route recording (walk to record)
    static void StartRecordingRoute();
    static void StopRecordingRoute();
    static bool IsRecordingRoute();

    // Layer management
    static std::vector<std::string> GetUserLayerNames();

private:
    MapAnnotationsModule() = default;
    ~MapAnnotationsModule() override = default;
};
