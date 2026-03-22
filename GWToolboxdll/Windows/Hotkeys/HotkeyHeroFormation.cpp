#include "stdafx.h"

#include <random>

#include <GWCA/Constants/Constants.h>
#include <GWCA/Context/WorldContext.h>
#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/Party.h>
#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/PartyMgr.h>

#include <DirectXMath.h>
#include <ImGuiAddons.h>

#include <Utils/GuiUtils.h>
#include <Windows/Hotkeys/HotkeyHeroFormation.h>
#include <Windows/Pathfinding/Pathing.h>

namespace {
    // Hero names indexed by GW::Constants::HeroID enum value
    constexpr const char* formation_hero_names[] = {
        "", "Norgu", "Goren", "Tahlkora", "Master of Whispers", "Acolyte Jin", "Koss",
        "Dunkoro", "Acolyte Sousuke", "Melonni", "Zhed Shadowhoof", "General Morgahn",
        "Margrid the Sly", "Zenmai", "Olias", "Razah", "M.O.X.",
        "Keiran Thackeray", "Jora", "Pyre Fierceshot", "Anton", "Livia", "Hayda", "Kahmu",
        "Gwen", "Xandra", "Vekk", "Ogden Stonehealer",
        "Merc 1", "Merc 2", "Merc 3", "Merc 4", "Merc 5", "Merc 6", "Merc 7", "Merc 8",
        "Miku", "Zei Ri", "Devona", "Ghost of Althea"
    };
    static_assert(std::size(formation_hero_names) == HotkeyHeroFormation::kHeroCount);

    // Cached merc hero names resolved from party agent names
    std::unique_ptr<GuiUtils::EncString> merc_enc_names[8];
    uint32_t merc_agent_ids[8] = {};

    // Dot colors for the 7 formation slots (RGBA)
    constexpr ImU32 slot_colors[] = {
        IM_COL32(255, 100, 100, 255), // red
        IM_COL32(100, 255, 100, 255), // green
        IM_COL32(100, 150, 255, 255), // blue
        IM_COL32(255, 255, 100, 255), // yellow
        IM_COL32(255, 150, 50, 255),  // orange
        IM_COL32(200, 100, 255, 255), // purple
        IM_COL32(100, 255, 255, 255), // cyan
    };
}

const char* HotkeyHeroFormation::GetHeroName(size_t hero_id)
{
    if (hero_id >= kHeroCount) return "";
    // Merc heroes are indices 28-35: get name from agent when in party
    if (hero_id >= 28 && hero_id <= 35) {
        const int m = static_cast<int>(hero_id) - 28;
        const GW::PartyInfo* party = GW::PartyMgr::GetPartyInfo();
        const GW::AgentLiving* me = GW::Agents::GetControlledCharacter();
        if (party && me) {
            const auto& heroes = party->heroes;
            if (heroes.valid()) {
                for (size_t i = 0; i < heroes.size(); i++) {
                    if (heroes[i].owner_player_id == me->login_number &&
                        heroes[i].hero_id == static_cast<GW::Constants::HeroID>(hero_id) &&
                        heroes[i].agent_id) {
                        if (merc_agent_ids[m] != heroes[i].agent_id) {
                            merc_agent_ids[m] = heroes[i].agent_id;
                            const wchar_t* enc = GW::Agents::GetAgentEncName(heroes[i].agent_id);
                            if (enc) {
                                merc_enc_names[m] = std::make_unique<GuiUtils::EncString>(enc);
                            }
                        }
                        if (merc_enc_names[m]) {
                            const auto& s = merc_enc_names[m]->string();
                            if (!s.empty()) return s.c_str();
                        }
                        break;
                    }
                }
            }
        }
    }
    return formation_hero_names[hero_id];
}

int HotkeyHeroFormation::GetPartyIndexForHero(const GW::Constants::HeroID hero_id)
{
    const GW::PartyInfo* party_info = GW::PartyMgr::GetPartyInfo();
    if (!party_info) return -1;
    const GW::HeroPartyMemberArray& heroes = party_info->heroes;
    if (!heroes.valid()) return -1;
    const GW::AgentLiving* me = GW::Agents::GetControlledCharacter();
    if (!me) return -1;
    const uint32_t my_player_id = me->login_number;
    for (size_t i = 0; i < heroes.size(); i++) {
        if (heroes[i].owner_player_id == my_player_id &&
            heroes[i].hero_id == hero_id) {
            return static_cast<int>(i + 1); // 1-indexed for FlagHero
        }
    }
    return -1;
}

