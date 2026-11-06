// Window interactions are queued until after RmlUi finishes dispatching events (game_style_panel.h).
#include "menu/game_style_panel.h"
#include "menu/game_stage.h"
#include "menu/localization.h"
#include "menu/prompts.h"
#include "menu/text.h"
#include <RmlUi/Core.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <fmt/format.h>
namespace narutorise::menu {
namespace {
using game_stage::Copies;
using game_stage::Prompt;
using game_stage::kOutline;
using game_stage::kOverOutline;
using game_stage::kPromptOutline;
using game_stage::kValueShadow;
using game_stage::kInk;
using game_stage::kWhite;
std::string Escape(const std::string& text) { return Rml::StringUtilities::EncodeRml(text); }
std::string Replace(std::string text, std::string_view from, std::string_view to) {
  for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
    text.replace(at, from.size(), to);
  return text;
}
constexpr float kNameLeft = 33.333f;  // The labels' place in their rows (game_menu.rcss .name).
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
                          std::function<void(int)> select_tab, std::function<void()> close, std::string focus,
                          bool at_once) {
  Content content{std::move(tabs), tab, std::move(rows), std::move(select_tab), std::move(close), std::move(focus)};
  if (document_ && motion_.phase() != Phase::kClose && content.tab != tab_ && int(content.tabs.size()) == tabs_) {
    // Another tab: its panels enter once the current ones leave (Tick).
    next_ = std::move(content);
    if (motion_.phase() != Phase::kTabOut) motion_.Start(Phase::kTabOut);
    return true;
  }
  const bool opening = !document_;
  if (opening) {
    document_ = context.LoadDocument("game_menu.rml");
    if (!document_) return false;
    document_->AddEventListener("click", this);
    document_->AddEventListener("keydown", this);
    document_->AddEventListener("mouseover", this);
    document_->AddEventListener("mousedown", this);
    const auto dimensions = context.GetDimensions();
    Layout(dimensions.x, dimensions.y, context.GetDensityIndependentPixelRatio());
    document_->Show(Rml::ModalFlag::Modal, Rml::FocusFlag::Document);
    motion_.Attach(document_);
  }
  // The same tab redrawn (a changed value) shows at once.
  motion_.Settle();
  next_.reset();
  Build(std::move(content));
  if (opening && !at_once) {
    motion_.Start(Phase::kOpen);
    Play(Sound::kOpen);
  }
  return true;
}

void GameStylePanel::Build(Content content) {
  building_ = true;  // The page's first focus makes no sound.
  for (const auto& row : rows_) if (auto* item = document_->GetElementById(row.id)) item->RemoveEventListener("focus", this);
  rows_ = std::move(content.rows); select_tab_ = std::move(content.select_tab); close_ = std::move(content.close);
  tab_ = content.tab; tabs_ = int(content.tabs.size());
  document_->SetClass("cyrillic", Localization::GetLanguage() == Localization::Language::Russian);
  std::string bar = Prompt("tab-previous", "lb");
  for (size_t i = 0; i < content.tabs.size(); ++i)
    bar += fmt::format("<button class='tab{}' tab='{}'>{}</button>", int(i) == tab_ ? " selected" : "", i,
                       Escape(EdoText(content.tabs[i])));
  bar += Prompt("tab-next", "rb") + "<div id='splash'><div class='ink'/></div>";
  document_->GetElementById("tabs")->SetInnerRML(bar);
  SetGlyphs(glyphs_);

  // Rows on the movie's translucent panels: a section starts a new panel.
  auto* options = document_->GetElementById("options");
  options->SetInnerRML("");
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
        control = fmt::format("<div class='track'><div class='fill' style='width: {:.3f}rem;'><div class='blue'/></div></div>",
                              340 * std::clamp(level, 0.0f, 1.0f));
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
    item->SetInnerRML("<div class='name'><span class='label'>" + Escape(EdoText(row.label)) +
                      "</span>" + (row.restart ? "<div class='star'/>" : "") + "</div><div class='control'>" +
                      control + "</div>");
    item->AddEventListener("focus", this);
    group->AppendChild(std::move(item));
  }
  document_->SetClass("restart", restart);
  document_->GetElementById("back-label")->SetInnerRML(Escape(EdoText(Text("back"))));
  document_->GetElementById("legend-label")->SetInnerRML(Escape(EdoText(Text("restart-legend"))));
  auto* focused = document_->GetElementById(content.focus.empty() && !rows_.empty() ? rows_.front().id : content.focus);
  if (!focused && !rows_.empty()) focused = document_->GetElementById(rows_.front().id);
  if (!rows_.empty()) Describe(0);
  if (focused) focused->Focus();
  fit_passes_ = kFitPasses;
  std::vector<Rml::Element*> items;
  for (const auto& row : rows_) items.push_back(document_->GetElementById(row.id));
  motion_.BindRows(items, FocusedRow());
  ApplyTextEffects();
  building_ = false;
}

