// The widescreen mode's arithmetic (widescreen/layout.h) and Flash catalog
// (widescreen/catalog.h), without the game.
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

#include "widescreen/catalog.h"
#include "widescreen/layout.h"

using namespace widescreen;

namespace {
int checks = 0;
void Check(bool value, const std::string& message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}
bool Near(float a, float b, float tolerance = 1e-4f) { return std::abs(a - b) <= tolerance; }
std::string Text(Size s) { return std::to_string(s.width) + "x" + std::to_string(s.height); }
}  // namespace

int main() {
  try {
    // Logical screen.
    const auto size = [](uint32_t w, uint32_t h) { return LogicalSize(AspectMode::kAuto, w, h); };
    Check(size(1920, 1080) == kStandardSize, "16:9 stays 1280x720: " + Text(size(1920, 1080)));
    Check(size(1366, 768) == kStandardSize, "1366x768 rounds to 1280x720: " + Text(size(1366, 768)));
    Check(size(2520, 1080) == Size{1680, 720}, "21:9 (7:3): " + Text(size(2520, 1080)));
    Check(size(3440, 1440) == Size{1728, 720}, "3440x1440: " + Text(size(3440, 1440)));
    Check(size(2560, 1080) == Size{1712, 720}, "2560x1080: " + Text(size(2560, 1080)));
    Check(size(5120, 1440) == Size{2560, 720}, "32:9: " + Text(size(5120, 1440)));
    Check(size(7680, 1080) == Size{2560, 720}, "wider than 32:9 is 32:9: " + Text(size(7680, 1080)));
    Check(size(1920, 1200) == Size{1280, 800}, "16:10: " + Text(size(1920, 1200)));
    Check(size(1600, 1200) == Size{1280, 960}, "4:3: " + Text(size(1600, 1200)));
    Check(size(1000, 1200) == Size{1280, 960}, "taller than 4:3 is 4:3: " + Text(size(1000, 1200)));
    Check(size(0, 1080) == kStandardSize && size(1920, 0) == kStandardSize, "an empty window keeps 1280x720");
    Check(LogicalSize(AspectMode::kStandard, 5120, 1440) == kStandardSize, "the 16:9 option keeps 1280x720");
    for (uint32_t w : {800u, 1280u, 1600u, 2560u, 3440u, 3840u, 5120u})
      for (uint32_t h : {600u, 720u, 1080u, 1200u, 1440u, 2160u}) {
        const Size s = size(w, h);
        Check(s.width % 16 == 0 && s.height % 16 == 0, "multiples of 16: " + Text(s));
        Check(s.width >= 1280 && s.height >= 720 && (s.width == 1280 || s.height == 720), "one side at base: " + Text(s));
      }
    // The engine's surfaces stop at 8192: the screen times the scale stays within it.
    Check(LogicalSize(AspectMode::kAuto, 5120, 1440, 4, 4) == Size{2048, 720}, "32:9 at 4x narrows to 2048x720: " +
          Text(LogicalSize(AspectMode::kAuto, 5120, 1440, 4, 4)));
    Check(LogicalSize(AspectMode::kAuto, 5120, 1440, 3, 3) == Size{2560, 720}, "32:9 at 3x fits");
    Check(LogicalSize(AspectMode::kAuto, 3440, 1440, 4, 4) == Size{1728, 720}, "21:9 at 4x fits");
    Check(LogicalSize(AspectMode::kAuto, 1600, 1200, 4, 4) == Size{1280, 960}, "4:3 at 4x fits");
    Check(LogicalSize(AspectMode::kAuto, 1920, 1080, 4, 4) == kStandardSize, "16:9 at 4x stays 1280x720");
    for (const float k : {1.0f, 1.5f, 2.0f, 3.0f, 4.0f})
      for (uint32_t w : {960u, 1280u, 2560u, 3440u, 5120u, 7680u}) {
        const Size s = LogicalSize(AspectMode::kAuto, w, 1080, k, k);
        Check(s.width * k <= kMaxSurfaceSide && s.height * k <= kMaxSurfaceSide && s.width % 16 == 0,
              "within the engine's surfaces at " + std::to_string(k) + "x: " + Text(s));
      }
    Check(UnpackSize(PackSize(Size{1728, 720})) == Size{1728, 720}, "a packed screen unpacks");

    // Options as narutorise.toml spells them.
    Check(ParseAspectMode("16:9") == AspectMode::kStandard && ParseAspectMode("auto") == AspectMode::kAuto &&
              ParseAspectMode("") == AspectMode::kAuto, "aspect_ratio");
    Check(ParseHudPlacement("center", HudPlacement::kEdges) == HudPlacement::kCenter &&
              ParseHudPlacement("edges", HudPlacement::kCenter) == HudPlacement::kEdges &&
              ParseHudPlacement("?", HudPlacement::kCenter) == HudPlacement::kCenter, "hud_world / hud_fight");

    // EDRAM: the game's own counts at 1280 x 720 (820E6B00).
    Check(EdramTiles(1280, 720, 0) == 720, "the main color is 720 tiles (the li 720 of 820E6CF4)");
    Check(EdramTiles(1280, 256, 2) == 1024, "a 1280x256 4X tile is 1024 tiles (+508, +512 at 1024)");
    Check(EdramTiles(640, 360, 2) == 720, "the 640x360 4X half-resolution depth is 720 tiles");
    Check(EdramTiles(1680, 720, 0) == 945, "1680x720 color");
    Check(EdramBase(720, 720) == 720, "1280x720: the depth stays after the color");
    Check(EdramBase(1440, 1440) == 608, "2560x720: the depth moves down to fit 2048");
    Check(EdramBase(945, 945) == 945, "1680x720 fits");
    for (const Size s : {Size{1280, 720}, Size{1680, 720}, Size{2560, 720}, Size{1280, 960}}) {
      const uint32_t color = EdramTiles(s.width, s.height, 0), half = EdramTiles(s.width / 2, s.height / 2, 2);
      Check(EdramBase(color, color) + color <= kEdramTiles, "main depth fits: " + Text(s));
      Check(EdramBase(half, half) + half <= kEdramTiles, "half depth fits: " + Text(s));
      Check(MainDepthEdramBase(s) == EdramBase(color, color), "the main depth's base: " + Text(s));
      Check(HalfDepthEdramBase(s, half) + half <= kEdramTiles, "the half depth's base: " + Text(s));
    }
    Check(MainDepthEdramBase(kStandardSize) == 720, "1280x720: the game's own base of +520");
    Check(HalfDepthEdramBase(kStandardSize, 720) == 720, "1280x720: the game's own base of +524");

    // Tiles: the game's table at 1280 x 720.
    const auto tiles = TileRects(kStandardSize);
    Check(tiles[0] == std::array<uint32_t, 4>{0, 0, 1280, 256} && tiles[1] == std::array<uint32_t, 4>{0, 256, 1280, 512} &&
              tiles[2] == std::array<uint32_t, 4>{0, 512, 1280, 720}, "the table at 82081798");
    const auto tall = TileRects({1280, 960});
    Check(tall[2][3] == 960 && tall[0][3] == 320 && tall[1][3] == 640, "4:3 tiles cover 960 rows");
    Check(TileRects({2560, 720})[1][2] == 2560, "32:9 tiles cover 2560 columns");

    // Vert+.
    const float fov = 1.0f;
    Check(VertPlusFov(fov, 0.5625f) == fov && VertPlusFov(fov, 0.4f) == fov, "no change at 16:9 and wider");
    const float tall_fov = VertPlusFov(fov, 0.75f);
    Check(Near(std::tan(tall_fov / 2) / 0.75f, std::tan(fov / 2) / 0.5625f), "4:3 keeps the 16:9 horizontal extent");

    // Thumbnail crop: 840x360 into 320x180, 1280x480 into 320x180, 640x480 into 320x180.
    Viewport v = CropViewport(840, 360, 320, 180);
    Check(Near(v.x, -50) && Near(v.y, 0) && Near(v.width, 420) && Near(v.height, 180), "21:9 crop");
    v = CropViewport(640, 480, 320, 180);
    Check(Near(v.x, 0) && Near(v.y, -30) && Near(v.width, 320) && Near(v.height, 240), "4:3 crop");
    v = CropViewport(640, 360, 320, 180);
    Check(Near(v.x, 0) && Near(v.y, 0) && Near(v.width, 320) && Near(v.height, 180), "16:9 is not cropped");

    // Flash rectangles.
    const Rect full{};
    Check(MovieRect(kStandardSize, full, kAnchorLeft) == full, "1280x720 leaves the rectangle");
    const Size wide{1680, 720};
    Rect r = MovieRect(wide, full, kAnchorNone);
    Check(Near(r.x * 1680, 200) && Near(r.w * 1680, 1280) && Near(r.y, 0) && Near(r.h, 1), "center box at 21:9");
    r = MovieRect(wide, full, kAnchorLeft);
    Check(Near(r.x, 0) && Near(r.w * 1680, 1280), "left anchor");
    r = MovieRect(wide, full, kAnchorRight);
    Check(Near(r.x * 1680 + r.w * 1680, 1680), "right anchor ends at the right edge");
    r = MovieRect(wide, full, kAnchorCover);
    Check(Near(r.x, 0) && Near(r.w, 1) && r.h > 1 && Near(r.y, (1 - r.h) / 2), "cover fills the width, crops the height");
    const Size tall_screen{1280, 960};
    r = MovieRect(tall_screen, full, kAnchorBottom);
    Check(Near(r.x, 0) && Near(r.w, 1) && Near(r.y * 960 + r.h * 960, 960) && Near(r.h * 960, 720), "bottom anchor at 4:3");
    r = MovieRect(tall_screen, full, kAnchorLeft);
    Check(Near(r.x, 0) && Near(r.y * 960, 120), "left does nothing at 4:3; centered vertically");
    // A marker: the script puts the stage's middle on a character at 47% of
    // the screen (x = 0.47 - 0.5); the middle must land there at any width.
    const Rect marker{-0.03f, 0.2f, 1, 1};
    r = MovieRect(wide, marker, kAnchorLeft);
    Check(Near(r.x * 1680 + 640, 0.47f * 1680, 0.01f) && Near(r.y, 0.2f) && Near(r.w * 1680, 1280),
          "a marker's stage middle stays on its character, at the stage's pixel size");
    r = MovieRect(tall_screen, marker, kAnchorNone);
    Check(Near(r.y * 960 + 360, 0.7f * 960, 0.01f), "and vertically on a 4:3 screen");

    // Catalog.
    Check(FindMovieRule("HUD53").known && FindMovieRule("Flash\\hud53.swf").group == Group::kShared, "HUD53 by any spelling");
    const MovieRule bars = FindMovieRule("ScriptedEvent\\BlackStripes");
    Check(ClipAnchors(bars, 2) == (kAnchorExtend | kAnchorTop) && ClipAnchors(bars, 1) == (kAnchorExtend | kAnchorBottom),
          "cutscene bars extend at the top and bottom edges");
    Check(FindMovieRule("Black_Fade").anchors == kAnchorFill, "fades fill the screen");
    Check(!FindMovieRule("OldHUD53").known, "whole components only");
    Check(FindMovieRule("Menu/KonohaBackground").anchors == kAnchorCover, "Konoha background covers");
    Check(FindMovieRule("MiniMap").anchors == (kAnchorRight | kAnchorBottom), "minimap bottom right");
    Check(!FindMovieRule("Menu/Menu_WaitingPlayer").known && !FindMovieRule("").known, "unknown movies stay in the box");
    Check(FindMovieRule("Menu\\Menu_FightMode").anchors == kAnchorLeft &&
              FindMovieRule("Menu\\Menu_PlayerSelection").anchors == kAnchorLeft,
          "the menus of the main menu's panel hang from the left edge");
    Check(FindMovieRule("Timer").group == Group::kFight && FindMovieRule("Timer").anchors == kAnchorNone,
          "the fight's timer marks a fight and stays centered");
    const MovieRule world = FindMovieRule("MiniMap"), fight = FindMovieRule("Equipment_WeaponsP1");
    Check(EffectiveAnchors(world, HudPlacement::kEdges, HudPlacement::kCenter, false) == (kAnchorRight | kAnchorBottom),
          "world HUD at the edges by default");
    Check(EffectiveAnchors(world, HudPlacement::kCenter, HudPlacement::kCenter, false) == kAnchorBottom,
          "world HUD centered keeps the vertical anchor");
    Check(EffectiveAnchors(fight, HudPlacement::kEdges, HudPlacement::kCenter, true) == kAnchorBottom,
          "fight HUD centered by default");
    Check(EffectiveAnchors(fight, HudPlacement::kEdges, HudPlacement::kEdges, true) == (kAnchorLeft | kAnchorBottom),
          "fight HUD at the edges by option");
    r = MovieRect(wide, full, kAnchorExtend);
    Check(Near(r.x, 0) && Near(r.w, 1) && Near(r.y, 0) && Near(r.h, 1), "extend fills the width");

    // Root pieces of the menus (stage twips, 20 a pixel).
    const MovieRule pause = FindMovieRule("Menu\\Menu_Pause");
    Check(pause.known && ClipAnchors(pause, 14) == kAnchorLeft && ClipAnchors(pause, 33) == kAnchorRight &&
              ClipAnchors(pause, 34) == kAnchorLeft && ClipAnchors(pause, 15) == kAnchorNone,
          "pause borders and list on the left, its panel in the box");
    Check(ClipAnchors(FindMovieRule("Menu\\Menu_InGameBackground"), 1) == kAnchorFill, "pause fill fills");
    const MovieRule save = FindMovieRule("Menu\\Menu_SaveGame");
    Check(ClipAnchors(save, 22) == kAnchorLeft && ClipAnchors(save, 19) == kAnchorNone,
          "the save slots keep their distance to the left border; the thumbnail panel stays");
    Check(ClipAnchors(FindMovieRule("Menu\\MainMenu_Background"), 7) == kAnchorCover,
          "the scrolling clouds of the main menu scale with the sky");
    Check(FindMovieRule("8Trigrams64Palms_Intro").framed && FindMovieRule("HUD_Fight_Start_PLVs").framed &&
              FindMovieRule("Sexy_Fight").framed && !FindMovieRule("8Trigrams64Palms_Button_Press").framed &&
              !FindMovieRule("NinjutsuReplication").framed && !FindMovieRule("MiniMap").framed,
          "the 16:9 pictures of the versus screen and the jutsu are framed, their small prompts are not");
    Check(FindMovieRule("HUD_Fight_Start_PLVs").group == Group::kFight &&
              FindMovieRule("HUD_Fight_Start_Naruto_Neji").group == Group::kFight &&
              !FindMovieRule("HUD_Fight_Start_PLVs").clip_count, "every versus screen marks a fight, framed whole");
    Check(FindMovieRule("HUD53").id && FindMovieRule("Flash\\hud53.swf").id == FindMovieRule("HUD53").id &&
              FindMovieRule("ControlPanel").id != FindMovieRule("HUD53").id && !FindMovieRule("OldHUD53").id,
          "a catalog entry has one id under any spelling, none outside the catalog");
    Check(FindMovieRule("rage\\RageBlackOverlay").anchors == kAnchorFill, "the rage overlay fills the screen");
    const MovieRule fight_pause = FindMovieRule("Menu\\Menu_FightPause");
    Check(fight_pause.group == Group::kFight && ClipAnchors(fight_pause, 1) == kAnchorFill,
          "the fight's pause shades the whole screen and still marks a fight");
    Check(ClipAnchors(FindMovieRule("Menu\\MoveList"), 1) == kAnchorFill, "the move list shades the whole screen");

    // Deeper pieces: the longest path wins, and a rule covers what is under it.
    const MovieRule dialog = FindMovieRule("PLV_Text_System");
    const uint16_t band[] = {1, 1, 1}, strip_leaf[] = {1, 1, 2, 1, 1}, text[] = {1, 1, 12, 10, 1, 1};
    const ClipRule* clip = FindPiece(dialog, band, 3);
    Check(clip && clip->anchors == kAnchorExtend && clip->repeat_side == Repeat::kNone, "the dialog band extends");
    clip = FindPiece(dialog, strip_leaf, 5);
    Check(clip && clip->repeat == 2288 && clip->repeat_side == Repeat::kBoth && !clip->anchors,
          "the strip of spirals repeats whole bitmaps, unstretched");
    Check(!FindPiece(dialog, text, 6) && !FindPiece(dialog, band, 2), "the text and the dialog's root have no rule");
    Check(ClipAnchors(dialog, 1) == kAnchorNone, "a deeper rule is not a root rule");
    const uint16_t hud_band[] = {1, 1, 1, 3};
    clip = FindPiece(FindMovieRule("HUD53"), hud_band, 4);
    Check(clip && clip->repeat_side == Repeat::kBefore, "the player's bar repeats toward its outer side");
    const ClipRule nested[] = {Piece({4}, kAnchorLeft), Piece({4, 2}, kAnchorRight)};
    const MovieRule two{kAnchorNone, Group::kNone, true, nested, 2};
    const uint16_t deep[] = {4, 2, 7}, other[] = {4, 3};
    Check(FindPiece(two, deep, 3) == &nested[1] && FindPiece(two, other, 2) == &nested[0], "longest path first");
    Check(RepeatCopies(200, 611) == 2 && RepeatCopies(640, 611) == 3 && RepeatCopies(0, 611) == 0 &&
              RepeatCopies(200, 0) == 0, "copies reach the screen's edge plus one, none at 16:9");
    const Matrix2D identity{};
    Matrix2D m = AnchorPiece(wide, identity, kAnchorLeft);
    Check(Near(m.tx, -200 * 20) && Near(m.a, 1), "a left piece moves 200 px at 21:9 without scale");
    m = AnchorPiece(wide, identity, kAnchorRight);
    Check(Near(m.tx, 200 * 20), "a right piece moves 200 px");
    m = AnchorPiece(wide, identity, kAnchorExtend);
    Check(Near(m.a * 0 + m.tx, -200 * 20, 0.5f) && Near(m.a * 1280 * 20 + m.tx, 1480 * 20, 0.5f) && Near(m.d, 1),
          "an extended fill spans the screen, height kept");
    m = AnchorPiece(wide, identity, kAnchorCover);
    Check(Near(m.a, m.d) && Near(m.a, 1680.0f / 1280) && Near(m.tx, -200 * 20, 0.5f), "cover scales alike");
    m = AnchorPiece(tall_screen, identity, kAnchorFill);
    Check(Near(m.a, 1) && Near(m.d, 960.0f / 720) && Near(m.ty, -120 * 20, 0.5f), "fill covers a 4:3 screen");
    Check(MovieRect(wide, full, kAnchorFill) == full, "a filling movie covers the screen");
    Check(Near(AnchorPiece(kStandardSize, identity, kAnchorLeft | kAnchorExtend).tx, 0) &&
              Near(AnchorPiece(kStandardSize, identity, kAnchorCover).a, 1), "1280x720 leaves pieces alone");
    const MovieRule shared{kAnchorLeft, Group::kShared, true};
    Check(EffectiveAnchors(shared, HudPlacement::kEdges, HudPlacement::kCenter, true) == kAnchorNone &&
              EffectiveAnchors(shared, HudPlacement::kEdges, HudPlacement::kCenter, false) == kAnchorLeft,
          "a shared piece follows the fight option while a fight piece shows");
    Check(MirrorAnchors(kAnchorLeft | kAnchorTop) == (kAnchorRight | kAnchorTop) &&
              MirrorAnchors(kAnchorRight | kAnchorBottom) == (kAnchorLeft | kAnchorBottom) &&
              MirrorAnchors(kAnchorLeft | kAnchorRight) == (kAnchorLeft | kAnchorRight) &&
              MirrorAnchors(kAnchorCover) == kAnchorCover, "a mirrored copy trades left and right");
    const Size edges{1728, 720};
    Check(Near(MovieRect(edges, full, MirrorAnchors(kAnchorLeft | kAnchorTop)).x, 2 * BoxLeft(edges) / 1728.0f),
          "the opponent's copy of the shared HUD rests on the right edge");
  } catch (const std::exception& error) {
    std::cerr << "FAIL after " << checks << " checks: " << error.what() << "\n";
    return 1;
  }
  std::cout << "widescreen_test: " << checks << " checks passed\n";
  return 0;
}
