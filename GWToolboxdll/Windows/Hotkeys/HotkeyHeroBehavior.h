#pragma once

#include <GWCA/GameEntities/Hero.h>

#include <Windows/Hotkeys/TBHotkey.h>

class HotkeyHeroBehavior : public TBHotkey {
public:
    GW::HeroBehavior behavior = static_cast<GW::HeroBehavior>(1); // Guard
    uint64_t hero_ids = 0; // bitmask indexed by HeroID enum value (0 = all)

    static const char* IniSection() { return "HeroBehavior"; }
    [[nodiscard]] const char* Name() const override { return IniSection(); }

    HotkeyHeroBehavior(const ToolboxIni* ini, const char* section);

    void Save(ToolboxIni* ini, const char* section) const override;

    bool Draw() override;
    int Description(char* buf, size_t bufsz) override;
    void Execute() override;
};