void GameStylePanel::ApplyTextEffects() {
  if (!document_) return;
  // On each text element: an inherited font-effect does not reach the text here.
  const std::string outline = Copies(kOutline, text_px_, kWhite), over = Copies(kOutline, text_px_, kOverOutline);
  const auto apply = [&](const char* selector, const std::string& effect) {
    Rml::ElementList elements;
    document_->QuerySelectorAll(elements, selector);
    for (auto* element : elements) element->SetProperty("font-effect", effect);
  };
  apply("#options .label", outline);
  apply("#options .value, #options .word", Copies(kValueShadow, text_px_, kInk));
  apply("#tabs .tab", outline);
  apply("#back-label, #ok-label, #legend-label", Copies(kPromptOutline, text_px_, kInk));
  if (auto* item = FocusedRow() >= 0 ? document_->GetElementById(focus_id_) : nullptr)
    if (auto* label = item->QuerySelector(".label")) label->SetProperty("font-effect", over);
  if (auto* selected = document_->QuerySelector("#tabs .tab.selected")) selected->SetProperty("font-effect", over);
}

int GameStylePanel::FocusedRow() const {
  for (size_t i = 0; i < rows_.size(); ++i) if (rows_[i].id == focus_id_) return int(i);
  return -1;
}

void GameStylePanel::Close() {
  if (!document_ || motion_.phase() == Phase::kClose) return;
  motion_.Start(Phase::kClose);
  Play(Sound::kClose);
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
      // The label and its star end a gap before the control. Measured within
      // the row, as the opening slides the labels by their margins.
      auto* star = item->QuerySelector(".name .star");
      const float end = label->GetOffsetLeft() + label->GetOffsetWidth();
      const float trail = star ? star->GetOffsetLeft() + star->GetOffsetWidth() - end : 0;
      shrink(label, label->GetOffsetWidth(),
             control->GetOffsetLeft() - kNameLeft * rem - label->GetOffsetLeft() - trail - 12 * rem);
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
  const float k = game_stage::Place(*document_, width, height, ratio);
  motion_.SetStage(0, k);
  text_px_ = k * ratio;  // Pixels per stage unit, for the text effects.
  ApplyTextEffects();
}

void GameStylePanel::SetGlyphs(controls::Glyphs glyphs) {
  glyphs_ = glyphs;
  if (document_) game_stage::SetPrompts(*document_, glyphs);
}

