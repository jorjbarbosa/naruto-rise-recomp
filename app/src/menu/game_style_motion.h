// The motion of the game's Options screen on game_menu.rml (GameStylePanel),
// measured frame by frame on the game's own screen at 60 fps: the opening
// (the paper at once, the curtains, the rows sliding in one after another,
// the title band, the panels and the prompts), the closing, a tab switch
// (the rows and the panels leaving and entering), the focused label's pulse
// and the spring of the cursor's cloud when the focus moves. Times are the
// movie's 60 fps frames from a steady clock, taken two at a time every 1/30 s
// as the game's menus advance at its logic's step; lengths are units of the
// 1280 x 720 stage (rem). Only
// margins, sizes and opacity move, plus transforms of leaf text and of the
// cloud: the list keeps clipping with the scissor, as the render interface
// has no clip masks (render_interface.h).
#pragma once
#include <array>
#include <chrono>
#include <functional>
#include <vector>
namespace Rml { class Element; class ElementDocument; }
namespace narutorise::menu {
class GameStyleMotion {
 public:
  using Clock = std::chrono::steady_clock;
  enum class Phase { kIdle, kOpen, kClose, kTabOut, kTabIn };
  // Tests step the frames with their own clock; empty restores the steady one.
  void SetClock(std::function<Clock::time_point()> now);
  // The stage on the window (GameStylePanel::Layout): its left margin in dp
  // and dp per stage unit, for the curtains that reach the window's edges.
  void SetStage(float margin_dp, float scale) { margin_ = margin_dp; scale_ = scale; }
  // A newly loaded page; Detach before it goes.
  void Attach(Rml::ElementDocument* document);
  void Detach();
  // The rows of the page as just built, in order, and the focused one.
  void BindRows(const std::vector<Rml::Element*>& rows, int focused);
  // The focus moved: the cloud springs to the row.
  void Focus(int row);
  // Starts a phase over the bound elements; another running phase is settled first.
  void Start(Phase phase);
  // A frame: the phase and the focused label's pulse. Returns the phase that
  // ended in it, if any.
  Phase Advance();
  // After the context's layout: the cloud on the focused row. True when it changed.
  bool PlaceCursor();
  // The rest of the phase at once (input skips it); Advance then ends it.
  void Skip();
  // Without ending it: the phase's elements back to their document style.
  void Settle();
  Phase phase() const { return phase_; }

 private:
  struct Row { Rml::Element *row = nullptr, *name = nullptr, *label = nullptr; float start = 0; };
  float FramesSince(Clock::time_point when) const;
  float End() const;
  void Apply(float frame);
  std::function<Clock::time_point()> now_ = Clock::now;
  Phase phase_ = Phase::kIdle;
  Clock::time_point start_{}, opened_{}, focused_at_{};
  bool skipped_ = false;
  float margin_ = 0, scale_ = 1;
  Rml::ElementDocument* document_ = nullptr;
  Rml::Element *backdrop_ = nullptr, *paper_ = nullptr, *left_ = nullptr, *right_ = nullptr, *band_ = nullptr,
               *splash_ = nullptr, *stage_ = nullptr, *options_ = nullptr, *help_ = nullptr, *footer_ = nullptr,
               *cursor_ = nullptr, *pulsing_ = nullptr;
  std::vector<Rml::Element*> groups_;
  std::vector<Row> rows_;
  int focused_ = -1;
  // Where the phase has the panels and each row's label (stage units), for the cloud.
  float panels_ = 0;
  std::vector<float> slides_;
  std::vector<bool> shown_;
  std::array<float, 4> placed_{};  // The cloud's left, top, scale and opacity last set.
};
}  // namespace narutorise::menu
