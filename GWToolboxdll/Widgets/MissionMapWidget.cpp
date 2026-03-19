#include "stdafx.h"

#include <GWCA/Context/GameplayContext.h>
#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/NPC.h>
#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/GameThreadMgr.h>
#include <GWCA/GameEntities/Pathing.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/UIMgr.h>
#include <GWCA/Utilities/Hooker.h>

#include <Widgets/MissionMapWidget.h>
#include <Widgets/WorldMapWidget.h>
#include <Widgets/Minimap/Minimap.h>
#include <Modules/QuestModule.h>
#include <ImGuiAddons.h>

namespace {
    bool draw_all_terrain_lines = false;
    bool draw_all_minimap_lines = true;
    bool show_enemy_markers = true;
    bool show_exploration_overlay = true;
    bool show_vq_overlay = false; // master toggle for all VQ features on mission map

    // Enemy tracking for VQ assistance
    enum class EnemyState { Alive, Stale };
    struct TrackedEnemy {
        GW::GamePos pos;
        GW::Vec2f velocity = {0.0f, 0.0f}; // last known movement direction
        EnemyState state = EnemyState::Alive;
    };
    std::unordered_map<uint32_t, TrackedEnemy> tracked_enemies; // agent_id -> tracked enemy
    GW::Constants::MapID tracked_enemies_map_id = static_cast<GW::Constants::MapID>(0);
    GW::Constants::InstanceType tracked_enemies_instance_type = GW::Constants::InstanceType::Loading;

    constexpr float TWO_PI = 6.2831853f;
    constexpr float COMPASS_RANGE = 5000.0f;
    constexpr float STALE_CHECK_RANGE = COMPASS_RANGE * 0.9f; // slightly less than compass to avoid edge flickering

    // Exploration tracking (fog of war)
    constexpr float EXPLORE_CELL_SIZE = 250.0f; // game units per grid cell
    std::unordered_set<uint64_t> explored_cells;
    GW::Constants::MapID explored_map_id = static_cast<GW::Constants::MapID>(0);
    GW::Constants::InstanceType explored_instance_type = GW::Constants::InstanceType::Loading;

    uint64_t CellKey(int cx, int cy)
    {
        return (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) | static_cast<uint32_t>(cy);
    }

    void UpdateExploration(const GW::GamePos& player_pos)
    {
        const auto map_id = GW::Map::GetMapID();
        const auto instance_type = GW::Map::GetInstanceType();
        if (map_id != explored_map_id || instance_type != explored_instance_type) {
            explored_cells.clear();
            explored_map_id = map_id;
            explored_instance_type = instance_type;
        }

        if (GW::Map::GetInstanceType() != GW::Constants::InstanceType::Explorable)
            return;

        // Mark all cells within compass range as explored
        const int range_cells = static_cast<int>(COMPASS_RANGE / EXPLORE_CELL_SIZE) + 1;
        const int player_cx = static_cast<int>(floorf(player_pos.x / EXPLORE_CELL_SIZE));
        const int player_cy = static_cast<int>(floorf(player_pos.y / EXPLORE_CELL_SIZE));
        const float range_sq = COMPASS_RANGE * COMPASS_RANGE;

        for (int dx = -range_cells; dx <= range_cells; dx++) {
            for (int dy = -range_cells; dy <= range_cells; dy++) {
                // Check if center of cell is within compass range
                const float cell_center_x = (player_cx + dx + 0.5f) * EXPLORE_CELL_SIZE;
                const float cell_center_y = (player_cy + dy + 0.5f) * EXPLORE_CELL_SIZE;
                const float ddx = cell_center_x - player_pos.x;
                const float ddy = cell_center_y - player_pos.y;
                if (ddx * ddx + ddy * ddy <= range_sq) {
                    explored_cells.insert(CellKey(player_cx + dx, player_cy + dy));
                }
            }
        }
    }

    // Map border: cached walkable grid (in game coords)
    // Recomputed when map changes.
    struct BorderSegment { GW::GamePos p1, p2; };
    std::vector<BorderSegment> cached_border_segments;
    std::vector<bool> cached_walkable_grid;
    int cached_grid_x0 = 0, cached_grid_y0 = 0, cached_grid_w = 0, cached_grid_h = 0;
    GW::Constants::MapID border_map_id = static_cast<GW::Constants::MapID>(0);
    constexpr float BORDER_CELL_SIZE = 300.0f; // game units per grid cell for border rasterization

    bool IsGridCellWalkable(int cx, int cy) {
        cx -= cached_grid_x0; cy -= cached_grid_y0;
        if (cx < 0 || cx >= cached_grid_w || cy < 0 || cy >= cached_grid_h) return false;
        return cached_walkable_grid[cy * cached_grid_w + cx];
    }


    // Check if a grid cell [cx0,cx1] x [cy0,cy1] overlaps a trapezoid
    bool CellOverlapsTrapezoid(float cx0, float cy0, float cx1, float cy1, const GW::PathingTrapezoid& trap)
    {
        // No Y overlap?
        if (cy1 <= trap.YB || cy0 >= trap.YT) return false;
        // Clamp to the Y overlap range
        const float y_lo = std::max(cy0, trap.YB);
        const float y_hi = std::min(cy1, trap.YT);
        const float height = trap.YT - trap.YB;
        if (height < 0.001f) {
            // Degenerate: check if X ranges overlap at YB
            return cx1 > std::min(trap.XBL, trap.XTL) && cx0 < std::max(trap.XBR, trap.XTR);
        }
        // Compute trapezoid X range at y_lo and y_hi, take the union
        auto trap_x_range = [&](float y, float& left, float& right) {
            const float t = (y - trap.YB) / height;
            left = trap.XBL + t * (trap.XTL - trap.XBL);
            right = trap.XBR + t * (trap.XTR - trap.XBR);
        };
        float l0, r0, l1, r1;
        trap_x_range(y_lo, l0, r0);
        trap_x_range(y_hi, l1, r1);
        const float trap_left = std::min(l0, l1);
        const float trap_right = std::max(r0, r1);
        return cx1 > trap_left && cx0 < trap_right;
    }