// The list scrolls by rows of different panels, so its ends usually fall
// inside one. Copies of the paper cover what does not fit whole: a row is
// shown entire or not at all, and its panel ends flat where more follows.
bool GameStylePanel::TrimList() {
  auto* options = document_ ? document_->GetElementById("options") : nullptr;
  auto* upper = document_ ? document_->GetElementById("trim-top") : nullptr;
  auto* lower = document_ ? document_->GetElementById("trim-bottom") : nullptr;
  const float rem = document_ ? document_->GetComputedValues().font_size() : 0;
  if (!options || !upper || !lower || rem <= 0) return false;
  const float view_top = options->GetAbsoluteTop(), view_bottom = view_top + options->GetClientHeight();
  float first = std::numeric_limits<float>::max(), last = std::numeric_limits<float>::lowest();
  if (motion_.phase() == Phase::kIdle)  // The panels pass under the curtains unclipped while they move.
    for (const auto& row : rows_) {
      auto* item = document_->GetElementById(row.id);
      auto* group = item ? item->GetParentNode() : nullptr;
      if (!group) continue;
      const float top = item->GetAbsoluteTop(), bottom = top + item->GetOffsetHeight();
      if (top < view_top - .5f || bottom > view_bottom + .5f) continue;
      first = std::min(first, group->GetFirstChild() == item ? group->GetAbsoluteTop() : top);
      last = std::max(last, group->GetLastChild() == item ? group->GetAbsoluteTop() + group->GetOffsetHeight() : bottom);
    }
  const bool whole = first <= last;
  const float stage = document_->GetElementById("stage-back")->GetAbsoluteTop();
  const std::array<float, 4> place{(view_top - stage) / rem, whole ? std::max(0.0f, first - view_top) / rem : 0,
                                   (std::min(view_bottom, whole ? last : view_bottom) - stage) / rem,
                                   whole ? std::max(0.0f, view_bottom - last) / rem : 0};
  bool same = true;
  for (size_t i = 0; i < place.size(); ++i) same &= std::abs(place[i] - trims_[i]) < .01f;
  if (same) return false;
  trims_ = place;
  const auto set = [&](Rml::Element* trim, float top, float height) {
    trim->SetProperty("display", height > .25f ? "block" : "none");
    trim->SetProperty("top", fmt::format("{:.3f}rem", top));
    trim->SetProperty("height", fmt::format("{:.3f}rem", height));
    trim->GetFirstChild()->SetProperty("top", fmt::format("{:.3f}rem", -top));
  };
  set(upper, place[0], place[1]);
  set(lower, place[2], place[3]);
  return true;
}

void GameStylePanel::Hide() {
  if (!document_) return;
  document_->RemoveEventListener("click", this);
  document_->RemoveEventListener("keydown", this);
  document_->RemoveEventListener("mouseover", this);
  document_->RemoveEventListener("mousedown", this);
  for (const auto& row : rows_) if (auto* item = document_->GetElementById(row.id)) item->RemoveEventListener("focus", this);
  motion_.Detach();
  document_->Close(); document_ = nullptr;
  rows_.clear(); select_tab_ = {}; close_ = {}; pending_ = {}; next_.reset(); skipping_ = false;
  held_keys_.clear();
}

void GameStylePanel::Tick() {
  // Keys waiting for the next page (ProcessEvent) hurry its animations along.
  const bool hurry = !held_keys_.empty();
  if (hurry) motion_.Skip();
  const bool skipped = std::exchange(skipping_, false) || hurry;
  bool changed = false;
  switch (motion_.Advance()) {
    case Phase::kClose: pending_ = close_; break;
    case Phase::kTabOut:
      if (next_) {
        Build(std::move(*next_)); next_.reset(); changed = true;
        if (!skipped) motion_.Start(Phase::kTabIn);
      }
      break;
    default: break;
  }
  auto action = std::exchange(pending_, {});
  if (action) { action(); changed = true; }
  // One held key a frame, in order, each on the page it was meant for, once
  // that page is laid out: navigation goes by the rows' positions.
  if (!changed && document_ && !held_keys_.empty() && !pending_ && motion_.phase() != Phase::kTabOut) {
    const int key = held_keys_.front();
    held_keys_.pop_front();
    replaying_ = true;
    document_->GetContext()->ProcessKeyDown(Rml::Input::KeyIdentifier(key), 0);
    replaying_ = false;
  }
}

void GameStylePanel::SetClock(std::function<GameStyleMotion::Clock::time_point()> now) { motion_.SetClock(std::move(now)); }

void GameStylePanel::Describe(size_t index) {
  if (!document_ || index >= rows_.size()) return;
  document_->GetElementById("help")->SetInnerRML(Escape(rows_[index].description));
}

void GameStylePanel::Choose(size_t index, int value) {
  const auto& row = rows_.at(index);
  if (!row.change || value == row.selected || value < 0 || value >= int(row.choices.size())) return;
  pending_ = [fn = row.change, value] { fn(value); };
  Play(Sound::kAdjust);
}

