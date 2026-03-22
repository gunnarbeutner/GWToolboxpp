#pragma once

#include <GWCA/Constants/Constants.h>

#include <Windows/Hotkeys/TBHotkey.h>

class HotkeyHeroFormation : public TBHotkey {
public:
    struct FormationSlot {
        float x = 0.0f;
        float y = 0.0f;
        float radius = 0.0f;   // random scatter radius (game units)
        uint64_t hero_ids = 0; // bitmask indexed by HeroID enum value
    };

    FormationSlot slots[7]{};
    int selected_slot = -1;
    bool use_target_direction = false;
    int zoom_level = 0; // 0=aggro(+/-1010), 1=spirit(+/-2500), 2=compass(+/-5000)
    char name[64] = "Formation";

    static const char* IniSection() { return "HeroFormation"; }
    [[nodiscard]] const char* Name() const override { return IniSection(); }

    HotkeyHeroFormation(const ToolboxIni* ini, const char* section);

    void Save(ToolboxIni* ini, const char* section) const override;

    bool Draw() override;
    int Description(char* buf, size_t bufsz) override;
    void Execute() override;

    // Shared hero-name lookup (also used by HotkeyHeroBehavior). Resolves
    // mercenary names from party agents when available, otherwise returns the
    // hardcoded label. Returns "" if hero_id is out of range.
    static const char* GetHeroName(size_t hero_id);
    static constexpr size_t kHeroCount = 40;

private:
    bool DrawFormationCanvas();
    bool DrawHeroAssignment();
    static int GetPartyIndexForHero(GW::Constants::HeroID hero_id);
};