    void RebuildMapBorder()
    {
        cached_border_segments.clear();
        cached_walkable_grid.clear();
        const auto pathing_map = GW::Map::GetPathingMap();
        if (!pathing_map) return;

        // Collect all trapezoids
        std::vector<const GW::PathingTrapezoid*> traps;
        float min_x = FLT_MAX, min_y = FLT_MAX, max_x = -FLT_MAX, max_y = -FLT_MAX;
        for (size_t p = 0; p < pathing_map->size(); p++) {
            const auto& plane = pathing_map->at(p);
            for (uint32_t t = 0; t < plane.trapezoid_count; t++) {
                const auto& trap = plane.trapezoids[t];
                traps.push_back(&trap);
                min_x = std::min({min_x, trap.XTL, trap.XTR, trap.XBL, trap.XBR});
                max_x = std::max({max_x, trap.XTL, trap.XTR, trap.XBL, trap.XBR});
                min_y = std::min(min_y, trap.YB);
                max_y = std::max(max_y, trap.YT);
            }
        }
        if (traps.empty()) return;

        // Build grid
        cached_grid_x0 = static_cast<int>(floorf(min_x / BORDER_CELL_SIZE)) - 1;
        cached_grid_y0 = static_cast<int>(floorf(min_y / BORDER_CELL_SIZE)) - 1;
        const int grid_x1 = static_cast<int>(ceilf(max_x / BORDER_CELL_SIZE)) + 1;
        const int grid_y1 = static_cast<int>(ceilf(max_y / BORDER_CELL_SIZE)) + 1;
        cached_grid_w = grid_x1 - cached_grid_x0;
        cached_grid_h = grid_y1 - cached_grid_y0;

        // Mark walkable cells - mark any cell that overlaps a trapezoid
        cached_walkable_grid.assign(cached_grid_w * cached_grid_h, false);
        for (const auto* trap : traps) {
            const int ty0 = static_cast<int>(floorf(trap->YB / BORDER_CELL_SIZE)) - cached_grid_y0;
            const int ty1 = static_cast<int>(ceilf(trap->YT / BORDER_CELL_SIZE)) - cached_grid_y0;
            const int tx0 = static_cast<int>(floorf(std::min({trap->XTL, trap->XBL}) / BORDER_CELL_SIZE)) - cached_grid_x0;
            const int tx1 = static_cast<int>(ceilf(std::max({trap->XTR, trap->XBR}) / BORDER_CELL_SIZE)) - cached_grid_x0;
            for (int cy = std::max(0, ty0); cy <= std::min(cached_grid_h - 1, ty1); cy++) {
                for (int cx = std::max(0, tx0); cx <= std::min(cached_grid_w - 1, tx1); cx++) {
                    if (cached_walkable_grid[cy * cached_grid_w + cx]) continue;
                    const float cell_x0 = (cached_grid_x0 + cx) * BORDER_CELL_SIZE;
                    const float cell_y0 = (cached_grid_y0 + cy) * BORDER_CELL_SIZE;
                    if (CellOverlapsTrapezoid(cell_x0, cell_y0, cell_x0 + BORDER_CELL_SIZE, cell_y0 + BORDER_CELL_SIZE, *trap)) {
                        cached_walkable_grid[cy * cached_grid_w + cx] = true;
                    }
                }
            }
        }

        // Extract border edges: walkable cell next to non-walkable neighbor
        for (int cy = 0; cy < cached_grid_h; cy++) {
            for (int cx = 0; cx < cached_grid_w; cx++) {
                if (!cached_walkable_grid[cy * cached_grid_w + cx]) continue;
                const float x0 = (cached_grid_x0 + cx) * BORDER_CELL_SIZE;
                const float y0 = (cached_grid_y0 + cy) * BORDER_CELL_SIZE;
                const float x1 = x0 + BORDER_CELL_SIZE;
                const float y1 = y0 + BORDER_CELL_SIZE;

                if (!IsGridCellWalkable(cached_grid_x0 + cx, cached_grid_y0 + cy - 1))
                    cached_border_segments.push_back({{x0, y0, 0}, {x1, y0, 0}});
                if (!IsGridCellWalkable(cached_grid_x0 + cx, cached_grid_y0 + cy + 1))
                    cached_border_segments.push_back({{x0, y1, 0}, {x1, y1, 0}});
                if (!IsGridCellWalkable(cached_grid_x0 + cx - 1, cached_grid_y0 + cy))
                    cached_border_segments.push_back({{x0, y0, 0}, {x0, y1, 0}});
                if (!IsGridCellWalkable(cached_grid_x0 + cx + 1, cached_grid_y0 + cy))
                    cached_border_segments.push_back({{x1, y0, 0}, {x1, y1, 0}});
            }
        }

    }

    // Navigation to closest enemy
    bool nav_active = false;
    GW::GamePos nav_target_pos;       // current enemy position (tracked continuously)
    GW::GamePos nav_marker_pos;       // where we last placed the quest marker
    bool nav_marker_set = false;      // true once the quest marker has been placed at least once
    bool nav_marker_hidden = false;   // true when marker is hidden because enemy is on compass

    constexpr float MARKER_UPDATE_DIST = 500.0f; // only move the quest marker if enemy moved this far from last marker pos

    void StopNavigating()
    {
        nav_active = false;
        nav_target_pos = {};
        nav_marker_pos = {};
        nav_marker_set = false;
        nav_marker_hidden = false;
        GW::GameThread::Enqueue([] {
            QuestModule::SetCustomQuestMarker({0, 0});
        });
    }

    void SetNavTarget(const GW::GamePos& target)
    {
        nav_active = true;
        nav_target_pos = target;

        // Check if the target enemy is within compass range - hide the marker if so
        const auto player = GW::Agents::GetControlledCharacter();
        if (player) {
            const float dx = target.x - player->pos.x;
            const float dy = target.y - player->pos.y;
            constexpr float HIDE_RANGE = COMPASS_RANGE * 0.5f;
            const bool in_compass = dx * dx + dy * dy < HIDE_RANGE * HIDE_RANGE;

            if (in_compass && !nav_marker_hidden) {
                // Hide the marker
                nav_marker_hidden = true;
                GW::GameThread::Enqueue([] {
                    QuestModule::SetCustomQuestMarker({0, 0});
                });
                return;
            }
            if (in_compass) return; // already hidden

            // Enemy is outside compass - show/update the marker
            if (nav_marker_hidden) {
                nav_marker_hidden = false;
                nav_marker_set = false; // force marker update
            }
        }

        // Only update the quest marker if the target moved significantly from where the marker is
        const float dx = target.x - nav_marker_pos.x;
        const float dy = target.y - nav_marker_pos.y;
        if (nav_marker_set && dx * dx + dy * dy < MARKER_UPDATE_DIST * MARKER_UPDATE_DIST) {
            return; // Marker is close enough, don't re-set it
        }

        nav_marker_pos = target;
        nav_marker_set = true;
        GW::Vec2f world_pos;
        if (WorldMapWidget::GamePosToWorldMap(target, world_pos)) {
            GW::GameThread::Enqueue([world_pos] {
                QuestModule::SetCustomQuestMarker(world_pos, true);
            });
        }
    }

    void NavigateToClosestEnemy()
    {
        const auto player = GW::Agents::GetControlledCharacter();
        if (!player) return;

        const GW::GamePos player_pos = player->pos;

        // Find closest enemy by straight-line distance
        float best_dist_sq = FLT_MAX;
        GW::GamePos best_pos = {};
        bool found = false;

        for (const auto& [agent_id, enemy] : tracked_enemies) {
            const float dx = enemy.pos.x - player_pos.x;
            const float dy = enemy.pos.y - player_pos.y;
            const float dist_sq = dx * dx + dy * dy;
            if (dist_sq < best_dist_sq) {
                best_dist_sq = dist_sq;
                best_pos = enemy.pos;
                found = true;
            }
        }

        if (!found) {
            // No enemies known yet - hide marker but keep nav mode active
            if (!nav_marker_hidden) {
                nav_marker_hidden = true;
                GW::GameThread::Enqueue([] {
                    QuestModule::SetCustomQuestMarker({0, 0});
                });
            }
            return;
        }

        SetNavTarget(best_pos);
    }

