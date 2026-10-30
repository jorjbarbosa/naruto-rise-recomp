// The Options screen's motion, from captures of the game at 60 fps (game_style_motion.h).
#include "menu/game_style_motion.h"
#include <RmlUi/Core.h>
#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <numbers>
namespace narutorise::menu {
namespace {
// Measured on the game's own Options screen, frame by frame
// (out/menu-jogo-study-20261030/reference). Stage units; positive is right or down.
constexpr float kFrameMs = 1000.0f / 60;
// Opening, frames from the cut to the paper. The curtains come in linearly;
// each row's label slides in from the left, one row every four frames; the
// title band, the panels (with the values) and the prompts follow.
constexpr float kCurtainsIn = 16, kCurtainLeft = 260, kCurtainRight = 300;
constexpr float kRowsFirst = 7, kRowsApart = 4;
constexpr std::array kRowIn{-48.f, -36.f, -27.f, -16.f, -12.f, -8.f, -5.f, -4.f, -3.f, -2.f, -1.f, 0.f};
constexpr float kBandIn = 9;
constexpr std::array kBandFrom{-480.f, -344.f, -284.f, -230.f, -182.f, -102.f, -71.f, -45.f, -11.f, -3.f, 0.f};
constexpr float kPanelsIn = 13, kPanelsAway = -1200;  // Before kPanelsIn: beyond the window's left edge.
constexpr std::array kPanelsFrom{-960.f, -823.f, -686.f, -549.f, -275.f, -138.f, 0.f};
constexpr float kPromptsIn = 20;
constexpr std::array kPromptsFrom{26.f, 0.f};
constexpr float kSplashIn = 23, kSplashFrames = 5, kHelpIn = 19, kHelpFrames = 5;
// Closing, frames from the input: the paper goes at once, each row leaves
// one frame after the previous, then the panels, the band, the prompts and
// the curtains.
constexpr float kRowOut = 232, kPanelsOut = 137, kPromptsOut = 26;
constexpr std::array kBandOut{0.f, 0.f, -43.f, -86.f, -130.f, -217.f, -261.f, -348.f, -392.f, -436.f, -480.f};
constexpr float kCurtainsOut = 8, kCurtainsOutFrames = 15, kCloseEnd = kCurtainsOut + kCurtainsOutFrames;
// A tab switch: the closing's rows and panels, then the opening's from its first row.
constexpr float kTabInPanels = kPanelsIn - kRowsFirst;
// The cloud when the focus moves: scale and horizontal shift of its centre.
constexpr std::array kCloudScale{.52f, .63f, .70f, .76f, .85f, .98f, 1.09f, 1.15f, 1.22f, 1.28f, 1.28f, 1.22f, 1.17f,
                                 1.11f, 1.04f, .93f, .89f, .91f, .93f, 1.f, 1.02f, 1.04f, 1.09f, 1.07f, 1.04f, 1.f,
                                 .96f, .96f, .96f, .98f, 1.f};
constexpr std::array kCloudShift{-26.f, -22.5f, -21.f, -19.5f, -17.5f, -14.5f, -11.f, -9.5f, -8.f, -6.5f, -4.5f, -4.f,
                                 -3.f, -2.5f, -2.f, -.5f, -.5f, 0.f};
constexpr float kCloudLeft = -39.5f, kCloudTop = 4.167f;  // At rest, from its row.
// The focused label widens and narrows, anchored at its left.
constexpr float kPulseFrames = 50, kPulseGrowth = .0405f;

template <size_t N>
float Sample(const std::array<float, N>& table, float frame) {
  if (frame <= 0) return table.front();
  if (frame >= float(N - 1)) return table.back();
  const size_t at = size_t(frame);
  return table[at] + (table[at + 1] - table[at]) * (frame - float(at));
}
float Progress(float frame, float from, float to) { return std::clamp((frame - from) / (to - from), 0.0f, 1.0f); }
std::string Rem(float n) { return fmt::format("{:.3f}rem", n); }
void Set(Rml::Element* element, const char* property, std::string value) {
  if (element) element->SetProperty(property, std::move(value));
}
void Opacity(Rml::Element* element, float opacity) { Set(element, "opacity", fmt::format("{:.3f}", opacity)); }
}  // namespace

void GameStyleMotion::SetClock(std::function<Clock::time_point()> now) {
  now_ = now ? std::move(now) : std::function<Clock::time_point()>(Clock::now);
}

void GameStyleMotion::Attach(Rml::ElementDocument* document) {
  Detach();
  document_ = document;
  const auto by_id = [&](const char* id) { return document->GetElementById(id); };
  backdrop_ = by_id("backdrop"); paper_ = by_id("paper"); left_ = by_id("side-left"); right_ = by_id("side-right");
  band_ = by_id("tabs"); splash_ = by_id("splash"); stage_ = by_id("stage"); options_ = by_id("options");
  help_ = by_id("help"); footer_ = by_id("footer"); cursor_ = by_id("cursor");
  opened_ = now_();
}

void GameStyleMotion::Detach() {
  Settle();
  document_ = nullptr;
  backdrop_ = paper_ = left_ = right_ = band_ = splash_ = stage_ = options_ = help_ = footer_ = cursor_ = pulsing_ = nullptr;
  groups_.clear(); rows_.clear(); slides_.clear(); shown_.clear();
  focused_ = -1; placed_ = {};
}

void GameStyleMotion::BindRows(const std::vector<Rml::Element*>& rows, int focused) {
  if (!document_) return;
  band_ = document_->GetElementById("tabs"); splash_ = document_->GetElementById("splash");  // Rebuilt with the tabs.
  groups_.clear(); rows_.clear();
  for (auto* row : rows) {
    if (!row) continue;
    rows_.push_back({row, row->QuerySelector(".name"), row->QuerySelector(".label")});
    if (groups_.empty() || groups_.back() != row->GetParentNode()) groups_.push_back(row->GetParentNode());
  }
  slides_.assign(rows_.size(), 0);
  shown_.assign(rows_.size(), true);
  focused_ = focused; pulsing_ = nullptr; placed_ = {};
}

void GameStyleMotion::Focus(int row) {
  if (row == focused_) return;
  focused_ = row;
  focused_at_ = now_();
}

void GameStyleMotion::Start(Phase phase) {
  Settle();
  if (!document_ || phase == Phase::kIdle) return;
  phase_ = phase; start_ = now_(); skipped_ = false;
  for (size_t i = 0; i < rows_.size(); ++i)
    rows_[i].start = phase == Phase::kOpen ? kRowsFirst + kRowsApart * float(i)
                   : phase == Phase::kTabIn ? kRowsApart * float(i) : float(i);
  // The cloud arrives with its row.
  if ((phase == Phase::kOpen || phase == Phase::kTabIn) && focused_ >= 0 && focused_ < int(rows_.size()))
    focused_at_ = start_ + std::chrono::duration_cast<Clock::duration>(
                               std::chrono::duration<float, std::milli>(rows_[size_t(focused_)].start * kFrameMs));
  Apply(0);
}

float GameStyleMotion::FramesSince(Clock::time_point when) const {
  // A hair over, so a frame due now counts as reached.
  return std::chrono::duration<float, std::milli>(now_() - when).count() / kFrameMs + 1e-3f;
}

float GameStyleMotion::End() const {
  const float rows = rows_.empty() ? 0 : rows_.back().start;
  const float cloud = focused_ >= 0 && focused_ < int(rows_.size()) ? rows_[size_t(focused_)].start + kCloudScale.size() : 0;
  switch (phase_) {
    case Phase::kOpen: return std::max({kSplashIn + kSplashFrames, kPromptsIn + 1, rows + kRowIn.size(), cloud});
    case Phase::kClose: return kCloseEnd;
    case Phase::kTabOut: return std::max(9.f, rows + 3);  // The panels are gone by frame 8.
    case Phase::kTabIn: return std::max({kTabInPanels + kPanelsFrom.size(), rows + kRowIn.size(), cloud});
    case Phase::kIdle: break;
  }
  return 0;
}

GameStyleMotion::Phase GameStyleMotion::Advance() {
  // The focused label's pulse runs while the page shows.
  Rml::Element* label = focused_ >= 0 && focused_ < int(rows_.size()) ? rows_[size_t(focused_)].label : nullptr;
  if (pulsing_ && pulsing_ != label) pulsing_->RemoveProperty("transform");
  if ((pulsing_ = label)) {
    const float wave = (1 - std::cos(2 * std::numbers::pi_v<float> * FramesSince(opened_) / kPulseFrames)) / 2;
    Set(label, "transform", fmt::format("scaleX({:.4f})", 1 + kPulseGrowth * wave));
  }
  if (phase_ == Phase::kIdle) return Phase::kIdle;
  const float end = End(), frame = skipped_ ? end : std::min(FramesSince(start_), end);
  Apply(frame);
  if (frame < end) return Phase::kIdle;
  const Phase ended = phase_;
  if (ended == Phase::kClose) phase_ = Phase::kIdle;  // A closed page stays as it left until its document goes.
  else Settle();
  return ended;
}

void GameStyleMotion::Skip() { if (phase_ != Phase::kIdle) skipped_ = true; }

void GameStyleMotion::Apply(float q) {
  const auto curtains = [&](float out) {  // 0: in place, 1: beyond the window's edges.
    Set(left_, "margin-left", fmt::format("{:.3f}dp", -(margin_ + kCurtainLeft * scale_) * out));
    Set(right_, "margin-left", fmt::format("{:.3f}dp", (margin_ + kCurtainRight * scale_) * out));
  };
  const auto panels = [&](float offset) {
    panels_ = offset;
    for (auto* group : groups_) Set(group, "margin-left", Rem(40 + offset));
  };
  // Each label slides on its own, against its panel's offset. A margin, not
  // a transform: RmlUi culls text by its layout box unless its own parent
  // is transformed.
  const auto rows = [&](auto slide) {
    for (size_t i = 0; i < rows_.size(); ++i) {
      const float t = q - rows_[i].start;
      shown_[i] = t >= 0 || phase_ == Phase::kClose || phase_ == Phase::kTabOut;
      slides_[i] = slide(t);
      Set(rows_[i].name, "margin-left", Rem(slides_[i] - panels_));
      Opacity(rows_[i].name, shown_[i] && slides_[i] > -900 ? 1 : 0);
    }
  };
  const auto arriving = [&](float from) { return q < from ? kPanelsAway : Sample(kPanelsFrom, q - from); };
  const auto leaving = [](float t) { return t >= 0 ? -kRowOut * (t + 1) : 0.f; };
  switch (phase_) {
    case Phase::kOpen:
      curtains(1 - Progress(q, 0, kCurtainsIn));
      Opacity(band_, q >= kBandIn ? 1 : 0);
      Set(band_, "margin-left", Rem(Sample(kBandFrom, q - kBandIn)));
      Set(splash_, "width", Rem(q < kSplashIn ? 0 : 152.667f * (.94f + .06f * Progress(q, kSplashIn, kSplashIn + kSplashFrames))));
      Set(splash_, "height", Rem(q < kSplashIn ? 0 : 117.333f * (.58f + .42f * Progress(q, kSplashIn, kSplashIn + kSplashFrames))));
      panels(arriving(kPanelsIn));
      rows([](float t) { return Sample(kRowIn, t); });
      Opacity(footer_, q >= kPromptsIn ? 1 : 0);
      Set(footer_, "margin-top", Rem(Sample(kPromptsFrom, q - kPromptsIn)));
      Opacity(help_, Progress(q, kHelpIn, kHelpIn + kHelpFrames));
      break;
    case Phase::kClose:
      Opacity(backdrop_, 0); Opacity(paper_, 0); Opacity(help_, 0);
      panels(q >= 1 ? -kPanelsOut * (q - 1) : 0);
      rows(leaving);
      Set(band_, "margin-left", Rem(Sample(kBandOut, q)));
      Opacity(band_, q < float(kBandOut.size() - 1) ? 1 : 0);
      Set(footer_, "margin-top", Rem(q >= 1 ? kPromptsOut * (q - 1) : 0));
      Opacity(footer_, q < 5 ? 1 : 0);
      curtains(Progress(q, kCurtainsOut, kCloseEnd));
      break;
    case Phase::kTabOut:
      Opacity(help_, 0);
      panels(q >= 1 ? -kPanelsOut * (q - 1) : 0);
      rows(leaving);
      break;
    case Phase::kTabIn:
      panels(arriving(kTabInPanels));
      rows([](float t) { return Sample(kRowIn, t); });
      Opacity(help_, Progress(q, kTabInPanels + kPanelsFrom.size(), kTabInPanels + kPanelsFrom.size() + kHelpFrames));
      break;
    case Phase::kIdle: break;
  }
}

bool GameStyleMotion::PlaceCursor() {
  if (!cursor_ || !stage_ || !document_) return false;
  const bool valid = focused_ >= 0 && focused_ < int(rows_.size());
  const float t = FramesSince(focused_at_), rem = document_->GetComputedValues().font_size();
  std::array<float, 4> place{0, 0, 1, 0};
  if (valid && rem > 0 && t >= 0 && shown_[size_t(focused_)]) {
    auto* row = rows_[size_t(focused_)].row;
    // Only while its row is inside the list's view.
    const float top = row->GetAbsoluteTop(), bottom = top + row->GetOffsetHeight();
    const bool inside = !options_ || (top >= options_->GetAbsoluteTop() - 1 &&
                                      bottom <= options_->GetAbsoluteTop() + options_->GetClientHeight() + 1);
    place = {(row->GetAbsoluteLeft() - stage_->GetAbsoluteLeft()) / rem - panels_ + kCloudLeft +
                 slides_[size_t(focused_)] + Sample(kCloudShift, t),
             (top - stage_->GetAbsoluteTop()) / rem + kCloudTop, Sample(kCloudScale, t),
             inside && slides_[size_t(focused_)] > -900 ? 1.f : 0.f};
  }
  const auto rounded = [](const std::array<float, 4>& a) {
    std::array<float, 4> r{};
    for (size_t i = 0; i < a.size(); ++i) r[i] = std::round(a[i] * 100) / 100;
    return r;
  };
  if (rounded(place) == rounded(placed_) && cursor_->IsVisible() == (place[3] > 0)) return false;
  placed_ = place;
  Set(cursor_, "display", place[3] > 0 ? "block" : "none");
  Set(cursor_, "left", Rem(place[0])); Set(cursor_, "top", Rem(place[1]));
  Set(cursor_, "transform", fmt::format("scale({:.3f})", place[2]));
  return true;
}

void GameStyleMotion::Settle() {
  if (phase_ == Phase::kIdle) return;
  const auto clear = [](Rml::Element* element, std::initializer_list<const char*> properties) {
    if (element) for (const char* property : properties) element->RemoveProperty(property);
  };
  clear(backdrop_, {"opacity"}); clear(paper_, {"opacity"});
  for (auto* element : {left_, right_}) clear(element, {"margin-left"});
  clear(band_, {"margin-left", "opacity"});
  clear(splash_, {"width", "height"});
  for (auto* group : groups_) clear(group, {"margin-left"});
  for (const auto& row : rows_) clear(row.name, {"margin-left", "opacity"});
  clear(footer_, {"margin-top", "opacity"});
  clear(help_, {"opacity"});
  panels_ = 0;
  std::fill(slides_.begin(), slides_.end(), 0.f);
  std::fill(shown_.begin(), shown_.end(), true);
  phase_ = Phase::kIdle;
}
}  // namespace narutorise::menu
