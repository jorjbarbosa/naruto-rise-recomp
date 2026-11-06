#include "menu/game_style_controls.h"
#include "menu/game_stage.h"
#include "menu/localization.h"
#include <RmlUi/Core.h>
#include <algorithm>
#include <cmath>
#include <fmt/format.h>
namespace narutorise::menu {
namespace {
// Beside a row, as the settings page's cloud; the content column's rows have
// the sidebar's end on their left, so the cloud stays closer to them.
constexpr float kCloudWidth = 58, kCloudHeight = 37.333f, kCloudSidebar = -39.5f, kCloudDetail = -24;
// The narrow columns have another column on their left: a smaller cloud stays
// inside the row, in the room before its label.
constexpr float kCloudInnerScale = .6f, kCloudInnerStart = -3;
// What a focused control belongs to: the cloud and the red outline go to its row.
Rml::Element* RowOf(Rml::Element* focus) {
  for (auto* element = focus; element; element = element->GetParentNode())
    if (element->IsClassSet("binding-row") || element->IsClassSet("number-row") || element->IsClassSet("device-row"))
      return element;
  return focus;
}
bool Within(const Rml::Element* element, const Rml::Element* container) {
  for (; element; element = element->GetParentNode())
    if (element == container) return true;
  return false;
}
}  // namespace

void GameStyleControls::Attach(Rml::ElementDocument* document) {
  document_ = document;
  stage_ = document->GetElementById("stage");
  cursor_ = document->GetElementById("cursor");
  sidebar_ = document->GetElementById("controls-sidebar");
  detail_ = document->GetElementById("controls-detail");
  document->SetClass("cyrillic", Localization::GetLanguage() == Localization::Language::Russian);
  focused_ = nullptr; placed_ = {};
}

void GameStyleControls::Detach() {
  document_ = nullptr;
  stage_ = cursor_ = sidebar_ = detail_ = focused_ = nullptr;
}

void GameStyleControls::Layout(int width, int height, float ratio) {
  if (!document_) return;
  text_px_ = game_stage::Place(*document_, width, height, ratio) * ratio;
  ApplyTextEffects();
}

void GameStyleControls::Rebuilt() {
  focused_ = nullptr;
  ApplyTextEffects();
  fit_passes_ = kFitPasses;
}

void GameStyleControls::ApplyTextEffects() {
  if (!document_) return;
  using namespace game_stage;
  const auto apply = [&](const char* selector, const std::string& effect) {
    Rml::ElementList elements;
    document_->QuerySelectorAll(elements, selector);
    for (auto* element : elements) element->SetProperty("font-effect", effect);
  };
  const std::string outline = Copies(kOutline, text_px_, kWhite), over = Copies(kOutline, text_px_, kOverOutline);
  apply(".tab, .category-label, #controls-detail .category, .binding-label, .number-label, .device-choice, h2", outline);
  apply(".binding, .number-control span, .step, .category-marker, .identify, .secondary, .primary, .danger",
        Copies(kValueShadow, text_px_, kInk));
  apply("#back-label, #controls-footer", Copies(kPromptOutline, text_px_, kInk));
  if (auto* selected = document_->QuerySelector(".tab.selected")) selected->SetProperty("font-effect", over);
  auto* focus = document_->GetContext() ? document_->GetContext()->GetFocusElement() : nullptr;
  if (auto* row = RowOf(focus); row && row != document_) {
    auto* label = row->QuerySelector(".binding-label");
    if (!label) label = row->QuerySelector(".number-label");
    if (!label) label = row->QuerySelector(".category-label");
    if (!label && !row->IsClassSet("device-row")) label = row;
    if (label && label != document_) label->SetProperty("font-effect", over);
  }
}

bool GameStyleControls::FitText() {
  if (!document_ || fit_passes_ == 0) return false;
  --fit_passes_;
  // Font sizes stay in rem, so they follow later layouts of the stage. Text
  // widths round per glyph, so a size can take more than one pass.
  const float rem = document_->GetComputedValues().font_size();
  if (rem <= 0) return false;
  bool changed = false;
  Rml::ElementList elements;
  document_->QuerySelectorAll(elements, ".tab, .category-label, #controls-detail .category, .binding, .binding-label, .number-label, .device-choice, "
                                        ".identify, .secondary, .primary, .danger, h2, #controls-footer");
  for (auto* element : elements) {
    const float room = element->GetClientWidth(), width = element->GetScrollWidth();
    if (room <= 0 || width <= room + .5f) continue;
    element->SetProperty("font-size", fmt::format("{:.3f}rem", element->GetComputedValues().font_size() * room / width / rem));
    changed = true;
  }
  if (!changed) fit_passes_ = 0;
  return changed;
}

bool GameStyleControls::PlaceCursor() {
  if (!document_ || !cursor_) return false;
  auto* focus = document_->GetContext() ? document_->GetContext()->GetFocusElement() : nullptr;
  bool changed = false;
  if (focus != focused_) {
    focused_ = focus;
    ApplyTextEffects();
    changed = true;
  }
  const float rem = document_->GetComputedValues().font_size();
  auto* column = Within(focus, sidebar_) ? sidebar_ : Within(focus, detail_) ? detail_ : nullptr;
  auto* row = column ? RowOf(focus) : nullptr;
  std::array<float, 4> place{0, 0, 1, 0};
  if (row && rem > 0) {
    // Only while its row is inside the column's view.
    const float top = row->GetAbsoluteTop(), bottom = top + row->GetOffsetHeight();
    const bool inside = top >= column->GetAbsoluteTop() - 1 && bottom <= column->GetAbsoluteTop() + column->GetClientHeight() + 1;
    const bool narrow = row->Closest(".stick-columns, .deadzone-adjustments") != nullptr;
    const float scale = narrow ? kCloudInnerScale : 1;
    // Scaled about its centre.
    const float start = narrow ? kCloudInnerStart - kCloudWidth * (1 - scale) / 2 : column == sidebar_ ? kCloudSidebar : kCloudDetail;
    place = {(row->GetAbsoluteLeft() - stage_->GetAbsoluteLeft()) / rem + start,
             (top - stage_->GetAbsoluteTop()) / rem + (row->GetOffsetHeight() / rem - kCloudHeight) / 2, scale, inside ? 1.f : 0.f};
  }
  const auto same = [](float a, float b) { return std::abs(a - b) < .01f; };
  if (same(place[0], placed_[0]) && same(place[1], placed_[1]) && same(place[2], placed_[2]) && same(place[3], placed_[3])) return changed;
  placed_ = place;
  cursor_->SetProperty("display", place[3] > 0 ? "block" : "none");
  cursor_->SetProperty("left", fmt::format("{:.3f}rem", place[0]));
  cursor_->SetProperty("top", fmt::format("{:.3f}rem", place[1]));
  cursor_->SetProperty("transform", fmt::format("scale({:.3f})", place[2]));
  return true;
}
}  // namespace narutorise::menu