    void UpdateEnemyTracking()
    {
        const auto map_id = GW::Map::GetMapID();
        const auto instance_type = GW::Map::GetInstanceType();
        if (map_id != tracked_enemies_map_id || instance_type != tracked_enemies_instance_type) {
            tracked_enemies.clear();
            tracked_enemies_map_id = map_id;
            tracked_enemies_instance_type = instance_type;
            if (nav_active) {
                StopNavigating();
            }
        }

        if (instance_type != GW::Constants::InstanceType::Explorable)
            return;

        const auto player = GW::Agents::GetControlledCharacter();
        if (!player) return;

        const auto agents = GW::Agents::GetAgentArray();
        if (!agents) return;

        // Track which agent_ids are currently visible
        std::unordered_set<uint32_t> visible_enemy_ids;

        for (const auto agent : *agents) {
            if (!agent) continue;
            const auto living = agent->GetAsAgentLiving();
            if (!living) continue;
            if (living->allegiance != GW::Constants::Allegiance::Enemy) continue;
            // Skip spirits and minions via NPC definition flags
            const auto npc = GW::Agents::GetNPCByID(living->player_number);
            if (npc && (npc->IsSpirit() || npc->IsMinion())) continue;

            visible_enemy_ids.insert(agent->agent_id);

            if (!living->GetIsAlive()) {
                tracked_enemies.erase(agent->agent_id);
                continue;
            }

            auto& tracked = tracked_enemies[agent->agent_id];
            tracked.pos = living->pos;
            tracked.velocity = living->velocity;
            tracked.state = EnemyState::Alive;
        }

        // Mark enemies as stale if we're in range of their last known position
        // but they're no longer in the agent array.
        // Skip stale detection on the respawn frame - agents near the death location
        // vanish from the array when we teleport to a resurrection shrine.
        const auto* player_living = player->GetAsAgentLiving();
        static bool was_dead = false;
        const bool is_dead = !player_living || !player_living->GetIsAlive();
        if (is_dead) { was_dead = true; return; }
        if (was_dead) { was_dead = false; return; } // skip one frame after respawn

        const GW::GamePos player_pos = player->pos;
        for (auto& [agent_id, enemy] : tracked_enemies) {
            if (visible_enemy_ids.contains(agent_id)) continue;
            if (enemy.state != EnemyState::Alive) continue;

            const float dx = enemy.pos.x - player_pos.x;
            const float dy = enemy.pos.y - player_pos.y;
            if (dx * dx + dy * dy < STALE_CHECK_RANGE * STALE_CHECK_RANGE) {
                enemy.state = EnemyState::Stale;
            }
        }

        // Auto-refresh navigation: re-evaluate target each frame.
        // SetNavTarget gates the actual quest marker update to avoid flicker.
        if (nav_active) {
            NavigateToClosestEnemy();
        }
    }

    GW::Vec2f mission_map_top_left;
    GW::Vec2f mission_map_bottom_right;
    GW::Vec2f mission_map_scale;
    float mission_map_zoom;

    GW::Vec2f mission_map_center_pos;
    GW::Vec2f mission_map_last_click_location;
    GW::Vec2f current_pan_offset;

    GW::Vec2f mission_map_screen_pos;
    GW::Vec2f world_map_click_pos;

    MinimapRenderContext mission_map_render_context;

    bool right_clicking = false;

    GW::UI::Frame* mission_map_frame = nullptr;

    bool IsScreenPosOnMissionMap(const GW::Vec2f& screen_pos)
    {
        if (!(mission_map_frame && mission_map_frame->IsVisible()))
            return false;
        return (screen_pos.x >= mission_map_top_left.x && screen_pos.x <= mission_map_bottom_right.x &&
                screen_pos.y >= mission_map_top_left.y && screen_pos.y <= mission_map_bottom_right.y);
    }

    GW::Vec2f GetMissionMapScreenCenterPos()
    {
        return mission_map_top_left + (mission_map_bottom_right - mission_map_top_left) / 2;
    }

    // Given the world pos, calculate where the mission map would draw this on-screen
    bool WorldMapCoordsToMissionMapScreenPos(const GW::Vec2f& world_map_position, GW::Vec2f& screen_coords)
    {
        const auto offset = (world_map_position - current_pan_offset);
        const auto scaled_offset = GW::Vec2f(offset.x * mission_map_scale.x, offset.y * mission_map_scale.y);
        screen_coords = (scaled_offset * mission_map_zoom + mission_map_screen_pos);
        return true;
    }

    // Given the on-screen pos, calculate where the mission map coords land
    GW::Vec2f ScreenPosToMissionMapCoords(const GW::Vec2f screen_position)
    {
        GW::Vec2f unscaled_offset = (screen_position - mission_map_screen_pos) / mission_map_zoom;
        GW::Vec2f offset(
            unscaled_offset.x / mission_map_scale.x,
            unscaled_offset.y / mission_map_scale.y
        );
        return offset + current_pan_offset;
    }

    //
    bool GamePosToMissionMapScreenPos(const GW::GamePos& game_map_position, GW::Vec2f& screen_coords)
    {
        GW::Vec2f world_map_pos;
        return WorldMapWidget::GamePosToWorldMap(game_map_position, world_map_pos) && WorldMapCoordsToMissionMapScreenPos(world_map_pos, screen_coords);
    }

    void Draw(IDirect3DDevice9*);

    std::vector<GW::UI::UIMessage> messages_hit;
    GW::UI::UIInteractionCallback OnMissionMap_UICallback_Func = nullptr, OnMissionMap_UICallback_Ret = nullptr;

    void OnMissionMap_UICallback(GW::UI::InteractionMessage* message, void* wparam, void* lparam)
    {
        GW::Hook::EnterHook();
        switch (message->message_id) {
            case GW::UI::UIMessage::kInitFrame:
                OnMissionMap_UICallback_Ret(message, wparam, lparam);
                mission_map_frame = GW::UI::GetFrameById(message->frame_id);
                break;
            case GW::UI::UIMessage::kDestroyFrame:
                mission_map_frame = nullptr;
                OnMissionMap_UICallback_Ret(message, wparam, lparam);
                break;
            default:
                OnMissionMap_UICallback_Ret(message, wparam, lparam);
                break;
        }
        GW::Hook::LeaveHook();
    }

    bool HookMissionMapFrame()
    {
        if (OnMissionMap_UICallback_Func)
            return true;

        const auto mission_map_context = GW::Map::GetMissionMapContext();
        mission_map_frame = mission_map_context ? GW::UI::GetFrameById(mission_map_context->frame_id) : nullptr;
        if (!(mission_map_frame && mission_map_frame->frame_callbacks[0].callback))
            return false;
        OnMissionMap_UICallback_Func = mission_map_frame->frame_callbacks[0].callback;
        GW::Hook::CreateHook((void**)&OnMissionMap_UICallback_Func, OnMissionMap_UICallback, (void**)&OnMissionMap_UICallback_Ret);
        GW::Hook::EnableHooks(OnMissionMap_UICallback_Func);
        return true;
    }

