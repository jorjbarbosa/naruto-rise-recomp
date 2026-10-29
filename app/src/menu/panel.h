// Shared tabbed window: options on the left, description/values on the right.
#pragma once
#include <RmlUi/Core/EventListener.h>
#include <functional>
#include <string>
#include <vector>
namespace Rml { class Context; class ElementDocument; class Element; }
namespace narutorise::menu {
struct PanelRow {
  std::string id, label, value, description;
  bool modified = false, restart = false;
  std::vector<std::string> choices;
  int selected = 0;
  std::function<void(int)> change;
  std::function<void()> activate;
  std::string section;
  bool centered = false, good = false;
  std::function<void()> clear;
  bool inline_adjust = false;
};
class Panel final : public Rml::EventListener {
 public:
  bool Show(Rml::Context& context, std::string title, std::vector<std::string> tabs, int tab,
            std::vector<PanelRow> rows, std::function<void(int)> select_tab, std::function<void()> close,
            std::string focus = {}, bool dim = false);
  void Hide();
  bool Shown() const { return document_ != nullptr; }
  const std::string& FocusId() const { return focus_id_; }
  void Tick();
  void Layout(int width, int height, float ratio);
  void SetGamepadHints(bool gamepad);
  bool RevealSelection();
  void ProcessEvent(Rml::Event& event) override;
 private:
  void Describe(size_t row, bool values);
  Rml::ElementDocument* document_ = nullptr;
  std::vector<PanelRow> rows_;
  std::function<void(int)> select_tab_;
  std::function<void()> close_, pending_;
  int tab_ = 0, tabs_ = 0;
  float row_width_ = 536.0f, detail_width_ = 421.333f;
  bool gamepad_ = false;
  Rml::Element* reveal_ = nullptr;
  std::string focus_id_;
};
}  // namespace narutorise::menu
