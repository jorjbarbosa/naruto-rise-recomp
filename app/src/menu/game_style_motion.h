// The choreography of the game's Options movie on game_menu.rml
// (GameStylePanel): the opening, the closing, and the panels leaving and
// entering on a tab switch. Times are frames of the movie's 60 fps from a
// steady clock; lengths are units of its 1280 x 720 stage (rem). The curtains
// slide in from the sides, the title band from the left, the panels after it
// and the prompts from below; then each line's text is stamped (scale 2 to 1,
// a short drop, fading in) one line after the other, and the cursor's cloud
// pops. Only left, top, sizes and opacity move, plus the transform of leaf
// text and images: the list keeps clipping with the scissor, as the render
// interface has no clip masks (render_interface.h).
#pragma once
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
  // Starts a phase over the document's elements as they are now; another
  // running phase is settled first. focused: the row whose cursor pops.
  void Start(Phase phase, Rml::ElementDocument* document, int focused);
  // A frame: moves the elements. Returns the phase that ended in it, if any.
  Phase Advance();
  // The rest of the phase at once (input skips it); Advance then ends it.
  void Skip();
  // Without ending it: every element back to its document style.
  void Settle();
  Phase phase() const { return phase_; }

 private:
  struct Fill { Rml::Element* element = nullptr; float width = 0; };  // Its width in rem.
  struct Line { std::vector<Rml::Element*> stamped, faded; Fill fill{}; float start = 0; };
  float Frame() const;
  float End() const;
  void Apply(float frame);
  void Collect(Rml::ElementDocument* document, int focused);
  void Clear();
  std::function<Clock::time_point()> now_ = Clock::now;
  Phase phase_ = Phase::kIdle;
  Clock::time_point start_{};
  bool skipped_ = false;
  float margin_ = 0, scale_ = 1;
  Rml::ElementDocument* document_ = nullptr;
  Rml::Element *paper_ = nullptr, *left_ = nullptr, *right_ = nullptr, *band_ = nullptr, *splash_ = nullptr,
               *options_ = nullptr, *help_ = nullptr, *footer_ = nullptr, *cursor_ = nullptr;
  std::vector<Rml::Element*> tabs_, footer_text_;
  std::vector<Line> lines_;
  float cursor_start_ = 0;
};
}  // namespace narutorise::menu