    bool InitializeMissionMapParameters()
    {
        const auto gameplay_context = GW::GetGameplayContext();
        const auto mission_map_context = GW::Map::GetMissionMapContext();
        if (!(gameplay_context && mission_map_frame && mission_map_frame->IsVisible()))
            return false;

        const auto root = GW::UI::GetRootFrame();
        mission_map_top_left = mission_map_frame->position.GetContentTopLeft(root);
        mission_map_bottom_right = mission_map_frame->position.GetContentBottomRight(root);
        mission_map_scale = mission_map_frame->position.GetViewportScale(root);
        mission_map_zoom = gameplay_context->mission_map_zoom;
        mission_map_center_pos = mission_map_context->player_mission_map_pos;
        mission_map_last_click_location = mission_map_context->last_mouse_location;
        current_pan_offset = mission_map_context->h003c->mission_map_pan_offset;
        mission_map_screen_pos = GetMissionMapScreenCenterPos();
        return true;
    }

    bool MissionMapContextMenu(void*)
    {
        if (!(mission_map_frame && mission_map_frame->IsVisible()))
            return false;
        const auto c = ImGui::GetCurrentContext();
        auto viewport_offset = c->CurrentViewport->Pos;
        viewport_offset.x *= -1;
        viewport_offset.y *= -1;

        ImGui::Text("%.2f, %.2f", world_map_click_pos.x, world_map_click_pos.y);
#ifdef _DEBUG
        GW::GamePos game_pos;
        if (WorldMapWidget::WorldMapToGamePos(world_map_click_pos, game_pos)) {
            ImGui::Text("%.2f, %.2f", game_pos.x, game_pos.y);
        }
#endif
        if (ImGui::Button("Place Marker")) {
            nav_active = false;
            nav_target_pos = {};
            nav_marker_pos = {};
            GW::GameThread::Enqueue([] {
                QuestModule::SetCustomQuestMarker(world_map_click_pos, true);
            });
            return false;
        }
        if (QuestModule::GetCustomQuestMarker() && !nav_active) {
            if (ImGui::Button("Remove Marker")) {
                GW::GameThread::Enqueue([] {
                    QuestModule::SetCustomQuestMarker({0, 0});
                });
                return false;
            }
        }
        if (show_enemy_markers) {
            if (nav_active) {
                if (ImGui::Button("Stop navigating")) {
                    StopNavigating();
                    return false;
                }
            } else {
                if (ImGui::Button("Navigate to closest enemy")) {
                    nav_active = true;
                    NavigateToClosestEnemy();
                    return false;
                }
            }
        }
        return true;
    }

