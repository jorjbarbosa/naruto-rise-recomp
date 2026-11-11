#pragma once

// The catalog of the Flash movies that leave the 16:9 box: each by the name
// its element loads from Naruto_clip.SRA (elem+100), with its anchors and its
// HUD group. A movie outside the catalog stays in the 16:9 box. The first
// entries come from the movies' own layout (where the art rests on the stage);
// the diagnostic log (widescreen_log) lists the movies a session draws, to
// complete it. Plain data, no SDK.
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>

#include "widescreen/layout.h"

namespace widescreen {
// The HUD option a movie follows: hud_world, hud_fight, or none (always
// anchored, for anchors that only matter on taller screens or for screens
// that are not HUD). kShared is a piece seen in both modes: it follows the
// fight option while a fight piece is on screen.
enum class Group : uint8_t { kNone, kWorld, kFight, kShared };

// Copies of a piece along its own x axis, `repeat` stage pixels apart
// (RepeatPiece; a multiple of the art's own period, at most its width, so the
// copies meet without a gap or a seam): art that tiles (a strip of a
// repeating bitmap, a plain bar) reaches the screen's edges without being
// stretched. kBoth draws copies on both sides, kBefore
// only toward its own -x (the outer side of the player's bar; a mirrored bar
// gets them on its own outer side).
enum class Repeat : uint8_t { kNone, kBoth, kBefore };

// A piece of a movie by its depths from the root (path[0] the depth of a child
// of _level0, path[1] the depth of its child, ...) with its own anchors: the
// borders of the pause menu, the plain fill behind it, the sky of the main
// menu, the band of the dialog. A rule covers everything under its piece; the
// longest matching path wins.
struct ClipRule {
  static constexpr size_t kMaxPath = 6;
  std::array<uint16_t, kMaxPath> path{};
  uint8_t length = 0;
  uint8_t anchors = kAnchorNone;
  uint16_t repeat = 0;  // The distance between copies, in stage pixels.
  Repeat repeat_side = Repeat::kNone;
};
constexpr ClipRule Piece(std::initializer_list<uint16_t> path, uint8_t anchors, uint16_t repeat = 0,
                         Repeat side = Repeat::kNone) {
  ClipRule rule;
  for (uint16_t depth : path)
    if (rule.length < ClipRule::kMaxPath) rule.path[rule.length++] = depth;
  rule.anchors = anchors;
  rule.repeat = repeat;
  rule.repeat_side = repeat ? side : Repeat::kNone;
  return rule;
}

struct MovieRule {
  uint8_t anchors = kAnchorNone;
  Group group = Group::kNone;
  bool known = false;
  const ClipRule* clips = nullptr;  // The movie's pieces with anchors, if any.
  size_t clip_count = 0;
  // A 16:9 picture over the whole stage (the versus screen, the jutsu
  // sequences): while it draws, the screen outside the 16:9 box is black, as
  // the frame of a film, instead of the 3D scene behind it.
  bool framed = false;
  uint16_t id = 0;  // The catalog's entry (1 and up), 0 outside the catalog.
};

// The rule of the piece at `path` (depths from the root, `length` of them):
// the longest rule whose path starts it, null when none does.
const ClipRule* FindPiece(const MovieRule& rule, const uint16_t* path, size_t length);
// The anchors of the root piece at `depth` of a movie, kAnchorNone when the
// piece has no rule.
uint8_t ClipAnchors(const MovieRule& rule, int depth);
// The copies of a piece drawn on each side it repeats to: enough for `period`
// apart to reach `box_left` past the 16:9 box, plus one.
int RepeatCopies(float box_left, float period);

// The rule of a movie by its name: any case, '\' or '/', with or without an
// extension, matched on whole path components from the end ("HUD53" matches
// "Flash/HUD53.swf", not "OldHUD53").
MovieRule FindMovieRule(std::string_view name);

// The anchors in effect: a group set to the 16:9 center loses its left and
// right anchors; the vertical ones and the cover stay.
uint8_t EffectiveAnchors(const MovieRule& rule, HudPlacement world, HudPlacement fight, bool fight_on_screen);
}  // namespace widescreen
