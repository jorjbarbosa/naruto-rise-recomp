#include "widescreen/catalog.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <iterator>
#include <string>

namespace widescreen {
namespace {
constexpr uint8_t kL = kAnchorLeft, kR = kAnchorRight;

// Pieces of the menus (the display lists of the movies of Naruto_clip.SRA, by
// depth from the root): the borders and title banners rest at the stage's edges,
// the plain fills and the sky cover the stage. The left column of a menu (its
// list, cursor and left button) keeps its distance to the left border, as on
// the 16:9 stage; the right column stays in the box.
constexpr ClipRule kPause[] = {
    Piece({1}, kL),  Piece({13}, kL), Piece({14}, kL), Piece({12}, kR), Piece({33}, kR),  // Banner, borders.
    Piece({34}, kL), Piece({62}, kL), Piece({60}, kL),                                     // List, (B) Back.
};
constexpr ClipRule kOptions[] = {
    Piece({7}, kL), Piece({18}, kL), Piece({19}, kL), Piece({17}, kR), Piece({20}, kR),  // Banner, borders.
    // The panels with their rows and the buttons under them: one block from
    // the left border, as wide as on the stage.
    Piece({1}, kL), Piece({21}, kL), Piece({62}, kL), Piece({55}, kL), Piece({79}, kL),
};
constexpr ClipRule kSaveGame[] = {
    Piece({1}, kL),  Piece({17}, kL), Piece({18}, kL), Piece({16}, kR), Piece({21}, kR),  // Banner, borders.
    Piece({11}, kL), Piece({22}, kL), Piece({48}, kL), Piece({82}, kL),  // Cursor, slots, device.
};
constexpr ClipRule kInGameBackground[] = {Piece({1}, kAnchorFill)};  // d1 plain fill; d2, the swirl, stays.
constexpr ClipRule kBlackStripes[] = {Piece({2}, kAnchorExtend | kAnchorTop), Piece({1}, kAnchorExtend | kAnchorBottom)};
// The sky (one bitmap), the overlay over the right half (d2) and the two
// strips of clouds that scroll across the stage (d4, d7: 2353 and 2438 pixel
// bitmaps whose ends pass outside the stage) scale alike, so the screen shows
// the part of them the stage shows.
constexpr ClipRule kMainMenuBackground[] = {
    Piece({1}, kAnchorCover), Piece({19}, kAnchorCover), Piece({2}, kAnchorCover),
    Piece({4}, kAnchorCover), Piece({7}, kAnchorCover),
    Piece({20}, kAnchorFill),  // Plain overlay.
    // The hill and the characters at the bottom right.
    Piece({10}, kR | kAnchorBottom), Piece({11}, kR | kAnchorBottom),
    Piece({12}, kR | kAnchorBottom), Piece({13}, kR | kAnchorBottom),
};

// The title screen: the same sky, clouds, overlays and bottom right art as the main menu.
constexpr ClipRule kPressStart[] = {
    Piece({1}, kAnchorCover), Piece({35}, kAnchorCover), Piece({2}, kAnchorCover),
    Piece({4}, kAnchorCover), Piece({7}, kAnchorCover),
    Piece({36}, kAnchorFill),
    Piece({10}, kR | kAnchorBottom), Piece({11}, kR | kAnchorBottom),
    Piece({12}, kR | kAnchorBottom), Piece({13}, kR | kAnchorBottom),
};

// Character selection: a radial gradient (d1) and a bitmap that turns around
// (d2) behind everything, the black banner of the names (d5, one plain
// shape), and each player's jutsu and stats panels, whose outer ends pass the
// stage's edges. The large picture of the character (mcCharacterPreview) is
// cut at the stage's right edge.
constexpr ClipRule kCharacterSelection[] = {
    Piece({1}, kAnchorCover), Piece({2}, kAnchorCover), Piece({5}, kAnchorExtend),
    Piece({28}, kL),  Piece({134}, kL), Piece({196}, kL),  // mcP1Jutsu, mcP1Stats, mcNameLeft.
    Piece({81}, kR),  Piece({163}, kR), Piece({198}, kR),  // mcP2Jutsu, mcP2Stats, mcNameRight.
    Piece({204}, kR),
};
// Arena selection: the same gradient and turning bitmap.
constexpr ClipRule kArenaSelection[] = {Piece({1}, kAnchorCover), Piece({2}, kAnchorCover)};


// The fight's pause and its move list: the plain shade over the stage (d1).
constexpr ClipRule kStageShade[] = {Piece({1}, kAnchorFill)};

// The dialog (one root, d1): the band (mcText d1, a vertical gradient) and
// the plain backing of the text (mcTexte d1) span the screen; the strip of
// spirals on its top edge (mcText d2: an 88 pixel bitmap repeated over 2325
// pixels, scrolling) and the bar at the bottom (d18: three 552 pixel tiles)
// repeat whole. The portraits, the names and the text stay in the box.
constexpr ClipRule kDialog[] = {
    Piece({1, 1, 1}, kAnchorExtend), Piece({1, 1, 12, 1}, kAnchorExtend),
    Piece({1, 1, 2}, kAnchorNone, 2288, Repeat::kBoth),  // 26 bitmaps.
    Piece({1, 18}, kAnchorNone, 1656, Repeat::kBoth),
};

// The player's bar (HUD > moving > blackhud > d3): one plain rectangle 611
// pixels long, from the stage's edge at 16:9. In the box it repeats outward
// (toward its own -x, the screen's edge on either side) to the screen's edge.
constexpr ClipRule kHud53[] = {Piece({1, 1, 1, 3}, kAnchorNone, 611, Repeat::kBefore)};

struct Entry {
  std::string_view name;
  uint8_t anchors;
  Group group;
  const ClipRule* clips = nullptr;
  size_t clip_count = 0;
  bool framed = false;
};
template <size_t N>
constexpr Entry Pieces(std::string_view name, const ClipRule (&clips)[N], uint8_t anchors = kAnchorNone,
                     Group group = Group::kNone, bool framed = false) {
  return {name, anchors, group, clips, N, framed};
}
// A 16:9 picture over the whole stage, in the box with a black frame (which
// covers the screen outside the box: pieces anchored there would not show).
constexpr Entry Framed(std::string_view name, Group group = Group::kNone) {
  return {name, kAnchorNone, group, nullptr, 0, true};
}

// Rest positions measured on the stage of each movie (the bounds of its art in
// its first frames): the art of these pieces rests in one corner or along one edge.
constexpr Entry kMovies[] = {
    // Open world.
    {"HUD_Naruto_Background", kAnchorLeft | kAnchorTop, Group::kWorld},    // Portrait, -12..571 x 36..211.
    {"HUD_Naruto_SpiritFiller", kAnchorLeft | kAnchorTop, Group::kWorld},  // 200..530 x 89..143.
    {"MiniMap", kAnchorRight | kAnchorBottom, Group::kWorld},              // Frame 987..1212 x 457..680.
    {"HUD_Radar", kAnchorBottom, Group::kWorld},                           // 567..716 x 584..682.
    {"HUD_Arrow", kAnchorTop, Group::kWorld},                              // 562..713 x 19..160.
    {"HUD_Bandit", kAnchorRight | kAnchorTop, Group::kWorld},              // 725..1308 x 39..214.
    {"HUD_Collectibles", kAnchorRight, Group::kWorld},                     // From 1028.
    {"HUD_Gameplay", kAnchorRight, Group::kWorld},                         // From 1162.
    {"JourneyJutsu/HUD_ConcentrationGauge", kAnchorRight, Group::kWorld},  // 829..1535.
    {"HUD_SystemicGameplay", kAnchorRight | kAnchorTop, Group::kWorld},    // Races: top right.
    // Both: the player's portrait, life and chakra (-2..610 x 58..147) show
    // in the open world and in fights.
    Pieces("HUD53", kHud53, kAnchorLeft | kAnchorTop, Group::kShared),
    // The context buttons (Hold, LB, X, Y and their icons) at the top right: 980..1195 x 40..150 at rest.
    {"ControlPanel", kAnchorRight | kAnchorTop, Group::kShared},
    // Fight.
    {"Equipment_WeaponsP1", kAnchorLeft | kAnchorBottom, Group::kFight},   // 24..354 x 536..686.
    {"Equipment_WeaponsP2", kAnchorRight | kAnchorBottom, Group::kFight},
    {"HUD_Wins", kAnchorTop, Group::kFight},                               // Both counters, centered.
    {"KOSystem/KO_Pad", kAnchorBottom, Group::kFight},                     // 476..803 x 520..702.
    {"Hit_Combos", kAnchorNone, Group::kFight},                            // Centered; marks a fight on screen.
    // Centered pieces that only fights draw: they mark a fight on screen
    // for the shared HUD (the round banners, the fight's timer and pause).
    {"Timer", kAnchorNone, Group::kFight},
    {"PLV_Round", kAnchorNone, Group::kFight},
    {"PLV_FIGHT", kAnchorNone, Group::kFight},
    // The versus screen before a fight: a 16:9 picture (its plain bands end at the box's edges).
    Framed("HUD_Fight_Start_PLVs", Group::kFight),
    Pieces("Menu/Menu_FightPause", kStageShade, kAnchorNone, Group::kFight),
    // Neither: vertical anchors only, and the backgrounds.
    {"Subtitles", kAnchorBottom, Group::kNone},
    Pieces("PLV_Text_System", kDialog, kAnchorBottom),                       // The dialog band and portrait.
    Pieces("Menu/MoveList", kStageShade, kAnchorNone),                       // The jutsu and combo lists of the pause.
    {"Techniques_timer", kAnchorBottom, Group::kNone},
    {"Menu/KonohaBackground", kAnchorCover, Group::kNone},                 // One bitmap, exactly 0..1280 x 0..720.
    // Full-screen transitions: the plain fades fill the width; the textured
    // wipes and the flash cover the screen at their own proportions.
    {"Black_Fade", kAnchorFill, Group::kNone},
    {"White_Fade", kAnchorFill, Group::kNone},
    {"FadeToBlack", kAnchorFill, Group::kNone},
    {"FadeInOut", kAnchorFill, Group::kNone},
    {"BlackScreen", kAnchorFill, Group::kNone},
    {"White_Flash", kAnchorCover, Group::kNone},
    {"Wipe_01", kAnchorCover, Group::kNone},
    {"Wipe_02", kAnchorCover, Group::kNone},
    {"Wipe_03", kAnchorCover, Group::kNone},
    {"Wipe_04", kAnchorCover, Group::kNone},
    {"Wipe_05", kAnchorCover, Group::kNone},
    {"Wipe_Full1SecDelay", kAnchorCover, Group::kNone},
    {"Wipe_In", kAnchorCover, Group::kNone},
    {"Wipe_Out", kAnchorCover, Group::kNone},
    // The jutsu sequences and their transitions: 2D pictures drawn for the
    // whole 16:9 stage (their rest bounds cover it), framed in
    // black. The small pieces of the same sequences (the button prompts, the
    // QTE marks, NinjutsuReplication) stay as they are.
    Framed("Gaara_Transition"), Framed("Haku_Transition"), Framed("Kakashi_Transition"),
    Framed("Kakashi_Transition2"), Framed("Kiba_Transition"), Framed("Orochimaru_Transition"),
    Framed("Zabuza_Transition"), Framed("ShadowRep_transition"), Framed("ShadowRep_transition2"),
    Framed("ShadowRep_intro"), Framed("Sharingan_K"), Framed("Sharingan_S"), Framed("death_forseeing"),
    Framed("Sexy_Fight"), Framed("Sexy_Beurk_Back"), Framed("Sexy_Beurk_Front"), Framed("Sexy_Insuf_Back"),
    Framed("Sexy_Suff_Back"), Framed("RandomFight_Back"), Framed("Chidori_intro"),
    Framed("8Trigrams64Palms_intro"), Framed("8Trigrams64Palms_into_Head"), Framed("8Trigrams64Palms_Flash_Over"),
    Framed("Fireball_intro"), Framed("Fireball_jump_Back"), Framed("Fireball_jump_Front"),
    Framed("Fireball_outro_Front"), Framed("Haku_outro_Front"), Framed("Kiba_01_Akamaru_Front"),
    Framed("Kiba_02_Kibas_Back"), Framed("Kiba_03_Kibas_Back"), Framed("Kiba_03_Kibas_Front"),
    Framed("Kiba_outro_Front"),
    // The versus screens of the story's fights (the same 106 frames as HUD_Fight_Start_PLVs).
    Framed("HUD_Fight_Start_Naruto_Neji", Group::kFight), Framed("HUD_Fight_Start_Naruto_Ninja", Group::kFight),
    // The black overlay of the rage mode: a plain fill over the stage.
    {"rage/RageBlackOverlay", kAnchorFill, Group::kNone},
    // Menus whose panel, banner and list hang from the left edge (the same
    // panel, -48..868 x -122..670, in all of them).
    {"Menu/Menu", kAnchorLeft, Group::kNone},
    {"Menu/Menu_SinglePlayer", kAnchorLeft, Group::kNone},
    {"Menu/Menu_FightMode", kAnchorLeft, Group::kNone},
    {"Menu/Menu_FightOptionMenu", kAnchorLeft, Group::kNone},
    {"Menu/Menu_PlayerSelection", kAnchorLeft, Group::kNone},
    // Menus adapted piece by piece.
    Pieces("Menu/Menu_Pause", kPause),
    Pieces("Menu/Menu_Option", kOptions),
    Pieces("Menu/Menu_SaveGame", kSaveGame),
    Pieces("Menu/Menu_InGameBackground", kInGameBackground),
    Pieces("ScriptedEvent/BlackStripes", kBlackStripes),  // The cutscene bars: plain black, top and bottom.
    Pieces("Menu/MainMenu_Background", kMainMenuBackground),
    Pieces("Menu/PressStart", kPressStart),
    Pieces("Menu/Menu_CharacterSelection", kCharacterSelection),
    Pieces("Menu/Menu_ArenaSelection", kArenaSelection),
};

std::string Normalize(std::string_view name) {
  std::string out;
  out.reserve(name.size());
  for (char c : name) out += c == '\\' ? '/' : char(std::tolower(static_cast<unsigned char>(c)));
  const size_t slash = out.rfind('/'), dot = out.rfind('.');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) out.resize(dot);
  return out;
}

bool EndsWithComponents(std::string_view name, std::string_view key) {
  if (name.size() < key.size() || name.substr(name.size() - key.size()) != key) return false;
  return name.size() == key.size() || name[name.size() - key.size() - 1] == '/';
}

// The catalog's names normalized once, in the order of kMovies.
const std::array<std::string, std::size(kMovies)>& NormalizedNames() {
  static const auto names = [] {
    std::array<std::string, std::size(kMovies)> out;
    for (size_t i = 0; i < out.size(); ++i) out[i] = Normalize(kMovies[i].name);
    return out;
  }();
  return names;
}
}  // namespace

