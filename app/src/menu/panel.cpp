// Window interactions are queued until after RmlUi finishes dispatching events.
#include "menu/panel.h"
#include "menu/text.h"
#include <RmlUi/Core.h>
#include <utility>
#include <algorithm>
#include <fmt/format.h>
namespace narutorise::menu {
namespace {
std::string Escape(const std::string& text) { return Rml::StringUtilities::EncodeRml(text); }
}
bool Panel::Show(Rml::Context& context, std::string title, std::vector<std::string> tabs, int tab,
                 std::vector<PanelRow> rows, std::function<void(int)> select_tab, std::function<void()> close,
                 std::string focus, bool dim) {
  Hide();
  document_ = context.LoadDocument("panel.rml");
  if (!document_) return false;
  rows_ = std::move(rows); select_tab_ = std::move(select_tab); close_ = std::move(close);
  tab_ = tab; tabs_ = int(tabs.size());
  document_->GetElementById("panel-title")->SetInnerRML(Escape(title));
  document_->SetClass("dim", true);
  document_->SetClass("setup", dim);
  document_->SetClass("untabbed", tabs.empty());
  auto* tab_bar = document_->GetElementById("tabs");
  for (size_t i = 0; i < tabs.size(); ++i) {
    auto item = document_->CreateElement("button");
    item->SetAttribute("tab", int(i));
    item->SetClass("tab", true);
    item->SetInnerRML("<div class='tab-plate'/><span class='tab-label'>" + Escape(tabs[i]) + "</span>");
    item->SetClass("selected", int(i) == tab);
    tab_bar->AppendChild(std::move(item));
  }
  auto* options = document_->GetElementById("options");
  std::string section;
  for (size_t i = 0; i < rows_.size(); ++i) {
    const auto& row = rows_[i];
    if (!row.section.empty() && row.section != section) {
      section = row.section;
      auto heading = document_->CreateElement("div"); heading->SetClass("group", true);
      heading->SetInnerRML("<span class='group-title'>" + Escape(section) + "</span><div class='group-rule'/>");
      options->AppendChild(std::move(heading));
    }
    auto item = document_->CreateElement("button");
    item->SetId(row.id); item->SetClass("row", true);
    item->SetAttribute("row", int(i));
    item->SetClass("modified", row.modified);
    item->SetClass("centered", row.centered);
    item->SetClass("good", row.good);
    item->SetAttribute("centered", row.centered);
    item->SetClass("adjustable", !row.choices.empty());
    item->SetInnerRML("<div class='row-name'><span class='row-label'>" + Escape(row.label) + "</span>" +
        (row.restart ? "<span class='restart'>" + Escape(Text("restart")) + "</span>" : "") + "</div>" +
        "<div class='value'><span class='adjust left' adjust='-1'>‹</span><span class='current'>" +
        (row.modified ? "• " : "") + Escape(row.value) + "</span><span class='adjust right' adjust='1'>›</span></div>");
    item->AddEventListener("focus", this);
    options->AppendChild(std::move(item));
  }
  document_->AddEventListener("click", this);
  document_->AddEventListener("keydown", this);
  document_->AddEventListener("mouseover", this);
  document_->AddEventListener("mousedown", this);
  const auto dimensions = context.GetDimensions();
  Layout(dimensions.x, dimensions.y, context.GetDensityIndependentPixelRatio());
  SetGamepadHints(gamepad_);
  document_->Show(Rml::ModalFlag::Modal, Rml::FocusFlag::Document);
  auto* focused = document_->GetElementById(focus.empty() && !rows_.empty() ? rows_.front().id : focus);
  if (!rows_.empty()) Describe(0, false);
  if (focused) focused->Focus();
  return true;
}
void Panel::Layout(int width, int height, float ratio) {
  if (!document_) return;
  const float w = std::max(360.0f, std::min(1040.0f, width / ratio - 48.0f));
  const float h = std::max(240.0f, std::min(586.667f, height / ratio - 32.0f));
  const float left = w * (860.0f / 1560.0f);
  row_width_ = left - 37.333f; detail_width_ = w - left - 45.333f;
  const auto dp = [](float n) { return fmt::format("{:.3f}dp", n); };
  auto* panel = document_->GetElementById("panel");
  panel->SetProperty("width", dp(w)); panel->SetProperty("height", dp(h));
  // Keep clipping axis aligned. A transformed ancestor makes RmlUi request
  // clip masks, while our renderer intentionally supports scissor rectangles.
  panel->SetProperty("left", dp((width / ratio - w) / 2));
  panel->SetProperty("top", dp((height / ratio - h) / 2));
  auto* options = document_->GetElementById("options");
  options->SetProperty("width", dp(row_width_)); options->SetProperty("height", dp(h - 120.0f));
  auto* detail = document_->GetElementById("detail");
  detail->SetProperty("left", dp(left + 24 - 1.333f)); detail->SetProperty("width", dp(detail_width_));
  detail->SetProperty("height", dp(h - 120.0f));
  auto* divider = document_->GetElementById("divider"); divider->SetProperty("left", dp(left - 1.333f));
  document_->GetElementById("tab-background")->SetProperty("width", dp(w));
  for (int i = 0; i < options->GetNumChildren(); ++i) {
    auto* row = options->GetChild(i); row->SetProperty("width", dp(row_width_));
    if (!row->HasAttribute("row")) continue;
    const float value = row_width_ * .40f;
    row->GetChild(0)->SetProperty("width", dp(row_width_ - value - 46.667f));
    row->GetChild(1)->SetProperty("width", dp(value));
    if (row->GetAttribute<bool>("centered", false)) row->GetChild(0)->SetProperty("width", dp(row_width_ - 6.667f));
  }
  auto* tabs = document_->GetElementById("tabs");
  const float scale = std::min(1.0f, (w - 90) / 600.0f);
  for (int i = 0; i < tabs->GetNumChildren(); ++i) {
    // The v4 widths are minimums: a longer translated label widens its tab.
    const float sizes[] = {80, 80, 126.667f, 126.667f, 120};
    tabs->GetChild(i)->SetProperty("min-width", dp(scale * (tabs_ == 5 ? sizes[i] : 72)));
  }
}
void Panel::SetGamepadHints(bool gamepad) {
  gamepad_ = gamepad;
  if (!document_) return;
  bool mapping = std::any_of(rows_.begin(), rows_.end(), [](const PanelRow& row) { return bool(row.clear); });
  std::string footer = Escape(Text(mapping ? (gamepad ? "mapping-pad-hints" : "mapping-key-hints") : tabs_ == 5 ?
      (gamepad ? "panel-pad-hints" : "panel-key-hints") : (gamepad ? "list-pad-hints" : "list-key-hints")));
  for (const auto& [symbol, direction] : {std::pair{"←", "left"}, std::pair{"→", "right"}}) {
    for (size_t at = footer.find(symbol); at != std::string::npos; at = footer.find(symbol, at + 1))
      footer.replace(at, std::string(symbol).size(), "<span class='key-arrow " + std::string(direction) + "'><span class='shaft'/><span class='tip-a'/><span class='tip-b'/></span>");
  }
  document_->GetElementById("footer")->SetInnerRML(footer);
}
void Panel::Hide() {
  reveal_ = nullptr;
  if (!document_) return;
  document_->RemoveEventListener("click", this);
  document_->RemoveEventListener("keydown", this);
  document_->RemoveEventListener("mouseover", this);
  document_->RemoveEventListener("mousedown", this);
  auto* options = document_->GetElementById("options");
  for (int i = 0; i < options->GetNumChildren(); ++i) options->GetChild(i)->RemoveEventListener("focus", this);
  document_->Close(); document_ = nullptr;
  rows_.clear(); select_tab_ = {}; close_ = {}; pending_ = {};
}
void Panel::Tick() { auto action = std::exchange(pending_, {}); if (action) action(); }
void Panel::Describe(size_t index, bool values) {
  reveal_ = nullptr;
  if (!document_ || index >= rows_.size()) return;
  const auto& row = rows_[index];
  auto* detail = document_->GetElementById("detail");
  detail->SetInnerRML("<h2>" + Escape(row.label) + "</h2><p>" + Escape(row.description) + "</p>");
  if (values) for (size_t i = 0; i < row.choices.size(); ++i) {
    auto item = document_->CreateElement("button");
    item->SetClass("choice", true);
    item->SetAttribute("choice-row", int(index)); item->SetAttribute("choice", int(i));
    item->SetClass("chosen", int(i) == row.selected);
    item->SetProperty("width", fmt::format("{:.3f}dp", detail_width_));
    item->SetInnerRML((int(i) == row.selected ? "• " : "") + Escape(row.choices[i]));
    auto* element = detail->AppendChild(std::move(item));
    if (int(i) == row.selected) { element->Focus(); reveal_ = element; }
  }
}
bool Panel::RevealSelection() {
  if (!reveal_) return false;
  auto* selected = std::exchange(reveal_, nullptr);
  selected->ScrollIntoView(Rml::ScrollIntoViewOptions{Rml::ScrollAlignment::Nearest});
  return true;
}
void Panel::ProcessEvent(Rml::Event& event) {
  Rml::Element* target = event.GetTargetElement();
  while (target && target != document_ && !target->HasAttribute("row") && !target->HasAttribute("tab") &&
         !target->HasAttribute("choice") && !target->HasAttribute("adjust") && target->GetId() != "panel-close") target = target->GetParentNode();
  if (!target) return;
  if (event.GetType() == "mouseover") { if (target->GetTagName() == "button") target->Focus(); return; }
  const int row = target->GetAttribute<int>("row", -1);
  if (row >= 0 && event.GetType() == "mousedown" && event.GetParameter<int>("button", 0) == 1) {
    pending_ = rows_[size_t(row)].clear; return;
  }
  if (event.GetType() == "focus") { if (row >= 0) { focus_id_ = rows_[size_t(row)].id; Describe(size_t(row), false); target->ScrollIntoView(Rml::ScrollIntoViewOptions{Rml::ScrollAlignment::Nearest}); } return; }
  const int key = event.GetParameter<int>("key_identifier", 0);
  if (event.GetType() == "keydown") {
    if (key == Rml::Input::KI_ESCAPE) { pending_ = close_; event.StopPropagation(); }
    else if (row >= 0 && key == Rml::Input::KI_DELETE && rows_[size_t(row)].clear) {
      pending_ = rows_[size_t(row)].clear; event.StopPropagation();
    }
    else if (!rows_.empty() && (key == Rml::Input::KI_HOME || key == Rml::Input::KI_END)) {
      const auto& id = key == Rml::Input::KI_HOME ? rows_.front().id : rows_.back().id;
      if (auto* element = document_->GetElementById(id)) element->Focus();
      event.StopPropagation();
    }
    else if (key == Rml::Input::KI_PRIOR || key == Rml::Input::KI_NEXT) {
      const int next = (tab_ + tabs_ + (key == Rml::Input::KI_NEXT ? 1 : -1)) % std::max(1, tabs_);
      pending_ = [fn = select_tab_, next] { if (fn) fn(next); }; event.StopPropagation();
    } else if (row >= 0 && (key == Rml::Input::KI_LEFT || key == Rml::Input::KI_RIGHT)) {
      const auto& option = rows_[size_t(row)];
      if (option.change && !option.choices.empty()) {
        const int next = (option.selected + int(option.choices.size()) + (key == Rml::Input::KI_RIGHT ? 1 : -1)) % int(option.choices.size());
        pending_ = [fn = option.change, next] { fn(next); }; event.StopPropagation();
      }
    }
    return;
  }
  if (event.GetType() != "click") return;
  if (target->HasAttribute("adjust")) {
    auto* item = target->GetParentNode();
    while (item && !item->HasAttribute("row")) item = item->GetParentNode();
    if (item) {
      const auto& option = rows_.at(size_t(item->GetAttribute<int>("row", 0)));
      const int count = int(option.choices.size());
      if (option.change && count) {
        const int next = (option.selected + count + target->GetAttribute<int>("adjust", 1)) % count;
        pending_ = [fn = option.change, next] { fn(next); };
      }
    }
    return;
  }
  if (target->GetId() == "panel-close") pending_ = close_;
  else if (target->HasAttribute("tab")) {
    const int tab = target->GetAttribute<int>("tab", 0);
    pending_ = [fn = select_tab_, tab] { if (fn) fn(tab); };
  } else if (target->HasAttribute("choice")) {
    const auto fn = rows_.at(size_t(target->GetAttribute<int>("choice-row", 0))).change;
    const int value = target->GetAttribute<int>("choice", 0);
    pending_ = [fn, value] { if (fn) fn(value); };
  } else if (row >= 0) {
    if (rows_[size_t(row)].activate) pending_ = rows_[size_t(row)].activate;
    else Describe(size_t(row), !rows_[size_t(row)].inline_adjust);
  }
}
}  // namespace narutorise::menu
