#pragma once

// The outline and shadow font effects with their lengths in dp. RmlUi reads a font effect's lengths
// as pixels whatever their unit, so an outline written in dp would stay as thin at any window size
// while the text grows with the dp ratio. These take RmlUi's own place under the same names, turn
// the dp lengths into pixels at the context's ratio and hand the effect to RmlUi's own instancer.
// Host main thread only; called by ui.cpp.

namespace ui::font_effects
{
    // Puts the instancers in place of RmlUi's "outline" and "shadow". After Rml::Initialise; they
    // stay registered until the process ends.
    void Register();

    // The dp ratio the next style sheets are parsed at; returns whether it changed. A style sheet
    // already parsed keeps its lengths: the caller clears RmlUi's style sheet cache and reloads the
    // open documents' style sheets. RmlUi keeps a size's outline glyphs by the length as written, so
    // a change that leaves a font's pixel size as it was may keep the old outline, at most a pixel off.
    bool SetDensityRatio(float ratio);
}
