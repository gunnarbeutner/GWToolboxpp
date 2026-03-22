#include "stdafx.h"

#include <Windows/HotkeysWindow.h>
#include <Windows/Hotkeys/HotkeyGroup.h>
#include <Widgets/HotkeyGroupWidget.h>

namespace {
    float background_opacity = 0.3f;
    float button_height = 45.0f;

    struct IconOption {
        const char* icon;
        const char* label;
    };

    const IconOption icon_options[] = {
        { "", "None" },
        { ICON_FA_FIST_RAISED, "Aggressive" },
        { ICON_FA_SHIELD_ALT, "Guard/Defend" },
        { ICON_FA_FLAG, "Flag" },
        { ICON_FA_CROSSHAIRS, "Target" },
        { ICON_FA_BOLT, "Attack" },
        { ICON_FA_HEART, "Heal" },
        { ICON_FA_MAGIC, "Buff" },
        { ICON_FA_RUNNING, "Move" },
        { ICON_FA_USERS, "Party" },
        { ICON_FA_SKULL, "Kill" },
        { ICON_FA_FIRE, "AoE" },
        { ICON_FA_EXCHANGE_ALT, "Swap" },
        { ICON_FA_COG, "Utility" },
        { ICON_FA_STAR, "Favorite" },
        { ICON_FA_PLAY, "Execute" },
        { ICON_FA_STOP, "Stop" },
        { ICON_FA_EYE, "Watch" },
        { ICON_FA_MAP_MARKER_ALT, "Position" },
        { ICON_FA_DOVE, "Passive" },
    };

    constexpr int icon_count = sizeof(icon_options) / sizeof(icon_options[0]);

    std::unordered_map<std::string, int> group_icons;

    // Build "icon label" display string for combo items
    std::string IconComboLabel(int idx)
    {
        if (idx <= 0 || idx >= icon_count) return "None";
        return std::string(icon_options[idx].icon) + " " + icon_options[idx].label;
    }

    std::string ButtonLabel(const std::string& group)
    {
        auto it = group_icons.find(group);
        int idx = (it != group_icons.end()) ? it->second : 0;
        if (idx > 0 && idx < icon_count) {
            return std::string(icon_options[idx].icon) + "   " + group;
        }
        return group;
    }
}

void HotkeyGroupWidget::Draw(IDirect3DDevice9*)
{
    if (!visible) {
        return;
    }

    std::vector<HotkeyGroup*> groups;
    for (auto* hk : TBHotkey::all_hotkeys) {
        if (auto* grp = dynamic_cast<HotkeyGroup*>(hk))
            groups.push_back(grp);
    }

    // Don't draw if there are no named groups
    bool has_named_group = false;
    for (const HotkeyGroup* g : groups) {
        if (g->label[0]) {
            has_named_group = true;
            break;
        }
    }
    if (!has_named_group) {
        return;
    }

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, background_opacity));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, ImGui::GetStyle().WindowPadding.y));
    ImGui::SetNextWindowSize(ImVec2(150.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(Name(), nullptr, GetWinFlags())) {
        for (HotkeyGroup* group : groups) {
            if (!group->label[0]) {
                continue;
            }

            const std::string label = ButtonLabel(group->label);
            const float h = button_height > 0.0f ? button_height : 0.0f;
            if (ImGui::Button(label.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, h))) {
                for (auto* hk : group->hotkeys) {
                    if (!hk->active) continue;
                    hk->pressed = true;
                    hk->Toggle();
                    hk->pressed = false;
                }
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void HotkeyGroupWidget::DrawSettingsInternal()
{
    ImGui::SliderFloat("Background opacity", &background_opacity, 0.0f, 1.0f);
    ImGui::SliderFloat("Button height", &button_height, 0.0f, 100.0f, "%.0f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = auto (fits text)");


    std::vector<HotkeyGroup*> groups;
    for (auto* hk : TBHotkey::all_hotkeys) {
        if (auto* grp = dynamic_cast<HotkeyGroup*>(hk))
            groups.push_back(grp);
    }
    bool has_groups = false;
    for (const HotkeyGroup* g : groups) {
        if (g->label[0]) { has_groups = true; break; }
    }

    if (has_groups) {
        ImGui::Separator();
        ImGui::Text("Group Icons:");
        for (const HotkeyGroup* group : groups) {
            if (!group->label[0]) continue;

            const std::string group_name = group->label;
            int& idx = group_icons[group_name];
            const std::string preview = IconComboLabel(idx);
            const std::string combo_id = "##icon_" + group_name;
            ImGui::PushItemWidth(160.0f);
            if (ImGui::BeginCombo((combo_id).c_str(), preview.c_str())) {
                for (int i = 0; i < icon_count; i++) {
                    const std::string item_label = IconComboLabel(i) + "##" + std::to_string(i);
                    if (ImGui::Selectable(item_label.c_str(), idx == i)) {
                        idx = i;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::PopItemWidth();
            ImGui::SameLine();
            ImGui::TextUnformatted(group_name.c_str());
        }
    }
}

void HotkeyGroupWidget::LoadSettings(ToolboxIni* ini)
{
    ToolboxWidget::LoadSettings(ini);
    background_opacity = static_cast<float>(ini->GetDoubleValue(Name(), "background_opacity", 0.3));
    button_height = static_cast<float>(ini->GetDoubleValue(Name(), "button_height", 45.0));

    group_icons.clear();
    TNamesDepend keys;
    ini->GetAllKeys(Name(), keys);
    for (const auto& key : keys) {
        const char* k = key.pItem;
        if (strncmp(k, "icon_", 5) == 0) {
            const std::string group_name = k + 5;
            group_icons[group_name] = static_cast<int>(ini->GetLongValue(Name(), k, 0));
        }
    }
}

void HotkeyGroupWidget::SaveSettings(ToolboxIni* ini)
{
    ToolboxWidget::SaveSettings(ini);
    ini->SetDoubleValue(Name(), "background_opacity", background_opacity);
    ini->SetDoubleValue(Name(), "button_height", button_height);

    for (const auto& [group, idx] : group_icons) {
        if (group.empty()) continue;
        const std::string key = "icon_" + group;
        ini->SetLongValue(Name(), key.c_str(), idx);
    }
}