    void Draw(IDirect3DDevice9* dx_device)
    {
        if (!HookMissionMapFrame())
            return;
        if (!InitializeMissionMapParameters())
            return;

#ifdef _DEBUG

        mission_map_render_context.top_left = mission_map_top_left;
        mission_map_render_context.bottom_right = mission_map_bottom_right;
        // NB: 104.f is the mission map scale per gwinch
        mission_map_render_context.base_scale = mission_map_scale.x * 104.f * mission_map_zoom;
        mission_map_render_context.zoom_scale = 1.f;
        mission_map_render_context.foreground_color = D3DCOLOR_ARGB(100, 0xe0, 0xe0, 0xe0);

        // Anchor point is where the player appears on screen in the mission map
        GW::Vec2f player_screen_pos;
        WorldMapCoordsToMissionMapScreenPos(mission_map_center_pos, player_screen_pos);
        mission_map_render_context.anchor_point = {player_screen_pos.x, player_screen_pos.y};


        Minimap::Render(dx_device, mission_map_render_context);
#endif


        struct Vertex {
            float x, y, z, w;
            DWORD color;
        };

        const auto& lines = Minimap::Instance().custom_renderer.GetLines();
        const auto map_id = GW::Map::GetMapID();

        std::vector<Vertex> line_vertices;
        constexpr float LINE_HALF_THICKNESS = 1.5f; // 3px wide route lines

        for (const auto& line : lines) {
            if (!line->visible) continue;
            if (!line->draw_on_mission_map &&
                !(draw_all_minimap_lines && line->draw_on_minimap) &&
                !(draw_all_terrain_lines && line->draw_on_terrain))
                continue;
            if (line->map != map_id) continue;

            GW::Vec2f projected_p1, projected_p2;
            if (!GamePosToMissionMapScreenPos(line->p1, projected_p1))
                continue;
            if (!GamePosToMissionMapScreenPos(line->p2, projected_p2))
                continue;

            const DWORD color = static_cast<DWORD>(line->color);
            float dx = projected_p2.x - projected_p1.x;
            float dy = projected_p2.y - projected_p1.y;
            const float len = sqrtf(dx * dx + dy * dy);
            if (len < 0.001f) continue;
            const float nx = -dy / len * LINE_HALF_THICKNESS;
            const float ny =  dx / len * LINE_HALF_THICKNESS;

            const Vertex v0 = {projected_p1.x + nx, projected_p1.y + ny, 0.0f, 1.0f, color};
            const Vertex v1 = {projected_p1.x - nx, projected_p1.y - ny, 0.0f, 1.0f, color};
            const Vertex v2 = {projected_p2.x - nx, projected_p2.y - ny, 0.0f, 1.0f, color};
            const Vertex v3 = {projected_p2.x + nx, projected_p2.y + ny, 0.0f, 1.0f, color};
            line_vertices.push_back(v0); line_vertices.push_back(v1); line_vertices.push_back(v2);
            line_vertices.push_back(v0); line_vertices.push_back(v2); line_vertices.push_back(v3);
        }

        // Build fog of war + map border from cached grid data
        const bool in_explorable = GW::Map::GetInstanceType() == GW::Constants::InstanceType::Explorable;
        std::vector<Vertex> fog_vertices;
        std::vector<Vertex> border_vertices;
        if (show_vq_overlay && in_explorable) {
            // Rebuild cache if map changed
            const auto cur_map_id = GW::Map::GetMapID();
            if (cur_map_id != border_map_id) {
                border_map_id = cur_map_id;
                RebuildMapBorder();
            }

            // Inaccessible: dark quads on non-walkable cells within bounding box
            constexpr DWORD INACCESSIBLE_COLOR = D3DCOLOR_ARGB(190, 0, 0, 0);
            // Unexplored walkable: lighter than inaccessible but still fogged
            constexpr DWORD FOG_UNEXPLORED     = D3DCOLOR_ARGB(140, 0, 0, 0);
            // Explored walkable: nothing drawn (original map shows through)

            // Compute visible grid range from mission map viewport corners
            GW::GamePos viewport_tl_game, viewport_br_game;
            GW::Vec2f wm_tl = ScreenPosToMissionMapCoords(mission_map_top_left);
            GW::Vec2f wm_br = ScreenPosToMissionMapCoords(mission_map_bottom_right);
            // Convert world map coords to game coords for grid range
            WorldMapWidget::WorldMapToGamePos(wm_tl, viewport_tl_game);
            WorldMapWidget::WorldMapToGamePos(wm_br, viewport_br_game);
            const float vis_min_x = std::min(viewport_tl_game.x, viewport_br_game.x);
            const float vis_max_x = std::max(viewport_tl_game.x, viewport_br_game.x);
            const float vis_min_y = std::min(viewport_tl_game.y, viewport_br_game.y);
            const float vis_max_y = std::max(viewport_tl_game.y, viewport_br_game.y);
            const int vis_gx0 = static_cast<int>(floorf(vis_min_x / BORDER_CELL_SIZE)) - 1;
            const int vis_gy0 = static_cast<int>(floorf(vis_min_y / BORDER_CELL_SIZE)) - 1;
            const int vis_gx1 = static_cast<int>(ceilf(vis_max_x / BORDER_CELL_SIZE)) + 1;
            const int vis_gy1 = static_cast<int>(ceilf(vis_max_y / BORDER_CELL_SIZE)) + 1;

            // Draw inaccessible shade on non-walkable cells (clamped to cached grid bounds)
            const int clamp_gx0 = std::max(vis_gx0, cached_grid_x0);
            const int clamp_gy0 = std::max(vis_gy0, cached_grid_y0);
            const int clamp_gx1 = std::min(vis_gx1, cached_grid_x0 + cached_grid_w - 1);
            const int clamp_gy1 = std::min(vis_gy1, cached_grid_y0 + cached_grid_h - 1);
            for (int gy = clamp_gy0; gy <= clamp_gy1; gy++) {
                for (int gx = clamp_gx0; gx <= clamp_gx1; gx++) {
                    if (IsGridCellWalkable(gx, gy)) continue;
                    const float x0 = gx * BORDER_CELL_SIZE;
                    const float y0 = gy * BORDER_CELL_SIZE;
                    const GW::GamePos corners[4] = {
                        {x0, y0, 0}, {x0 + BORDER_CELL_SIZE, y0, 0},
                        {x0 + BORDER_CELL_SIZE, y0 + BORDER_CELL_SIZE, 0}, {x0, y0 + BORDER_CELL_SIZE, 0},
                    };
                    GW::Vec2f screen[4];
                    bool all_ok = true;
                    for (int i = 0; i < 4; i++) {
                        if (!GamePosToMissionMapScreenPos(corners[i], screen[i])) { all_ok = false; break; }
                    }
                    if (!all_ok) continue;
                    fog_vertices.push_back({screen[0].x, screen[0].y, 0.0f, 1.0f, INACCESSIBLE_COLOR});
                    fog_vertices.push_back({screen[1].x, screen[1].y, 0.0f, 1.0f, INACCESSIBLE_COLOR});
                    fog_vertices.push_back({screen[2].x, screen[2].y, 0.0f, 1.0f, INACCESSIBLE_COLOR});
                    fog_vertices.push_back({screen[0].x, screen[0].y, 0.0f, 1.0f, INACCESSIBLE_COLOR});
                    fog_vertices.push_back({screen[2].x, screen[2].y, 0.0f, 1.0f, INACCESSIBLE_COLOR});
                    fog_vertices.push_back({screen[3].x, screen[3].y, 0.0f, 1.0f, INACCESSIBLE_COLOR});
                }
            }

            // Draw compass range circle around player (below frontier edges)
            {
                const auto player = GW::Agents::GetControlledCharacter();
                if (player) {
                    constexpr int CIRCLE_SEGMENTS = 64;
                    constexpr DWORD CIRCLE_COLOR = D3DCOLOR_ARGB(100, 180, 220, 255);
                    constexpr float CIRCLE_THICKNESS = 0.5f;

                    for (int i = 0; i < CIRCLE_SEGMENTS; i++) {
                        const float a1 = static_cast<float>(i) / CIRCLE_SEGMENTS * TWO_PI;
                        const float a2 = static_cast<float>(i + 1) / CIRCLE_SEGMENTS * TWO_PI;
                        const GW::GamePos p1 = {player->pos.x + COMPASS_RANGE * cosf(a1), player->pos.y + COMPASS_RANGE * sinf(a1), 0};
                        const GW::GamePos p2 = {player->pos.x + COMPASS_RANGE * cosf(a2), player->pos.y + COMPASS_RANGE * sinf(a2), 0};
                        GW::Vec2f s1, s2;
                        if (!GamePosToMissionMapScreenPos(p1, s1) || !GamePosToMissionMapScreenPos(p2, s2))
                            continue;
                        float dx = s2.x - s1.x;
                        float dy = s2.y - s1.y;
                        const float len = sqrtf(dx * dx + dy * dy);
                        if (len < 0.001f) continue;
                        const float nx = -dy / len * CIRCLE_THICKNESS;
                        const float ny =  dx / len * CIRCLE_THICKNESS;
                        fog_vertices.push_back({s1.x + nx, s1.y + ny, 0.0f, 1.0f, CIRCLE_COLOR});
                        fog_vertices.push_back({s1.x - nx, s1.y - ny, 0.0f, 1.0f, CIRCLE_COLOR});
                        fog_vertices.push_back({s2.x - nx, s2.y - ny, 0.0f, 1.0f, CIRCLE_COLOR});
                        fog_vertices.push_back({s1.x + nx, s1.y + ny, 0.0f, 1.0f, CIRCLE_COLOR});
                        fog_vertices.push_back({s2.x - nx, s2.y - ny, 0.0f, 1.0f, CIRCLE_COLOR});
                        fog_vertices.push_back({s2.x + nx, s2.y + ny, 0.0f, 1.0f, CIRCLE_COLOR});
                    }
                }
            }

            // Frontier edge: bright line where explored meets unexplored
            constexpr DWORD FRONTIER_COLOR = D3DCOLOR_ARGB(200, 255, 200, 50);
            constexpr float FRONTIER_HALF_THICKNESS = 1.5f;

            auto is_cell_explored = [&](int gx, int gy) -> bool {
                if (!show_exploration_overlay) return false;
                const float cx = (gx + 0.5f) * BORDER_CELL_SIZE;
                const float cy = (gy + 0.5f) * BORDER_CELL_SIZE;
                const int ecx = static_cast<int>(floorf(cx / EXPLORE_CELL_SIZE));
                const int ecy = static_cast<int>(floorf(cy / EXPLORE_CELL_SIZE));
                return explored_cells.contains(CellKey(ecx, ecy));
            };

            auto push_frontier_edge = [&](const GW::Vec2f& s1, const GW::Vec2f& s2) {
                float dx = s2.x - s1.x;
                float dy = s2.y - s1.y;
                const float len = sqrtf(dx * dx + dy * dy);
                if (len < 0.001f) return;
                const float nx = -dy / len * FRONTIER_HALF_THICKNESS;
                const float ny =  dx / len * FRONTIER_HALF_THICKNESS;
                const Vertex v0 = {s1.x + nx, s1.y + ny, 0.0f, 1.0f, FRONTIER_COLOR};
                const Vertex v1 = {s1.x - nx, s1.y - ny, 0.0f, 1.0f, FRONTIER_COLOR};
                const Vertex v2 = {s2.x - nx, s2.y - ny, 0.0f, 1.0f, FRONTIER_COLOR};
                const Vertex v3 = {s2.x + nx, s2.y + ny, 0.0f, 1.0f, FRONTIER_COLOR};
                fog_vertices.push_back(v0); fog_vertices.push_back(v1); fog_vertices.push_back(v2);
                fog_vertices.push_back(v0); fog_vertices.push_back(v2); fog_vertices.push_back(v3);
            };

            for (int gy = 0; gy < cached_grid_h; gy++) {
                for (int gx = 0; gx < cached_grid_w; gx++) {
                    if (!cached_walkable_grid[gy * cached_grid_w + gx]) continue;
                    const int abs_gx = cached_grid_x0 + gx;
                    const int abs_gy = cached_grid_y0 + gy;
                    const float x0 = abs_gx * BORDER_CELL_SIZE;
                    const float y0 = abs_gy * BORDER_CELL_SIZE;
                    const GW::GamePos corners[4] = {
                        {x0, y0, 0}, {x0 + BORDER_CELL_SIZE, y0, 0},
                        {x0 + BORDER_CELL_SIZE, y0 + BORDER_CELL_SIZE, 0}, {x0, y0 + BORDER_CELL_SIZE, 0},
                    };
                    GW::Vec2f screen[4];
                    bool all_ok = true;
                    for (int i = 0; i < 4; i++) {
                        if (!GamePosToMissionMapScreenPos(corners[i], screen[i])) { all_ok = false; break; }
                    }
                    if (!all_ok) continue;

                    const bool explored = is_cell_explored(abs_gx, abs_gy);
                    if (explored) {
                        // Nothing drawn - original map shows through
                    } else {
                        fog_vertices.push_back({screen[0].x, screen[0].y, 0.0f, 1.0f, FOG_UNEXPLORED});
                        fog_vertices.push_back({screen[1].x, screen[1].y, 0.0f, 1.0f, FOG_UNEXPLORED});
                        fog_vertices.push_back({screen[2].x, screen[2].y, 0.0f, 1.0f, FOG_UNEXPLORED});
                        fog_vertices.push_back({screen[0].x, screen[0].y, 0.0f, 1.0f, FOG_UNEXPLORED});
                        fog_vertices.push_back({screen[2].x, screen[2].y, 0.0f, 1.0f, FOG_UNEXPLORED});
                        fog_vertices.push_back({screen[3].x, screen[3].y, 0.0f, 1.0f, FOG_UNEXPLORED});

                        // Draw frontier edges where this unexplored cell borders an explored walkable cell
                        const float x1 = x0 + BORDER_CELL_SIZE;
                        const float y1 = y0 + BORDER_CELL_SIZE;
                        auto check_neighbor = [&](int nx, int ny, const GW::GamePos& ep1, const GW::GamePos& ep2) {
                            if (!IsGridCellWalkable(cached_grid_x0 + nx, cached_grid_y0 + ny)) return;
                            if (!is_cell_explored(cached_grid_x0 + nx, cached_grid_y0 + ny)) return;
                            GW::Vec2f es1, es2;
                            if (GamePosToMissionMapScreenPos(ep1, es1) && GamePosToMissionMapScreenPos(ep2, es2))
                                push_frontier_edge(es1, es2);
                        };
                        check_neighbor(gx, gy - 1, {x0, y0, 0}, {x1, y0, 0});
                        check_neighbor(gx, gy + 1, {x0, y1, 0}, {x1, y1, 0});
                        check_neighbor(gx - 1, gy, {x0, y0, 0}, {x0, y1, 0});
                        check_neighbor(gx + 1, gy, {x1, y0, 0}, {x1, y1, 0});
                    }
                }
            }

            constexpr DWORD BORDER_COLOR = D3DCOLOR_ARGB(160, 200, 220, 255);
            constexpr float HALF_THICKNESS = 0.5f;

            const float screen_left = mission_map_top_left.x;
            const float screen_top = mission_map_top_left.y;
            const float screen_right = mission_map_bottom_right.x;
            const float screen_bottom = mission_map_bottom_right.y;

            for (const auto& seg : cached_border_segments) {
                GW::Vec2f s1, s2;
                if (!GamePosToMissionMapScreenPos(seg.p1, s1) ||
                    !GamePosToMissionMapScreenPos(seg.p2, s2))
                    continue;

                // Skip segments entirely outside the viewport
                if ((s1.x < screen_left && s2.x < screen_left) ||
                    (s1.x > screen_right && s2.x > screen_right) ||
                    (s1.y < screen_top && s2.y < screen_top) ||
                    (s1.y > screen_bottom && s2.y > screen_bottom))
                    continue;

                float dx = s2.x - s1.x;
                float dy = s2.y - s1.y;
                const float len = sqrtf(dx * dx + dy * dy);
                if (len < 0.001f) continue;
                const float nx = -dy / len * HALF_THICKNESS;
                const float ny =  dx / len * HALF_THICKNESS;

                const Vertex v0 = {s1.x + nx, s1.y + ny, 0.0f, 1.0f, BORDER_COLOR};
                const Vertex v1 = {s1.x - nx, s1.y - ny, 0.0f, 1.0f, BORDER_COLOR};
                const Vertex v2 = {s2.x - nx, s2.y - ny, 0.0f, 1.0f, BORDER_COLOR};
                const Vertex v3 = {s2.x + nx, s2.y + ny, 0.0f, 1.0f, BORDER_COLOR};
                border_vertices.push_back(v0); border_vertices.push_back(v1); border_vertices.push_back(v2);
                border_vertices.push_back(v0); border_vertices.push_back(v2); border_vertices.push_back(v3);
            }
        }

        // Build enemy marker triangles (small diamonds)
        std::vector<Vertex> enemy_vertices;
        if (show_vq_overlay && in_explorable && show_enemy_markers) {
            constexpr float MARKER_SIZE = 9.0f;
            constexpr float OUTLINE_SIZE = MARKER_SIZE + 2.0f;
            constexpr float HALO_SIZE = MARKER_SIZE + 8.0f;
            constexpr DWORD COLOR_OUTLINE   = D3DCOLOR_ARGB(200, 0, 0, 0);
            constexpr DWORD COLOR_ALIVE     = D3DCOLOR_ARGB(255, 70, 130, 255);
            constexpr DWORD COLOR_STALE     = D3DCOLOR_ARGB(180, 255, 180, 50);

            auto push_diamond = [&](float cx, float cy, float size, DWORD color) {
                const Vertex top    = {cx, cy - size, 0.0f, 1.0f, color};
                const Vertex right  = {cx + size, cy, 0.0f, 1.0f, color};
                const Vertex bottom = {cx, cy + size, 0.0f, 1.0f, color};
                const Vertex left   = {cx - size, cy, 0.0f, 1.0f, color};
                enemy_vertices.push_back(top);    enemy_vertices.push_back(right);  enemy_vertices.push_back(bottom);
                enemy_vertices.push_back(bottom);  enemy_vertices.push_back(left);   enemy_vertices.push_back(top);
            };

            // Gradient halo: center is semi-transparent color, edges fade to fully transparent
            constexpr int HALO_SEGMENTS = 12;
            auto push_halo = [&](float cx, float cy, float radius, DWORD center_color) {
                // Extract RGB, use lower alpha for center, 0 for edge
                const DWORD edge_color = center_color & 0x00FFFFFF; // alpha = 0
                const Vertex center_v = {cx, cy, 0.0f, 1.0f, center_color};
                for (int i = 0; i < HALO_SEGMENTS; i++) {
                    const float a1 = static_cast<float>(i) / HALO_SEGMENTS * TWO_PI;
                    const float a2 = static_cast<float>(i + 1) / HALO_SEGMENTS * TWO_PI;
                    const Vertex v1 = {cx + radius * cosf(a1), cy + radius * sinf(a1), 0.0f, 1.0f, edge_color};
                    const Vertex v2 = {cx + radius * cosf(a2), cy + radius * sinf(a2), 0.0f, 1.0f, edge_color};
                    enemy_vertices.push_back(center_v);
                    enemy_vertices.push_back(v1);
                    enemy_vertices.push_back(v2);
                }
            };

            constexpr float ARROW_LENGTH = 28.0f;
            constexpr float ARROW_HALF_WIDTH = 6.0f;
            constexpr DWORD COLOR_ARROW = D3DCOLOR_ARGB(240, 255, 220, 50);
            constexpr DWORD COLOR_ARROW_OUTLINE = D3DCOLOR_ARGB(240, 0, 0, 0);

            auto push_velocity_arrow = [&](float cx, float cy, const TrackedEnemy& enemy) {
                const float vlen_sq = enemy.velocity.x * enemy.velocity.x + enemy.velocity.y * enemy.velocity.y;
                if (vlen_sq < 1.0f) return; // not moving

                // Convert velocity direction to screen space by projecting a second point
                GW::GamePos offset_pos = enemy.pos;
                const float vlen = std::sqrt(vlen_sq);
                offset_pos.x += enemy.velocity.x / vlen * 500.0f; // arbitrary offset in game coords
                offset_pos.y += enemy.velocity.y / vlen * 500.0f;
                GW::Vec2f screen_offset;
                if (!GamePosToMissionMapScreenPos(offset_pos, screen_offset)) return;

                // Screen-space direction
                float dx = screen_offset.x - cx;
                float dy = screen_offset.y - cy;
                const float slen = std::sqrt(dx * dx + dy * dy);
                if (slen < 0.1f) return;
                dx /= slen; dy /= slen;

                // Arrow tip is ARROW_LENGTH from center, perpendicular for the base
                const float tip_x = cx + dx * ARROW_LENGTH;
                const float tip_y = cy + dy * ARROW_LENGTH;
                const float base_x = cx + dx * (MARKER_SIZE * 0.5f);
                const float base_y = cy + dy * (MARKER_SIZE * 0.5f);
                const float nx = -dy, ny = dx; // perpendicular

                // Outline (slightly larger)
                constexpr float OL = 2.5f;
                const Vertex ol_tip   = {tip_x + dx * OL, tip_y + dy * OL, 0.0f, 1.0f, COLOR_ARROW_OUTLINE};
                const Vertex ol_left  = {base_x + nx * (ARROW_HALF_WIDTH + OL), base_y + ny * (ARROW_HALF_WIDTH + OL), 0.0f, 1.0f, COLOR_ARROW_OUTLINE};
                const Vertex ol_right = {base_x - nx * (ARROW_HALF_WIDTH + OL), base_y - ny * (ARROW_HALF_WIDTH + OL), 0.0f, 1.0f, COLOR_ARROW_OUTLINE};
                enemy_vertices.push_back(ol_tip);
                enemy_vertices.push_back(ol_left);
                enemy_vertices.push_back(ol_right);

                // Arrow fill
                const Vertex a_tip   = {tip_x, tip_y, 0.0f, 1.0f, COLOR_ARROW};
                const Vertex a_left  = {base_x + nx * ARROW_HALF_WIDTH, base_y + ny * ARROW_HALF_WIDTH, 0.0f, 1.0f, COLOR_ARROW};
                const Vertex a_right = {base_x - nx * ARROW_HALF_WIDTH, base_y - ny * ARROW_HALF_WIDTH, 0.0f, 1.0f, COLOR_ARROW};
                enemy_vertices.push_back(a_tip);
                enemy_vertices.push_back(a_left);
                enemy_vertices.push_back(a_right);
            };

            auto draw_enemy = [&](const TrackedEnemy& enemy) {
                GW::Vec2f screen_pos;
                if (!GamePosToMissionMapScreenPos(enemy.pos, screen_pos))
                    return;

                const bool is_stale = enemy.state == EnemyState::Stale;
                const DWORD color = is_stale ? COLOR_STALE : COLOR_ALIVE;
                const DWORD halo_color = is_stale
                    ? D3DCOLOR_ARGB(60, 255, 180, 50)
                    : D3DCOLOR_ARGB(80, 70, 130, 255);

                const float cx = screen_pos.x, cy = screen_pos.y;
                push_halo(cx, cy, HALO_SIZE, halo_color);
                push_diamond(cx, cy, OUTLINE_SIZE, COLOR_OUTLINE);
                push_diamond(cx, cy, MARKER_SIZE, color);

                // Draw velocity arrow on stale enemies
                if (enemy.state == EnemyState::Stale) {
                    push_velocity_arrow(cx, cy, enemy);
                }
            };

            // Draw stale first, then alive (alive on top)
            for (const auto& [agent_id, enemy] : tracked_enemies) {
                if (enemy.state == EnemyState::Stale) draw_enemy(enemy);
            }
            for (const auto& [agent_id, enemy] : tracked_enemies) {
                if (enemy.state == EnemyState::Alive) draw_enemy(enemy);
            }
        }

        // Draw VQ toggle button (always, even when overlay is off)
        if (GW::Map::GetInstanceType() == GW::Constants::InstanceType::Explorable) {
            constexpr float PADDING = 4.0f;
            const float btn_size = ImGui::GetTextLineHeight() + PADDING * 2;
            const ImVec2 btn_pos(
                mission_map_top_left.x + PADDING,
                mission_map_bottom_right.y - btn_size - PADDING
            );

            ImGui::SetNextWindowPos(btn_pos);
            ImGui::SetNextWindowSize({0, 0});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {2, 2});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, {0, 0});
            if (ImGui::Begin("##vq_toggle", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
                if (show_vq_overlay) {
                    if (ImGui::Button(ICON_FA_SKULL "##vq_off")) {
                        show_vq_overlay = false;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("VQ overlay active. Click to hide.");
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                    if (ImGui::Button(ICON_FA_SKULL "##vq_on")) {
                        show_vq_overlay = true;
                    }
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("VQ overlay hidden. Click to show.");
                }
            }
            ImGui::End();
            ImGui::PopStyleVar(2);
        }

        if (line_vertices.empty() && enemy_vertices.empty() && fog_vertices.empty() && border_vertices.empty())
            return;

        // Save render states
        DWORD oldAlphaBlend, oldSrcBlend, oldDestBlend, oldScissorTest, oldFVF;
        RECT oldScissorRect;
        dx_device->GetRenderState(D3DRS_ALPHABLENDENABLE, &oldAlphaBlend);
        dx_device->GetRenderState(D3DRS_SRCBLEND, &oldSrcBlend);
        dx_device->GetRenderState(D3DRS_DESTBLEND, &oldDestBlend);
        dx_device->GetRenderState(D3DRS_SCISSORTESTENABLE, &oldScissorTest);
        dx_device->GetScissorRect(&oldScissorRect);
        dx_device->GetFVF(&oldFVF);

        // Enable scissor testing for clipping
        dx_device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);