HotkeyHeroFormation::HotkeyHeroFormation(const ToolboxIni* ini, const char* section)
    : TBHotkey(ini, section)
{
    // Default rectangular formation: 2 columns, spread out behind player
    constexpr float default_positions[7][2] = {
        {-400.0f,  200.0f},  // slot 1: left-forward
        { 400.0f,  200.0f},  // slot 2: right-forward
        {-400.0f, -200.0f},  // slot 3: left-back
        { 400.0f, -200.0f},  // slot 4: right-back
        {-400.0f, -600.0f},  // slot 5: left-far back
        { 400.0f, -600.0f},  // slot 6: right-far back
        {   0.0f, -900.0f},  // slot 7: center-rear
    };
    for (int i = 0; i < 7; i++) {
        slots[i].x = default_positions[i][0];
        slots[i].y = default_positions[i][1];
    }

    if (ini) {
        strncpy(name, ini->GetValue(section, "name", name), sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        use_target_direction = ini->GetBoolValue(section, "use_target_direction", use_target_direction);
        for (int i = 0; i < 7; i++) {
            char key[32];
            snprintf(key, sizeof(key), "slot%d_x", i);
            slots[i].x = static_cast<float>(ini->GetDoubleValue(section, key, slots[i].x));
            snprintf(key, sizeof(key), "slot%d_y", i);
            slots[i].y = static_cast<float>(ini->GetDoubleValue(section, key, slots[i].y));
            snprintf(key, sizeof(key), "slot%d_radius", i);
            slots[i].radius = static_cast<float>(ini->GetDoubleValue(section, key, slots[i].radius));
            snprintf(key, sizeof(key), "slot%d_heroes_lo", i);
            const uint32_t lo = static_cast<uint32_t>(ini->GetLongValue(section, key, 0));
            snprintf(key, sizeof(key), "slot%d_heroes_hi", i);
            const uint32_t hi = static_cast<uint32_t>(ini->GetLongValue(section, key, 0));
            slots[i].hero_ids = static_cast<uint64_t>(hi) << 32 | lo;
        }
    }
}

void HotkeyHeroFormation::Save(ToolboxIni* ini, const char* section) const
{
    TBHotkey::Save(ini, section);
    ini->SetValue(section, "name", name);
    ini->SetBoolValue(section, "use_target_direction", use_target_direction);
    for (int i = 0; i < 7; i++) {
        char key[32];
        snprintf(key, sizeof(key), "slot%d_x", i);
        ini->SetDoubleValue(section, key, slots[i].x);
        snprintf(key, sizeof(key), "slot%d_y", i);
        ini->SetDoubleValue(section, key, slots[i].y);
        snprintf(key, sizeof(key), "slot%d_radius", i);
        ini->SetDoubleValue(section, key, slots[i].radius);
        snprintf(key, sizeof(key), "slot%d_heroes_lo", i);
        ini->SetLongValue(section, key, static_cast<long>(slots[i].hero_ids & 0xFFFFFFFF));
        snprintf(key, sizeof(key), "slot%d_heroes_hi", i);
        ini->SetLongValue(section, key, static_cast<long>(slots[i].hero_ids >> 32));
    }
}

int HotkeyHeroFormation::Description(char* buf, const size_t bufsz)
{
    return snprintf(buf, bufsz, "Hero Formation: %s", name);
}

bool HotkeyHeroFormation::DrawFormationCanvas()
{
    bool changed = false;
    constexpr const char* zoom_labels[] = { "Aggro (+/- 1010)", "Spirit (+/- 2500)", "Compass (+/- 5000)" };
    constexpr float zoom_ranges[] = { 1010.0f, 2500.0f, 5000.0f };
    ImGui::PushItemWidth(180);
    ImGui::Combo("Zoom##canvas_zoom", &zoom_level, zoom_labels, IM_ARRAYSIZE(zoom_labels));
    ImGui::PopItemWidth();
    zoom_level = std::clamp(zoom_level, 0, 2);

    constexpr float canvas_size = 300.0f;
    constexpr float half = canvas_size / 2.0f;
    const float range = zoom_ranges[zoom_level];
    const float scale = canvas_size / (range * 2.0f);
    constexpr float dot_radius = 8.0f;

    const ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    const ImVec2 canvas_end = ImVec2(canvas_pos.x + canvas_size, canvas_pos.y + canvas_size);
    const ImVec2 center(canvas_pos.x + half, canvas_pos.y + half);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(canvas_pos, canvas_end, IM_COL32(30, 30, 30, 255));
    draw_list->AddRect(canvas_pos, canvas_end, IM_COL32(80, 80, 80, 255));
    draw_list->PushClipRect(canvas_pos, canvas_end, true);

    ImGui::InvisibleButton("##canvas", ImVec2(canvas_size, canvas_size), ImGuiButtonFlags_MouseButtonLeft);
    const bool is_hovered = ImGui::IsItemHovered();
    const bool is_active = ImGui::IsItemActive();
    const ImGuiIO& io = ImGui::GetIO();

    if (is_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        bool hit_any = false;
        for (int i = 0; i < 7; i++) {
            const float sx = center.x + slots[i].x * scale;
            const float sy = center.y - slots[i].y * scale;
            const float dx = io.MousePos.x - sx;
            const float dy = io.MousePos.y - sy;
            if (dx * dx + dy * dy < (dot_radius + 4) * (dot_radius + 4)) {
                selected_slot = i;
                hit_any = true;
                break;
            }
        }
        if (!hit_any) {
            selected_slot = -1;
        }
    }

    if (selected_slot >= 0 && selected_slot < 7 && is_active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const float sx = io.MousePos.x - center.x;
        const float sy = center.y - io.MousePos.y;
        slots[selected_slot].x = std::clamp(sx / scale, -range, range);
        slots[selected_slot].y = std::clamp(sy / scale, -range, range);
        changed = true;
    }

    const float aggro_radius = 1010.0f * scale;
    const float spirit_radius = 2500.0f * scale;
    draw_list->AddCircle(center, aggro_radius, IM_COL32(200, 50, 50, 120), 64);
    draw_list->AddCircle(center, spirit_radius, IM_COL32(50, 100, 200, 120), 64);

    draw_list->AddLine(ImVec2(center.x - 10, center.y), ImVec2(center.x + 10, center.y), IM_COL32(255, 255, 255, 80));
    draw_list->AddLine(ImVec2(center.x, center.y - 10), ImVec2(center.x, center.y + 10), IM_COL32(255, 255, 255, 80));

    draw_list->AddTriangleFilled(
        ImVec2(center.x, canvas_pos.y + 5),
        ImVec2(center.x - 6, canvas_pos.y + 15),
        ImVec2(center.x + 6, canvas_pos.y + 15),
        IM_COL32(255, 255, 255, 100));

    draw_list->AddCircleFilled(center, dot_radius, IM_COL32(255, 255, 255, 255));
    draw_list->AddText(ImVec2(center.x - 3, center.y - 5), IM_COL32(0, 0, 0, 255), "P");

    for (int i = 0; i < 7; i++) {
        const float sx = center.x + slots[i].x * scale;
        const float sy = center.y - slots[i].y * scale;
        const ImVec2 dot_pos(sx, sy);
        const ImU32 color = slot_colors[i];

        if (slots[i].radius > 0.0f) {
            const float screen_radius = slots[i].radius * scale;
            draw_list->AddCircle(dot_pos, screen_radius, (color & 0x00FFFFFF) | 0x40000000, 32);
        }

        draw_list->AddCircleFilled(dot_pos, dot_radius, color);
        if (selected_slot == i) {
            draw_list->AddCircle(dot_pos, dot_radius + 3, IM_COL32(255, 255, 255, 255), 12, 2.0f);
        }

        char slot_label[4];
        snprintf(slot_label, sizeof(slot_label), "%d", i + 1);
        draw_list->AddText(ImVec2(sx - 3, sy - 5), IM_COL32(0, 0, 0, 255), slot_label);
    }

    draw_list->PopClipRect();
    return changed;
}

bool HotkeyHeroFormation::DrawHeroAssignment()
{
    if (selected_slot < 0 || selected_slot >= 7) {
        ImGui::TextDisabled("Click a dot above to select a slot");
        return false;
    }

    ImGui::Text("Slot %d Heroes:", selected_slot + 1);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "(%.0f, %.0f)", slots[selected_slot].x, slots[selected_slot].y);

    bool changed = false;
    ImGui::PushItemWidth(120);
    changed |= ImGui::DragFloat("X##slot_x", &slots[selected_slot].x, 10.0f, -3000.0f, 3000.0f, "%.0f");
    ImGui::SameLine();
    changed |= ImGui::DragFloat("Y##slot_y", &slots[selected_slot].y, 10.0f, -3000.0f, 3000.0f, "%.0f");
    ImGui::PopItemWidth();
    ImGui::ShowHelp("+X = right of player, +Y = forward");
    ImGui::PushItemWidth(120);
    changed |= ImGui::DragFloat("Radius##slot_radius", &slots[selected_slot].radius, 5.0f, 0.0f, 1000.0f, "%.0f");
    ImGui::PopItemWidth();
    ImGui::ShowHelp("Random scatter radius in game units.\nEach use picks a random point within this circle.");

    constexpr int columns = 3;
    if (ImGui::BeginTable("##heroes", columns, ImGuiTableFlags_None)) {
        for (size_t h = 1; h < kHeroCount; h++) {
            const char* hero_name = GetHeroName(h);
            if (hero_name[0] == '\0') continue;
            ImGui::TableNextColumn();
            bool checked = (slots[selected_slot].hero_ids >> h) & 1;
            if (ImGui::Checkbox(hero_name, &checked)) {
                if (checked) {
                    slots[selected_slot].hero_ids |= (1ULL << h);
                } else {
                    slots[selected_slot].hero_ids &= ~(1ULL << h);
                }
                changed = true;
            }
        }
        ImGui::EndTable();
    }

    return changed;
}

