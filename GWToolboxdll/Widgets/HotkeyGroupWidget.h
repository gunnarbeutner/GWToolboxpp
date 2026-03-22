#pragma once

#include <ToolboxWidget.h>

class HotkeyGroupWidget : public ToolboxWidget {
    HotkeyGroupWidget() = default;
    ~HotkeyGroupWidget() override = default;

public:
    static HotkeyGroupWidget& Instance()
    {
        static HotkeyGroupWidget instance;
        return instance;
    }

    [[nodiscard]] const char* Name() const override { return "Hotkey Groups"; }
    [[nodiscard]] const char* Icon() const override { return ICON_FA_KEYBOARD; }

    void Draw(IDirect3DDevice9* pDevice) override;
    void DrawSettingsInternal() override;
    void LoadSettings(ToolboxIni* ini) override;
    void SaveSettings(ToolboxIni* ini) override;
};