        RECT scissorRect;
        scissorRect.left = static_cast<LONG>(mission_map_top_left.x);
        scissorRect.top = static_cast<LONG>(mission_map_top_left.y);
        scissorRect.right = static_cast<LONG>(mission_map_bottom_right.x);
        scissorRect.bottom = static_cast<LONG>(mission_map_bottom_right.y);
        dx_device->SetScissorRect(&scissorRect);

        dx_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dx_device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dx_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        dx_device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);

        if (!fog_vertices.empty()) {
            dx_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, fog_vertices.size() / 3, fog_vertices.data(), sizeof(Vertex));
        }
        if (!border_vertices.empty()) {
            dx_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, border_vertices.size() / 3, border_vertices.data(), sizeof(Vertex));
        }
        if (!line_vertices.empty()) {
            dx_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, line_vertices.size() / 3, line_vertices.data(), sizeof(Vertex));
        }
        if (!enemy_vertices.empty()) {
            dx_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, enemy_vertices.size() / 3, enemy_vertices.data(), sizeof(Vertex));
        }

        // Restore render states
        dx_device->SetFVF(oldFVF);
        dx_device->SetRenderState(D3DRS_DESTBLEND, oldDestBlend);
        dx_device->SetRenderState(D3DRS_SRCBLEND, oldSrcBlend);
        dx_device->SetRenderState(D3DRS_ALPHABLENDENABLE, oldAlphaBlend);
        dx_device->SetRenderState(D3DRS_SCISSORTESTENABLE, oldScissorTest);

        // Draw alive enemy count in bottom-left of mission map
        if (show_vq_overlay && in_explorable && show_enemy_markers) {
            int alive_count = 0;
            int stale_count = 0;
            for (const auto& [id, enemy] : tracked_enemies) {
                if (enemy.state == EnemyState::Alive) alive_count++;
                else if (enemy.state == EnemyState::Stale) stale_count++;
            }

            const uint32_t foes_remaining = GW::Map::GetFoesToKill();
            const bool has_vq_data = foes_remaining > 0 || GW::Map::GetFoesKilled() > 0;

            if (alive_count > 0 || stale_count > 0 || (has_vq_data && foes_remaining > 0)) {
                char label[128];
                int pos = 0;
                if (alive_count > 0 || stale_count > 0) {
                    pos += snprintf(label + pos, sizeof(label) - pos, "%d", alive_count);
                    if (stale_count > 0) {
                        pos += snprintf(label + pos, sizeof(label) - pos, " (+%d?)", stale_count);
                    }
                    if (has_vq_data) {
                        pos += snprintf(label + pos, sizeof(label) - pos, " / %u remaining", foes_remaining);
                    }
                } else {
                    pos += snprintf(label + pos, sizeof(label) - pos, "%u remaining", foes_remaining);
                }

                constexpr float PADDING = 8.0f;
                const float btn_size = ImGui::GetTextLineHeight() + 12.0f;
                const ImVec2 text_pos(
                    mission_map_top_left.x + PADDING + btn_size,
                    mission_map_bottom_right.y - ImGui::GetTextLineHeight() - PADDING
                );

                auto* draw_list = ImGui::GetBackgroundDrawList();
                // Shadow for contrast
                draw_list->AddText({text_pos.x + 1, text_pos.y + 1}, IM_COL32(0, 0, 0, 220), label);
                draw_list->AddText({text_pos.x - 1, text_pos.y - 1}, IM_COL32(0, 0, 0, 220), label);
                draw_list->AddText({text_pos.x + 1, text_pos.y - 1}, IM_COL32(0, 0, 0, 220), label);
                draw_list->AddText({text_pos.x - 1, text_pos.y + 1}, IM_COL32(0, 0, 0, 220), label);
                // Text
                draw_list->AddText(text_pos, IM_COL32(255, 255, 255, 255), label);
            }
        }
    }
}

