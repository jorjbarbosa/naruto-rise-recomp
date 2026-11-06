#include "menu/game_stage.h"
#include "menu/prompts.h"
#include <RmlUi/Core.h>
#include <algorithm>
#include <cmath>
#include <tuple>
#include <fmt/format.h>
namespace narutorise::menu::game_stage {
namespace {
// The movie's stage, and where its curtains and paper are on it.
constexpr float kStageWidth = 1280, kStageHeight = 720;
constexpr float kPaperLeft = 160, kPaperRight = 1120, kCurtainWidth = 133.333f;
// Each curtain piece reaches this far under its neighbours: the art fades out
// over its two outer pixels, which would show the black behind them.
constexpr float kCurtainOverlap = 3;
constexpr float kEdgeOverPaper = 2;
struct PromptButton {
  const char* id;
  const char* button;  // The atlas name after the family's prefix.
  const char* key;
};
constexpr PromptButton kPromptButtons[] = {
    {"tab-previous", "lb", "PgUp"}, {"tab-next", "rb", "PgDn"}, {"back-prompt", "east", "Esc"}, {"ok-prompt", "south", "Enter"}};
}  // namespace

const Offsets kOutline{{2.5f, 2.5f}, {-2.5f, 2.5f}, {-1.5f, -1.5f}, {1.5f, -1.5f}};
const Offsets kPromptOutline{{1.5f, 1.5f}, {-1.5f, -1.5f}, {1.5f, -1.5f}, {-1.5f, 1.5f}};
const Offsets kValueShadow{{2.5f, 2.5f}};

// Shadows move by whole pixels: one at the offset's whole part, and one a
// pixel further out with the fraction left as its alpha, as the game's
// antialiased copies.
std::string Copies(const Offsets& offsets, float px, uint32_t rgb) {
  std::string effects;
  const auto shadow = [&](float x, float y, float alpha) {
    effects += fmt::format("{}shadow({}px {}px #{:06x}{:02x})", effects.empty() ? "" : ", ", int(x), int(y), rgb,
                           int(std::lround(255 * alpha)));
  };
  for (const auto& [dx, dy] : offsets) {
    const float x = dx * px, y = dy * px, whole = std::floor(std::abs(x)), fraction = std::abs(x) - whole;
    const float sx = x < 0 ? -1.f : 1.f, sy = y < 0 ? -1.f : 1.f;
    if (whole >= 1) shadow(sx * whole, sy * whole, 1);
    if (fraction > .1f) shadow(sx * (whole + 1), sy * (whole + 1), fraction);
  }
  return effects;
}

float Place(Rml::ElementDocument& document, int width, int height, float ratio) {
  // The page fills the game's image, which the native renderer fits at 16:9
  // in the window, with black bars beside or above it.
  const float window_w = width / ratio, window_h = height / ratio;
  const float w = std::min(window_w, window_h * kStageWidth / kStageHeight), h = w * kStageHeight / kStageWidth;
  const float k = w / kStageWidth;
  const auto dp = [](float n) { return fmt::format("{:.3f}dp", n); };
  document.SetProperty("left", dp((window_w - w) / 2)); document.SetProperty("top", dp((window_h - h) / 2));
  document.SetProperty("width", dp(w)); document.SetProperty("height", dp(h));
  document.SetProperty("font-size", dp(k));  // One rem: a stage unit.
  for (const char* id : {"stage-back", "stage"}) {  // The layers under and over the curtains.
    auto* stage = document.GetElementById(id);
    stage->SetProperty("left", dp(0)); stage->SetProperty("top", dp(0));
  }
  const auto place = [&](const char* id, float x, float width_dp) {
    auto* element = document.GetElementById(id);
    element->SetProperty("left", dp(x)); element->SetProperty("width", dp(std::max(0.0f, width_dp)));
  };
  place("paper", kPaperLeft * k, (kPaperRight - kPaperLeft) * k);
  // The edges also cover the paper's lighter outer column.
  const float edge = kPaperLeft - kCurtainWidth + kCurtainOverlap + kEdgeOverPaper;
  place("edge-left", (kCurtainWidth - kCurtainOverlap) * k, edge * k);
  place("edge-right", (kPaperRight - kEdgeOverPaper) * k, edge * k);
  const float tile = kCurtainWidth * k;
  for (const auto& [id, from, toward] : {std::tuple{"curtain-left", kCurtainWidth * k, -1.0f},
                                         std::tuple{"curtain-right", (kStageWidth - kCurtainWidth) * k, 1.0f}}) {
    auto* curtains = document.GetElementById(id);
    while (curtains->GetNumChildren()) curtains->RemoveChild(curtains->GetFirstChild());
    const float span = toward < 0 ? from : w - from;
    for (int n = 0; n * tile < span + tile && n < 16; ++n) {
      auto piece = document.CreateElement("div"); piece->SetClass("curtain", true);
      piece->SetProperty("left", dp((toward < 0 ? from - (n + 1) * tile : from + n * tile) - kCurtainOverlap * k));
      piece->SetProperty("width", dp(tile + 2 * kCurtainOverlap * k));
      curtains->AppendChild(std::move(piece));
    }
  }
  return k;
}

std::string Prompt(std::string_view id, std::string_view image, std::string_view attributes) {
  return fmt::format("<div id='{}' class='prompt {}' {}/>", id, image, attributes);
}

void SetPrompts(Rml::ElementDocument& document, controls::Glyphs glyphs) {
  const bool keyboard = glyphs == controls::Glyphs::Keyboard;
  const bool other_pad = !keyboard && glyphs != controls::Glyphs::Xbox;
  document.SetClass("keys", keyboard);
  document.SetClass("glyphs", other_pad);
  for (const auto& prompt : kPromptButtons) {
    auto* element = document.GetElementById(prompt.id);
    if (!element) continue;
    std::string content = fmt::format("<span>{}</span>", prompt.key);
    if (other_pad) content += GlyphRml(fmt::format("{}-{}", controls::GlyphPrefix(glyphs), prompt.button));
    element->SetInnerRML(content);
  }
}
}  // namespace narutorise::menu::game_stage
