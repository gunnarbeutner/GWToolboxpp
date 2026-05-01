#pragma once

#include <Defines.h>
#include <ToolboxUIElement.h>
#include <Utils/SettingsDoc.h>

class ToolboxWindow : public ToolboxUIElement {
public:
    [[nodiscard]] bool IsWindow() const override { return true; }
    [[nodiscard]] const char* TypeName() const override { return "window"; }

    void Initialize() override
    {
        ToolboxUIElement::Initialize();
        has_closebutton = true;
        has_pinbutton = true;
        has_titlebar = true;
    }

    void LoadSettings(SettingsDoc& doc, ToolboxIni* legacy) override
    {
        ToolboxUIElement::LoadSettings(doc, legacy);
        doc.Get(Name(), "show_closebutton", show_closebutton);
        doc.Get(Name(), "pinned", pinned);
        doc.Get(Name(), "show_pinbutton", show_pinbutton);
    }

    void SaveSettings(SettingsDoc& doc) override
    {
        ToolboxUIElement::SaveSettings(doc);
        doc.Set(Name(), "show_closebutton", show_closebutton);
        doc.Set(Name(), "pinned", pinned);
        doc.Set(Name(), "show_pinbutton", show_pinbutton);
    }
};
