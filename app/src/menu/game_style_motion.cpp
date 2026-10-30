// The Options movie's in- and out-animations on the game-style settings (game_style_motion.h).
#include "menu/game_style_motion.h"
#include <RmlUi/Core.h>
#include <algorithm>
#include <cmath>
#include <fmt/format.h>
namespace narutorise::menu {
namespace {
// Frames of the movie (out/menu-jogo-study-20261030/transicao-menu-opcoes.md)
// and travels in stage units.
constexpr float kFrameMs = 1000.0f / 60;
constexpr float kCurtainLeft = 260, kCurtainRight = 300, kBandTravel = 480, kPanelTravel = 960, kPromptRise = 130;
constexpr float kStampFrames = 8, kStampOutFrames = 4, kStampDrop = 57;
constexpr float kCursorFrames = 22, kCursorRise = 8, kFillFrames = 11;
// Lines are stamped one after another, at most kStaggerSpan frames from the first to the last.
constexpr float kLineStagger = 2, kStaggerSpan = 16;
constexpr float kOpenLines = 24, kOpenTabs = 20, kOpenPrompts = 27, kTabInLines = 7;
constexpr float kOpenEnd = 27, kCloseEnd = 27, kTabOutEnd = 7;
// The ink splash's size in game_menu.rcss, and the share of it the movie's mask shows first.
constexpr float kSplashWidth = 152.667f, kSplashHeight = 117.333f, kSplashFirstWidth = .94f, kSplashFirstHeight = .58f;

float Progress(float frame, float from, float to) { return std::clamp((frame - from) / (to - from), 0.0f, 1.0f); }
float EaseOut(float p) { return 1 - (1 - p) * (1 - p); }
float EaseInOut(float p) { return p < .5f ? 2 * p * p : 1 - 2 * (1 - p) * (1 - p); }
float Mix(float from, float to, float p) { return from + (to - from) * p; }
std::string Rem(float n) { return fmt::format("{:.3f}rem", n); }
void Opacity(Rml::Element* element, float opacity) {
  if (element) element->SetProperty("opacity", fmt::format("{:.3f}", std::clamp(opacity, 0.0f, 1.0f)));
}
void Transform(Rml::Element* element, float drop, float scale) {
  if (element) element->SetProperty("transform", fmt::format("translateY({:.3f}rem) scale({:.3f})", drop, scale));
}
// Letters falling into place from twice their size; frame counts from the stamp's start.
void StampIn(Rml::Element* element, float frame) {
  const float p = EaseOut(Progress(frame, 0, kStampFrames));
  Transform(element, -kStampDrop * (1 - p), 2 - p);
  Opacity(element, p);
}
// Letters growing to three times their size as they fade.
void StampOut(Rml::Element* element, float frame) {
  const float p = Progress(frame, 0, kStampOutFrames);
  Transform(element, 0, 1 + 2 * p);
  Opacity(element, 1 - p);
}
void Set(Rml::Element* element, const char* property, std::string value) {
  if (element) element->SetProperty(property, std::move(value));
}
}  // namespace

void GameStyleMotion::SetClock(std::function<Clock::time_point()> now) {
  now_ = now ? std::move(now) : std::function<Clock::time_point()>(Clock::now);
}

void GameStyleMotion::Start(Phase phase, Rml::ElementDocument* document, int focused) {
  Settle();
  if (!document || phase == Phase::kIdle) return;
  phase_ = phase; start_ = now_(); skipped_ = false;
  Clear();
  Collect(document, focused);
  Apply(0);
}

void GameStyleMotion::Collect(Rml::ElementDocument* document, int focused) {
  document_ = document;
  const auto by_id = [&](const char* id) { return document->GetElementById(id); };
  paper_ = by_id("paper"); left_ = by_id("side-left"); right_ = by_id("side-right"); band_ = by_id("tabs");
  splash_ = by_id("splash"); options_ = by_id("options"); help_ = by_id("help"); footer_ = by_id("footer");
  if (phase_ != Phase::kTabOut) {
    Rml::ElementList tabs;
    document->QuerySelectorAll(tabs, phase_ == Phase::kTabIn ? "#tabs .tab.selected" : "#tabs .tab");
    tabs_ = tabs;
  }
  if (phase_ == Phase::kOpen || phase_ == Phase::kClose)
    for (const char* id : {"back-label", "ok-label", "legend-label"}) footer_text_.push_back(by_id(id));
  if (phase_ == Phase::kOpen || phase_ == Phase::kClose)
    if (auto* star = document->QuerySelector("#legend .star")) footer_text_.push_back(star);
  Rml::ElementList rows;
  document->QuerySelectorAll(rows, "#options .row");
  const float first = phase_ == Phase::kTabIn ? kTabInLines : kOpenLines;
  const float stagger = rows.size() > 1 ? std::min(kLineStagger, kStaggerSpan / float(rows.size() - 1)) : 0;
  for (size_t i = 0; i < rows.size(); ++i) {
    Line line;
    rows[i]->QuerySelectorAll(line.stamped, ".label, .name .star, .value, .word, .number");
    rows[i]->QuerySelectorAll(line.faded, ".orb, .arrow, .chevron, .track");
    if (auto* fill = rows[i]->QuerySelector(".fill"))
      if (const auto* width = fill->GetLocalProperty("width")) line.fill = {fill, width->Get<float>()};
    line.start = first + float(i) * stagger;
    if (int(i) == focused) { cursor_ = rows[i]->QuerySelector(".cursor"); cursor_start_ = line.start; }
    lines_.push_back(std::move(line));
  }
}

float GameStyleMotion::Frame() const {
  return std::chrono::duration<float, std::milli>(now_() - start_).count() / kFrameMs;
}

float GameStyleMotion::End() const {
  switch (phase_) {
    case Phase::kOpen: case Phase::kTabIn: {
      float end = phase_ == Phase::kOpen ? std::max(kOpenEnd, kOpenPrompts + kStampFrames) : kTabInLines;
      if (phase_ == Phase::kOpen && !tabs_.empty()) end = std::max(end, kOpenTabs + float(tabs_.size() - 1) + kStampFrames);
      if (phase_ == Phase::kTabIn && !tabs_.empty()) end = std::max(end, kStampFrames);
      for (const auto& line : lines_) end = std::max(end, line.start + std::max(kStampFrames, kFillFrames));
      return cursor_ ? std::max(end, cursor_start_ + kCursorFrames) : end;
    }
    case Phase::kClose: return kCloseEnd;
    case Phase::kTabOut: return kTabOutEnd;
    case Phase::kIdle: break;
  }
  return 0;
}

GameStyleMotion::Phase GameStyleMotion::Advance() {
  if (phase_ == Phase::kIdle) return Phase::kIdle;
  const float end = End(), frame = skipped_ ? end : std::min(Frame(), end);
  Apply(frame);
  if (frame < end) return Phase::kIdle;
  const Phase ended = phase_;
  // A closed page stays faded out until its document goes.
  if (ended == Phase::kClose) { phase_ = Phase::kIdle; Clear(); }
  else Settle();
  return ended;
}

void GameStyleMotion::Skip() { if (phase_ != Phase::kIdle) skipped_ = true; }

void GameStyleMotion::Apply(float q) {
  const auto curtains = [&](float out) {  // 0: in place, 1: beyond the window's edges.
    Set(left_, "margin-left", fmt::format("{:.3f}dp", -(margin_ + kCurtainLeft * scale_) * out));
    Set(right_, "margin-left", fmt::format("{:.3f}dp", (margin_ + kCurtainRight * scale_) * out));
  };
  const auto veil = [&](float shown) {
    document_->SetProperty("background-color", fmt::format("#000000{:02x}", int(std::lround(255 * shown))));
    Opacity(paper_, shown);
  };
  const auto band = [&](float offset, float opacity) { Set(band_, "margin-left", Rem(offset)); Opacity(band_, opacity); };
  const auto panels = [&](float offset, float opacity) { Set(options_, "margin-left", Rem(offset)); Opacity(options_, opacity); };
  const auto prompts = [&](float drop) { Set(footer_, "margin-top", Rem(drop)); };
  const auto lines_in = [&] {
    for (const auto& line : lines_) {
      for (auto* element : line.stamped) StampIn(element, q - line.start);
      for (auto* element : line.faded) Opacity(element, EaseOut(Progress(q, line.start, line.start + kStampFrames)));
      if (line.fill.element) Set(line.fill.element, "width", Rem(line.fill.width * Progress(q, line.start, line.start + kFillFrames)));
    }
    if (cursor_) {
      const float t = q - cursor_start_;
      Transform(cursor_, 0, t < kCursorRise ? Mix(.52f, 1.32f, EaseOut(Progress(t, 0, kCursorRise)))
                                            : Mix(1.32f, 1, EaseInOut(Progress(t, kCursorRise, kCursorFrames))));
      Opacity(cursor_, t >= 0 ? 1 : 0);
    }
  };
  const auto lines_out = [&] {
    for (const auto& line : lines_) {
      for (auto* element : line.stamped) StampOut(element, q);
      for (auto* element : line.faded) Opacity(element, 1 - Progress(q, 0, kStampOutFrames));
    }
    Opacity(cursor_, 1 - Progress(q, 0, kStampOutFrames));
  };
  switch (phase_) {
    case Phase::kOpen: {
      veil(Progress(q, 0, 6));
      curtains(1 - Progress(q, 4, 20));
      band(-kBandTravel * (1 - EaseOut(Progress(q, 11, 24))), Progress(q, 11, 14));
      // The splash grows out of its mask once the band slows down.
      const float splash = Progress(q, 18, 25);
      Set(splash_, "width", Rem(q < 18 ? 0 : kSplashWidth * Mix(kSplashFirstWidth, 1, splash)));
      Set(splash_, "height", Rem(q < 18 ? 0 : kSplashHeight * Mix(kSplashFirstHeight, 1, splash)));
      panels(-kPanelTravel * (1 - Progress(q, 17, 24)), Progress(q, 17, 19));
      prompts(kPromptRise * (1 - Progress(q, 22, 27)));
      Opacity(help_, Progress(q, 22, 27));
      for (size_t i = 0; i < tabs_.size(); ++i) StampIn(tabs_[i], q - kOpenTabs - float(i));
      for (auto* element : footer_text_) StampIn(element, q - kOpenPrompts);
      lines_in();
      break;
    }
    case Phase::kClose:
      for (auto* element : tabs_) StampOut(element, q);
      for (auto* element : footer_text_) StampOut(element, q);
      lines_out();
      Opacity(help_, 1 - Progress(q, 0, 4));
      panels(-kPanelTravel * Progress(q, 3, 9), 1 - Progress(q, 6, 9));
      prompts(kPromptRise * Progress(q, 3, 7));
      band(-kBandTravel * Progress(q, 3, 13), 1 - Progress(q, 9, 13));
      curtains(Progress(q, 12, 25));
      veil(1 - Progress(q, 20, 26));
      break;
    case Phase::kTabOut:
      lines_out();
      Opacity(help_, 1 - Progress(q, 0, 4));
      panels(-kPanelTravel * Progress(q, 0, kTabOutEnd), 1 - Progress(q, 4, kTabOutEnd));
      break;
    case Phase::kTabIn:
      panels(-kPanelTravel * (1 - Progress(q, 0, kTabInLines)), Progress(q, 0, 2));
      Opacity(help_, Progress(q, 5, 10));
      for (auto* element : tabs_) StampIn(element, q);
      lines_in();
      break;
    case Phase::kIdle: break;
  }
}

void GameStyleMotion::Settle() {
  if (phase_ == Phase::kIdle) return;
  const auto clear = [](Rml::Element* element, std::initializer_list<const char*> properties) {
    if (element) for (const char* property : properties) element->RemoveProperty(property);
  };
  clear(document_, {"background-color"});
  clear(paper_, {"opacity"});
  for (auto* element : {left_, right_}) clear(element, {"margin-left"});
  clear(band_, {"margin-left", "opacity"});
  clear(splash_, {"width", "height"});
  clear(options_, {"margin-left", "opacity"});
  clear(help_, {"opacity"});
  clear(footer_, {"margin-top"});
  clear(cursor_, {"transform", "opacity"});
  for (auto* element : tabs_) clear(element, {"transform", "opacity"});
  for (auto* element : footer_text_) clear(element, {"transform", "opacity"});
  for (const auto& line : lines_) {
    for (auto* element : line.stamped) clear(element, {"transform", "opacity"});
    for (auto* element : line.faded) clear(element, {"opacity"});
    if (line.fill.element) line.fill.element->SetProperty("width", Rem(line.fill.width));
  }
  phase_ = Phase::kIdle;
  Clear();
}

void GameStyleMotion::Clear() {
  document_ = nullptr;
  paper_ = left_ = right_ = band_ = splash_ = options_ = help_ = footer_ = cursor_ = nullptr;
  tabs_.clear(); footer_text_.clear(); lines_.clear();
  cursor_start_ = 0;
}
}  // namespace narutorise::menu