void GameStylePanel::Change(size_t index, int step) {
  const auto& row = rows_.at(index);
  const int count = int(row.choices.size());
  if (!row.change || !count) return;
  switch (KindOf(row)) {
    case Kind::kSlider: Choose(index, std::clamp(row.selected + step, 0, count - 1)); break;
    default: Choose(index, (row.selected + count + step) % count); break;
  }
}

void GameStylePanel::ProcessEvent(Rml::Event& event) {
  if (event.GetType() == "keydown" && !replaying_ &&
      (motion_.phase() == Phase::kTabOut || pending_ || !held_keys_.empty())) {
    // The rows on screen are leaving, or an action is due on the next frame:
    // the key waits for the page it is meant for (Tick).
    held_keys_.push_back(event.GetParameter<int>("key_identifier", 0));
    event.StopPropagation();
    return;
  }
  if (motion_.phase() != Phase::kIdle && (event.GetType() == "keydown" || event.GetType() == "mousedown")) {
    // Any input ends the animation. Closing, or with the panels being
    // replaced, a click only does that: its target is going away.
    const Phase phase = motion_.phase();
    motion_.Skip(); skipping_ = true;
    if (phase == Phase::kClose || phase == Phase::kTabOut) { event.StopPropagation(); return; }
  }
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
    pending_ = rows_[size_t(row)].clear;
    if (pending_) Play(Sound::kAdjust);
    return;
  }
  if (event.GetType() == "focus") {
    if (row >= 0) {
      // The focused label takes the movie's red outline; the cloud springs to it.
      if (auto* previous = document_->GetElementById(focus_id_))
        if (auto* label = previous->QuerySelector(".label")) label->SetProperty("font-effect", Copies(kOutline, text_px_, kWhite));
      if (auto* label = item->QuerySelector(".label")) label->SetProperty("font-effect", Copies(kOutline, text_px_, kOverOutline));
      motion_.Focus(row);
      if (!building_ && rows_[size_t(row)].id != focus_id_) Play(Sound::kMove);
      focus_id_ = rows_[size_t(row)].id; Describe(size_t(row));
      item->ScrollIntoView(Rml::ScrollIntoViewOptions{Rml::ScrollAlignment::Nearest});
    }
    return;
  }
  const int key = event.GetParameter<int>("key_identifier", 0);
  const auto select = [&](int next) {
    pending_ = [fn = select_tab_, next] { if (fn) fn(next); };
    if (next != tab_) Play(Sound::kMove);
  };
  if (event.GetType() == "keydown") {
    if (key == Rml::Input::KI_ESCAPE) { Close(); event.StopPropagation(); }
    else if (row >= 0 && key == Rml::Input::KI_DELETE && rows_[size_t(row)].clear) {
      pending_ = rows_[size_t(row)].clear; Play(Sound::kAdjust); event.StopPropagation();
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
  if (target->GetId() == "back-prompt" || target->GetId() == "back-label") { Close(); return; }
  if (target->GetId() == "tab-previous" || target->GetId() == "tab-next") {
    select((tab_ + tabs_ + (target->GetId() == "tab-next" ? 1 : -1)) % std::max(1, tabs_)); return;
  }
  if (target->HasAttribute("tab")) { select(target->GetAttribute<int>("tab", 0)); return; }
  if (row < 0) return;
  if (target->HasAttribute("adjust")) { Change(size_t(row), target->GetAttribute<int>("adjust", 1)); return; }
  if (target->HasAttribute("choose")) { Choose(size_t(row), target->GetAttribute<int>("choose", 0)); return; }
  const auto& option = rows_[size_t(row)];
  switch (KindOf(option)) {
    case Kind::kAction:
      pending_ = option.activate;
      if (pending_) Play(Sound::kConfirm);
      break;
    case Kind::kToggle: Choose(size_t(row), option.selected == 1 ? 0 : 1); break;
    case Kind::kChoice: Change(size_t(row), 1); break;
    default: break;
  }
}
}  // namespace narutorise::menu
