// The look of the controls page on the game's stage (game_controls.rml): the
// same page Controls builds, with the movie's text outlines, the cursor's
// cloud on the focused row and text shrunk to its room. The document's styles
// (game_controls.rcss) do the rest.
#pragma once
#include <array>
namespace Rml { class Element; class ElementDocument; }
namespace narutorise::menu {
class GameStyleControls {
 public:
  void Attach(Rml::ElementDocument* document);
  void Detach();
  void Layout(int width, int height, float ratio);
  // The page's content was rebuilt.
  void Rebuilt();
  // A text changed in place: it may no longer fit.
  void Refit() { fit_passes_ = kFitPasses; }
  // After the context's update: true when the layout changed, for another update.
  bool FitText();
  bool PlaceCursor();

 private:
  void ApplyTextEffects();
  static constexpr int kFitPasses = 4;
  Rml::ElementDocument* document_ = nullptr;
  Rml::Element *stage_ = nullptr, *cursor_ = nullptr, *sidebar_ = nullptr, *detail_ = nullptr, *focused_ = nullptr;
  float text_px_ = 1;
  int fit_passes_ = 0;
  std::array<float, 4> placed_{};  // The cloud's left, top, scale and visibility last set.
};
}  // namespace narutorise::menu