bool HotkeyHeroFormation::Draw()
{
    bool changed = false;
    changed |= ImGui::InputText("Name##formation_name", name, sizeof(name));
    changed |= ImGui::Checkbox("Use target direction (instead of facing)", &use_target_direction);
    ImGui::ShowHelp("If checked, heroes flag relative to direction toward your target.\nOtherwise uses your character's facing direction.");

    ImGui::Separator();
    ImGui::Text("Formation Editor");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "(drag dots to position, click to select)");

    changed |= DrawFormationCanvas();

    ImGui::Separator();
    changed |= DrawHeroAssignment();

    return changed;
}

void HotkeyHeroFormation::Execute()
{
    if (!isExplorable()) return;

    const GW::AgentLiving* player = GW::Agents::GetControlledCharacter();
    if (!player) return;

    float reference_angle = player->rotation_angle;

    if (use_target_direction) {
        const GW::AgentLiving* target = GW::Agents::GetTargetAsAgentLiving();
        if (target && target != player) {
            const float dx = target->x - player->x;
            const float dy = target->y - player->y;
            reference_angle = std::atan2(dy, dx);
        }
    }

    const float fwd_x = std::cos(reference_angle);
    const float fwd_y = std::sin(reference_angle);
    const float right_x = fwd_y;
    const float right_y = -fwd_x;

    static std::mt19937 rng(std::random_device{}());

    for (int i = 0; i < 7; i++) {
        if (slots[i].hero_ids == 0) continue;

        float wx = player->x + slots[i].x * right_x + slots[i].y * fwd_x;
        float wy = player->y + slots[i].x * right_y + slots[i].y * fwd_y;

        if (slots[i].radius > 0.0f) {
            std::uniform_real_distribution<float> angle_dist(0.0f, 2.0f * DirectX::XM_PI);
            std::uniform_real_distribution<float> dist_dist(0.0f, slots[i].radius);
            const float rand_angle = angle_dist(rng);
            const float rand_dist = std::sqrt(dist_dist(rng) / slots[i].radius) * slots[i].radius;
            wx += rand_dist * std::cos(rand_angle);
            wy += rand_dist * std::sin(rand_angle);
        }

        auto pos = GW::GamePos(wx, wy, 0);

        Pathing::FindClosestPositionOnTrapezoid(pos);

        for (size_t h = 1; h < 64; h++) {
            if (!((slots[i].hero_ids >> h) & 1)) continue;
            const int party_idx = GetPartyIndexForHero(static_cast<GW::Constants::HeroID>(h));
            if (party_idx < 0) continue;
            GW::PartyMgr::FlagHero(static_cast<uint32_t>(party_idx), pos);
        }
    }
}
