// Window interactions are queued until after RmlUi finishes dispatching events (game_style_panel.h).
#include "menu/game_style_panel.h"
#include "menu/localization.h"
#include "menu/text.h"
#include <RmlUi/Core.h>
#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>
#include <fmt/format.h>
namespace narutorise::menu {
namespace {
std::string Escape(const std::string& text) { return Rml::StringUtilities::EncodeRml(text); }
std::string Replace(std::string text, std::string_view from, std::string_view to) {
  for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
    text.replace(at, from.size(), to);
  return text;
}
// The movie's stage, and where its panels, curtains and paper are on it.
constexpr float kStageWidth = 1280, kStageHeight = 720;
constexpr float kPaperLeft = 160, kPaperRight = 1120, kCurtainWidth = 133.333f;
// A prompt: the game's button image, or the key's name with the keyboard.
std::string Prompt(const char* id, const char* image, const char* key) {
  return fmt::format("<div id='{}' class='prompt {}'><span>{}</span></div>", id, image, key);
}
}  // namespace

GameStylePanel::Kind GameStylePanel::KindOf(const PanelRow& row) {
  if (row.choices.size() == 2 && row.choices[0] == Text("off") && row.choices[1] == Text("on")) return Kind::kToggle;
  if (row.inline_adjust && !row.choices.empty()) return Kind::kSlider;
  if (!row.choices.empty()) return Kind::kChoice;
  return row.activate ? Kind::kAction : Kind::kInfo;
}
std::string GameStylePanel::EdoText(std::string text) {
  text = Replace(std::move(text), "×", "X");
  text = Replace(std::move(text), "…", "...");
  return Replace(std::move(text), "•", "");
}

bool GameStylePanel::Show(Rml::Context& context, std::vector<std::string> tabs, int tab, std::vector<PanelRow> rows,
                          std::function<void(int)> select_tab, std::function<void()> close, std::string focus) {
  Hide();
  document_ = context.LoadDocument("game_menu.rml");
  if (!document_) return false;
  rows_ = std::move(rows); select_tab_ = std::move(select_tab); close_ = std::move(close);
  tab_ = tab; tabs_ = int(tabs.size());
  document_->SetClass("cyrillic", Localization::GetLanguage() == Localization::Language::Russian);
  std::string bar = Prompt("tab-previous", "lb", "PgUp");
  for (size_t i = 0; i < tabs.size(); ++i)
    bar += fmt::format("<button class='tab{}' tab='{}'>{}</button>", int(i) == tab ? " selected" : "", i,
                       Escape(EdoText(tabs[i])));
  bar += Prompt("tab-next", "rb", "PgDn") + "<div id='splash'/>";
  document_->GetElementById("tabs")->SetInnerRML(bar);

  // Rows on the movie's translucent panels: a section starts a new panel.
  auto* options = document_->GetElementById("options");
  Rml::Element* group = nullptr;
  std::string section;
  bool restart = false;
  for (size_t i = 0; i < rows_.size(); ++i) {
    const auto& row = rows_[i];
    if (!group || (!row.section.empty() && row.section != section)) {
      section = row.section;
      auto panel = document_->CreateElement("div"); panel->SetClass("group", true);
      group = options->AppendChild(std::move(panel));
    }
    restart |= row.restart;
    const std::string value = Escape(EdoText(row.value));
    std::string control;
    switch (KindOf(row)) {
      case Kind::kToggle:
        // ON before OFF, as the movie's YES before NO, in the short words
        // that fit its two options.
        control = fmt::format("<div class='option on{}' choose='1'><div class='orb'/><span class='word'>{}</span></div>"
                              "<div class='option off{}' choose='0'><div class='orb'/><span class='word'>{}</span></div>",
                              row.selected == 1 ? " chosen" : "", Escape(EdoText(Text("toggle-on"))),
                              row.selected == 0 ? " chosen" : "", Escape(EdoText(Text("toggle-off"))));
        break;
      case Kind::kSlider: {
        const float level = row.choices.size() > 1 ? float(row.selected) / float(row.choices.size() - 1) : 0;
        control = fmt::format("<div class='track'><div class='fill' style='width: {:.3f}rem;'><div class='blue'/></div></div>"
                              "<span class='number'>{}</span>", 280 * std::clamp(level, 0.0f, 1.0f), value);
        break;
      }
      case Kind::kChoice:
        control = "<div class='arrow left' adjust='-1'/><span class='value'>" + value +
                  "</span><div class='arrow right' adjust='1'/>";
        break;
      case Kind::kAction: {
        // A file or folder shows its name.
        std::string shown = row.value == "›" ? std::string() : row.value;
        if (const size_t slash = shown.find_last_of("\\/"); slash != std::string::npos && slash + 1 < shown.size())
          shown.erase(0, slash + 1);
        control = (shown.empty() ? std::string() : "<span class='value action'>" + Escape(EdoText(shown)) + "</span>") +
                  "<div class='chevron'/>";
        break;
      }
      case Kind::kInfo:
        control = "<span class='value info plain'>" + Escape(row.value) + "</span>";
        break;
    }
    auto item = document_->CreateElement("button");
    item->SetId(row.id); item->SetClass("row", true);
    item->SetAttribute("row", int(i));
    item->SetClass("modified", row.modified);
    item->SetInnerRML("<div class='cursor'/><div class='name'><span class='label'>" + Escape(EdoText(row.label)) +
                      "</span>" + (row.restart ? "<div class='star'/>" : "") + "</div><div class='control'>" +
                      control + "</div>");
    item->AddEventListener("focus", this);
    group->AppendChild(std::move(item));
  }
  document_->SetClass("restart", restart);
  document_->GetElementById("back-label")->SetInnerRML(Escape(EdoText(Text("back"))));
  document_->GetElementById("legend-label")->SetInnerRML(Escape(EdoText(Text("restart-legend"))));
  document_->AddEventListener("click", this);
  document_->AddEventListener("keydown", this);
  document_->AddEventListener("mouseover", this);
  document_->AddEventListener("mousedown", this);
  const auto dimensions = context.GetDimensions();
  Layout(dimensions.x, dimensions.y, context.GetDensityIndependentPixelRatio());
  SetGamepadHints(gamepad_);
  document_->Show(Rml::ModalFlag::Modal, Rml::FocusFlag::Document);
  auto* focused = document_->GetElementById(focus.empty() && !rows_.empty() ? rows_.front().id : focus);
  if (!focused && !rows_.empty()) focused = document_->GetElementById(rows_.front().id);
  if (!rows_.empty()) Describe(0);
  if (focused) focused->Focus();
  fit_passes_ = kFitPasses;
  return true;
}

bool GameStylePanel::FitText() {
  if (!document_ || fit_passes_ == 0) return false;
  --fit_passes_;
  // Font sizes stay in rem, so they follow later layouts of the stage. Text
  // widths round per glyph, so a size can take more than one pass.
  const float rem = document_->GetComputedValues().font_size();
  if (rem <= 0) return false;
  bool changed = false;
  const auto scale = [&](Rml::Element* text, float factor) {
    text->SetProperty("font-size", fmt::format("{:.3f}rem", text->GetComputedValues().font_size() * factor / rem));
    changed = true;
  };
  const auto shrink = [&](Rml::Element* text, float width, float room) {
    if (width > room + 0.5f && width > 0) scale(text, std::max(room, 0.0f) / width);
  };
  for (const auto& row : rows_) {
    auto* item = document_->GetElementById(row.id);
    if (!item) continue;
    auto* label = item->QuerySelector(".label");
    auto* control = item->QuerySelector(".control");
    if (label && control) {
      // The label and its star end a gap before the control.
      auto* star = item->QuerySelector(".name .star");
      const float end = label->GetAbsoluteLeft() + label->GetOffsetWidth();
      const float trail = star ? star->GetAbsoluteLeft() + star->GetOffsetWidth() - end : 0;
      shrink(label, label->GetOffsetWidth(), control->GetAbsoluteLeft() - label->GetAbsoluteLeft() - trail - 12 * rem);
    }
    if (auto* value = item->QuerySelector(".value")) shrink(value, value->GetScrollWidth(), value->GetClientWidth());
  }
  // The ON and OFF words share one size: the largest that fits every option.
  Rml::ElementList words;
  document_->GetElementsByClassName(words, "word");
  float factor = 1;
  for (auto* word : words) {
    auto* option = word->GetParentNode();
    const float room = option->GetAbsoluteLeft() + option->GetClientWidth() - word->GetAbsoluteLeft() - 8 * rem;
    if (word->GetOffsetWidth() > room + 0.5f) factor = std::min(factor, std::max(room, 0.0f) / word->GetOffsetWidth());
  }
  if (factor < 1) for (auto* word : words) scale(word, factor);
  if (!changed) fit_passes_ = 0;
  return changed;
}

void GameStylePanel::Layout(int width, int height, float ratio) {
  if (!document_) return;
  const float w = width / ratio, h = height / ratio;
  const float k = std::min(w / kStageWidth, h / kStageHeight);
  const float left = (w - kStageWidth * k) / 2, top = (h - kStageHeight * k) / 2;
  const auto dp = [](float n) { return fmt::format("{:.3f}dp", n); };
  document_->SetProperty("font-size", dp(k));  // One rem: a stage unit.
  auto* stage = document_->GetElementById("stage");
  stage->SetProperty("left", dp(left)); stage->SetProperty("top", dp(top));
  const auto place = [&](const char* id, float x, float width_dp) {
    auto* element = document_->GetElementById(id);
    element->SetProperty("left", dp(x)); element->SetProperty("width", dp(std::max(0.0f, width_dp)));
  };
  // The paper and the curtains span the window's height; beside a narrower
  // stage the curtains repeat to the window's edges.
  place("paper", left + kPaperLeft * k, (kPaperRight - kPaperLeft) * k);
  place("edge-left", left + kCurtainWidth * k, (kPaperLeft - kCurtainWidth) * k);
  place("edge-right", left + kPaperRight * k, (kPaperLeft - kCurtainWidth) * k);
  const float tile = kCurtainWidth * k;
  for (const auto& [id, edge, toward] : {std::tuple{"curtain-left", left + kCurtainWidth * k, -1.0f},
                                         std::tuple{"curtain-right", left + (kStageWidth - kCurtainWidth) * k, 1.0f}}) {
    auto* curtains = document_->GetElementById(id);
    while (curtains->GetNumChildren()) curtains->RemoveChild(curtains->GetFirstChild());
    const float span = toward < 0 ? edge : w - edge;
    for (int n = 0; n * tile < span + tile && n < 16; ++n) {
      auto piece = document_->CreateElement("div"); piece->SetClass("curtain", true);
      piece->SetProperty("left", dp(toward < 0 ? edge - (n + 1) * tile : edge + n * tile));
      piece->SetProperty("width", dp(tile));
      curtains->AppendChild(std::move(piece));
    }
  }
}

void GameStylePanel::SetGamepadHints(bool gamepad) {
  gamepad_ = gamepad;
  if (document_) document_->SetClass("keys", !gamepad);
}

void GameStylePanel::Hide() {
  if (!document_) return;
  document_->RemoveEventListener("click", this);
  document_->RemoveEventListener("keydown", this);
  document_->RemoveEventListener("mouseover", this);
  document_->RemoveEventListener("mousedown", this);
  for (const auto& row : rows_) if (auto* item = document_->GetElementById(row.id)) item->RemoveEventListener("focus", this);
  document_->Close(); document_ = nullptr;
  rows_.clear(); select_tab_ = {}; close_ = {}; pending_ = {};
}

void GameStylePanel::Tick() { auto action = std::exchange(pending_, {}); if (action) action(); }

void GameStylePanel::Describe(size_t index) {
  if (!document_ || index >= rows_.size()) return;
  document_->GetElementById("help")->SetInnerRML(Escape(rows_[index].description));
}

void GameStylePanel::Choose(size_t index, int value) {
  const auto& row = rows_.at(index);
  if (!row.change || value == row.selected || value < 0 || value >= int(row.choices.size())) return;
  pending_ = [fn = row.change, value] { fn(value); };
}

void GameStylePanel::Change(size_t index, int step) {
  const auto& row = rows_.at(index);
  const int count = int(row.choices.size());
  if (!row.change || !count) return;
  switch (KindOf(row)) {
    case Kind::kToggle: Choose(index, step < 0 ? 1 : 0); break;  // Left: ON, right: OFF, as drawn.
    case Kind::kSlider: Choose(index, std::clamp(row.selected + step, 0, count - 1)); break;
    default: Choose(index, (row.selected + count + step) % count); break;
  }
}

void GameStylePanel::ProcessEvent(Rml::Event& event) {
  Rml::Element* target = event.GetTargetElement();
  while (target && target != document_ && !target->HasAttribute("row") && !target->HasAttribute("tab") &&
         !target->HasAttribute("adjust") && !target->HasAttribute("choose") && target->GetId() != "back-prompt" &&
         target->GetId() != "back-label" && target->GetId() != "tab-previous" && target->GetId() != "tab-next")
    target = target->GetParentNode();
  if (!target) return;
  if (event.GetType() == "mouseover") { if (target->GetTagName() == "button" && target->HasAttribute("row")) target->Focus(); return; }
  auto* item = target;
  while (item && item != document_ && !item->HasAttribute("row")) item = item->GetParentNode();
  const int row = item && item != document_ ? item->GetAttribute<int>("row", -1) : -1;
  if (row >= 0 && event.GetType() == "mousedown" && event.GetParameter<int>("button", 0) == 1) {
    pending_ = rows_[size_t(row)].clear; return;
  }
  if (event.GetType() == "focus") {
    if (row >= 0) {
      focus_id_ = rows_[size_t(row)].id; Describe(size_t(row));
      item->ScrollIntoView(Rml::ScrollIntoViewOptions{Rml::ScrollAlignment::Nearest});
    }
    return;
  }
  const int key = event.GetParameter<int>("key_identifier", 0);
  const auto select = [&](int next) { pending_ = [fn = select_tab_, next] { if (fn) fn(next); }; };
  if (event.GetType() == "keydown") {
    if (key == Rml::Input::KI_ESCAPE) { pending_ = close_; event.StopPropagation(); }
    else if (row >= 0 && key == Rml::Input::KI_DELETE && rows_[size_t(row)].clear) {
      pending_ = rows_[size_t(row)].clear; event.StopPropagation();
    } else if (!rows_.empty() && (key == Rml::Input::KI_HOME || key == Rml::Input::KI_END)) {
      const auto& id = key == Rml::Input::KI_HOME ? rows_.front().id : rows_.back().id;
      if (auto* element = document_->GetElementById(id)) element->Focus();
      event.StopPropagation();
    } else if (key == Rml::Input::KI_PRIOR || key == Rml::Input::KI_NEXT) {
      select((tab_ + tabs_ + (key == Rml::Input::KI_NEXT ? 1 : -1)) % std::max(1, tabs_)); event.StopPropagation();
    } else if (row >= 0 && (key == Rml::Input::KI_LEFT || key == Rml::Input::KI_RIGHT)) {
      Change(size_t(row), key == Rml::Input::KI_RIGHT ? 1 : -1); event.StopPropagation();
    }
    return;
  }
  if (event.GetType() != "click") return;
  if (target->GetId() == "back-prompt" || target->GetId() == "back-label") { pending_ = close_; return; }
  if (target->GetId() == "tab-previous" || target->GetId() == "tab-next") {
    select((tab_ + tabs_ + (target->GetId() == "tab-next" ? 1 : -1)) % std::max(1, tabs_)); return;
  }
  if (target->HasAttribute("tab")) { select(target->GetAttribute<int>("tab", 0)); return; }
  if (row < 0) return;
  if (target->HasAttribute("adjust")) { Change(size_t(row), target->GetAttribute<int>("adjust", 1)); return; }
  if (target->HasAttribute("choose")) { Choose(size_t(row), target->GetAttribute<int>("choose", 0)); return; }
  const auto& option = rows_[size_t(row)];
  switch (KindOf(option)) {
    case Kind::kAction: pending_ = option.activate; break;
    case Kind::kToggle: Choose(size_t(row), option.selected == 1 ? 0 : 1); break;
    case Kind::kChoice: Change(size_t(row), 1); break;
    default: break;
  }
}
}  // namespace narutorise::menu