void MissionMapWidget::LoadSettings(ToolboxIni* ini)
{
    ToolboxWidget::LoadSettings(ini);
    LOAD_BOOL(draw_all_terrain_lines);
    LOAD_BOOL(draw_all_minimap_lines);
    LOAD_BOOL(show_enemy_markers);
    LOAD_BOOL(show_exploration_overlay);
    LOAD_BOOL(show_vq_overlay);
}

void MissionMapWidget::SaveSettings(ToolboxIni* ini)
{
    ToolboxWidget::SaveSettings(ini);
    SAVE_BOOL(draw_all_terrain_lines);
    SAVE_BOOL(draw_all_minimap_lines);
    SAVE_BOOL(show_enemy_markers);
    SAVE_BOOL(show_exploration_overlay);
    SAVE_BOOL(show_vq_overlay);
}

void MissionMapWidget::Draw(IDirect3DDevice9* dx_device)
{
    if (show_vq_overlay) {
        UpdateEnemyTracking();
        if (const auto player = GW::Agents::GetControlledCharacter()) {
            UpdateExploration(player->pos);
        }
    }
    if (visible)
        ::Draw(dx_device);
    HookMissionMapFrame();
}

void MissionMapWidget::DrawSettingsInternal()
{
    ImGui::Checkbox("Draw all terrain lines", &draw_all_terrain_lines);
    ImGui::Checkbox("Draw all minimap lines", &draw_all_minimap_lines);
    ImGui::Checkbox("Show enemy markers on mission map", &show_enemy_markers);
    ImGui::ShowHelp("Tracks enemy positions as they enter compass range.\nBlue = alive, Orange = last known (moved away).\nArrows on orange markers show last movement direction.");
    ImGui::Checkbox("Show exploration overlay on mission map", &show_exploration_overlay);
    ImGui::ShowHelp("Highlights areas you've explored during this session on the mission map.");
}

bool MissionMapWidget::WndProc(const UINT Message, WPARAM, LPARAM lParam)
{
    switch (Message) {
        case WM_GW_RBUTTONCLICK:
            GW::Vec2f cursor_pos = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (!IsScreenPosOnMissionMap(cursor_pos))
                break;
            if (GW::UI::GetCurrentTooltip())
                break;
            world_map_click_pos = ScreenPosToMissionMapCoords(cursor_pos);
            ImGui::SetContextMenu(MissionMapContextMenu);
            break;
    }
    return false;
}

void MissionMapWidget::Terminate()
{
    ToolboxWidget::Terminate();
    if (OnMissionMap_UICallback_Func)
        GW::Hook::RemoveHook(OnMissionMap_UICallback_Func);
    OnMissionMap_UICallback_Func = nullptr;
}
