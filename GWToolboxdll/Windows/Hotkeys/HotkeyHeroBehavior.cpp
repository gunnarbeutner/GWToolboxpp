#include "stdafx.h"

#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/Party.h>
#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/GameThreadMgr.h>
#include <GWCA/Managers/PartyMgr.h>

#include <ImGuiAddons.h>

#include <Constants/EncStrings.h>
#include <Modules/ChatFilter.h>
#include <Windows/Hotkeys/HotkeyHeroBehavior.h>
#include <Windows/Hotkeys/HotkeyHeroFormation.h>

namespace {
    constexpr std::array behaviors = {
        "Fight",
        "Guard",
        "Avoid Combat"
    };

    const char* GetBehaviorDesc(GW::HeroBehavior behaviour)
    {
        if (std::to_underlying(behaviour) < behaviors.size()) {
            return behaviors[std::to_underlying(behaviour)];
        }
        return nullptr;
    }
}

HotkeyHeroBehavior::HotkeyHeroBehavior(const ToolboxIni* ini, const char* section)
    : TBHotkey(ini, section)
{
    behavior = ini ? static_cast<GW::HeroBehavior>(ini->GetLongValue(section, "behavior", static_cast<long>(behavior))) : behavior;
    if (ini) {
        const uint32_t lo = static_cast<uint32_t>(ini->GetLongValue(section, "hero_ids_lo", 0));
        const uint32_t hi = static_cast<uint32_t>(ini->GetLongValue(section, "hero_ids_hi", 0));
        hero_ids = (static_cast<uint64_t>(hi) << 32) | lo;
    }
    if (!GetBehaviorDesc(behavior)) {
        behavior = GW::HeroBehavior::Guard;
    }
}

void HotkeyHeroBehavior::Save(ToolboxIni* ini, const char* section) const
{
    TBHotkey::Save(ini, section);
    ini->SetLongValue(section, "behavior", static_cast<long>(behavior));
    ini->SetLongValue(section, "hero_ids_lo", static_cast<long>(hero_ids & 0xFFFFFFFF));
    ini->SetLongValue(section, "hero_ids_hi", static_cast<long>((hero_ids >> 32) & 0xFFFFFFFF));
}

int HotkeyHeroBehavior::Description(char* buf, const size_t bufsz)
{
    if (hero_ids == 0) {
        return snprintf(buf, bufsz, "Hero Behavior: All %s", GetBehaviorDesc(behavior));
    }
    return snprintf(buf, bufsz, "Hero Behavior: %s", GetBehaviorDesc(behavior));
}

bool HotkeyHeroBehavior::Draw()
{
    bool changed = false;
    changed |= ImGui::Combo("Behavior###combo", reinterpret_cast<int*>(&behavior), behaviors.data(), behaviors.size(), behaviors.size());
    if (changed && !GetBehaviorDesc(behavior)) {
        behavior = GW::HeroBehavior::Guard;
    }

    ImGui::Text("Heroes (none checked = all):");
    constexpr int columns = 3;
    if (ImGui::BeginTable("##heroes", columns, ImGuiTableFlags_None)) {
        for (size_t h = 1; h < HotkeyHeroFormation::kHeroCount; h++) {
            const char* hero_name = HotkeyHeroFormation::GetHeroName(h);
            if (hero_name[0] == '\0') continue;
            ImGui::TableNextColumn();
            bool checked = (hero_ids >> h) & 1;
            if (ImGui::Checkbox(hero_name, &checked)) {
                if (checked) {
                    hero_ids |= (1ULL << h);
                } else {
                    hero_ids &= ~(1ULL << h);
                }
                changed = true;
            }
        }
        ImGui::EndTable();
    }

    return changed;
}

void HotkeyHeroBehavior::Execute()
{
    if (!isExplorable()) return;
    GW::GameThread::Enqueue([&] {
        const auto* party = GW::PartyMgr::GetPartyInfo();
        if (!party) return;
        const auto& heroes = party->heroes;
        if (!heroes.valid()) return;
        const auto* me = GW::Agents::GetControlledCharacter();
        if (!me) return;
        constexpr clock_t SUPPRESS_MS = 1000;
        ChatFilter::BlockMessageForMs(GW::EncStrings::HeroBehavior::Fight, SUPPRESS_MS);
        ChatFilter::BlockMessageForMs(GW::EncStrings::HeroBehavior::Guard, SUPPRESS_MS);
        ChatFilter::BlockMessageForMs(GW::EncStrings::HeroBehavior::Avoid, SUPPRESS_MS);
        for (size_t i = 0; i < heroes.size(); i++) {
            if (heroes[i].owner_player_id != me->login_number) continue;
            if (hero_ids != 0 && !((hero_ids >> static_cast<size_t>(heroes[i].hero_id)) & 1)) continue;
            GW::PartyMgr::SetHeroBehavior(heroes[i].agent_id, behavior);
        }
    });
}