MovieRule FindMovieRule(std::string_view name) {
  if (name.empty()) return {};
  const std::string normalized = Normalize(name);
  const auto& names = NormalizedNames();
  for (size_t i = 0; i < names.size(); ++i)
    if (EndsWithComponents(normalized, names[i])) {
      const Entry& entry = kMovies[i];
      return {entry.anchors, entry.group, true, entry.clips, entry.clip_count, entry.framed};
    }
  return {};
}

const ClipRule* FindPiece(const MovieRule& rule, const uint16_t* path, size_t length) {
  const ClipRule* best = nullptr;
  for (size_t i = 0; i < rule.clip_count; ++i) {
    const ClipRule& clip = rule.clips[i];
    if (!clip.length || clip.length > length || (best && clip.length <= best->length)) continue;
    if (std::equal(clip.path.begin(), clip.path.begin() + clip.length, path)) best = &clip;
  }
  return best;
}

uint8_t ClipAnchors(const MovieRule& rule, int depth) {
  if (depth < 0) return kAnchorNone;
  const uint16_t path = uint16_t(depth);
  const ClipRule* clip = FindPiece(rule, &path, 1);
  return clip && clip->length == 1 ? clip->anchors : kAnchorNone;
}

int RepeatCopies(float box_left, float period) {
  if (!(period > 0.0f) || !(box_left > 0.0f)) return 0;
  return int(std::ceil(box_left / period)) + 1;
}

uint8_t EffectiveAnchors(const MovieRule& rule, HudPlacement world, HudPlacement fight, bool fight_on_screen) {
  HudPlacement placement = HudPlacement::kEdges;
  switch (rule.group) {
    case Group::kWorld: placement = world; break;
    case Group::kFight: placement = fight; break;
    case Group::kShared: placement = fight_on_screen ? fight : world; break;
    case Group::kNone: break;
  }
  uint8_t anchors = rule.anchors;
  if (placement == HudPlacement::kCenter) anchors &= uint8_t(~(kAnchorLeft | kAnchorRight));
  return anchors;
}
}  // namespace widescreen
