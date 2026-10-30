// The in-game settings in the style of the game's Options screen
// (game_menu.rml): the game's paper, curtains, title band, cursor and buttons
// ("game/" images of the RenderInterface), Edo SZ text, and the rows of Panel
// drawn the way the movie draws its own. The stage is the movie's 1280 x 720:
// the document's lengths are rem, one rem a stage unit, scaled to fit the
// window; the paper and the curtains reach the window's edges.
#pragma once
#include "menu/game_style_motion.h"
#include "menu/panel.h"
#include <RmlUi/Core/EventListener.h>
#include <functional>
#include <optional>
#include <string>
#include <vector>
namespace Rml { class Context; class ElementDocument; class Element; }
namespace narutorise::menu {
class GameStylePanel final : public Rml::EventListener {
 public:
  // How a row shows its value.
  enum class Kind { kChoice, kToggle, kSlider, kAction, kInfo };
  static Kind KindOf(const PanelRow& row);
  // The Edo SZ spelling of a text: the glyphs it lacks become the nearest it has.
  static std::string EdoText(std::string text);
  // Opens with the movie's in-animation; another tab of the open page swaps
  // its panels; the same tab redraws at once. Back (Esc, B) plays the
  // out-animation before close runs; Hide closes at once.
  bool Show(Rml::Context& context, std::vector<std::string> tabs, int tab, std::vector<PanelRow> rows,
            std::function<void(int)> select_tab, std::function<void()> close, std::string focus = {});
  void Hide();
  bool Shown() const { return document_ != nullptr; }
  const std::string& FocusId() const { return focus_id_; }
  // Before the context's update: the animation's frame and the queued actions.
  void Tick();
  void SetClock(std::function<GameStyleMotion::Clock::time_point()> now);
  // After the context's update: labels and values wider than their room
  // (translations, file names) shrink to it. True when the layout changed,
  // for another update and pass, at most kFitPasses after Show.
  bool FitText();
  void Layout(int width, int height, float ratio);
  void SetGamepadHints(bool gamepad);
  void ProcessEvent(Rml::Event& event) override;
 private:
  using Phase = GameStyleMotion::Phase;
  struct Content {
    std::vector<std::string> tabs;
    int tab = 0;
    std::vector<PanelRow> rows;
    std::function<void(int)> select_tab;
    std::function<void()> close;
    std::string focus;
  };
  void Build(Content content);
  int FocusedRow() const;
  void Close();
  void Describe(size_t row);
  void Change(size_t row, int step);
  void Choose(size_t row, int value);
  Rml::ElementDocument* document_ = nullptr;
  std::vector<PanelRow> rows_;
  std::function<void(int)> select_tab_;
  std::function<void()> close_, pending_;
  int tab_ = 0, tabs_ = 0;
  static constexpr int kFitPasses = 4;
  int fit_passes_ = 0;
  bool gamepad_ = false, skipping_ = false;
  GameStyleMotion motion_;
  std::optional<Content> next_;
  std::string focus_id_;
};
}  // namespace narutorise::menu
