// narutorise - the game's Options screen, in a script module (built into each
// AI2C DLL: the base game's and the three DLCs').
//
// The menus' script runs the Options screen as a state (sub_8810B248 in the
// base module), called at every frame while it shows. Entering, it shows its
// movie (Menu\Menu_Option, menu 26) and the paper behind it
// (Menu\Menu_InGameBackground), and copies the game's options (the Option
// object). Left and right go through fn_Option_SetValue, which writes an
// option and applies it at once (the volumes, the vibration). A saves the
// options when they changed and leaves; B restores the copy and leaves.
//
// When the in-game settings stand in for the screen (game_options.h), this
// hook hides its movie as soon as the state shows it, as the script hides a
// menu it leaves (fn_HideMenu: the movie's layer, here), and reports the
// options. In the title menus the paper comes with the screen and goes after
// it, so it hides too; in a paused game the paper is the pause menu's and
// stays. At each frame it sets the options changed in the settings through
// fn_Option_SetValue before the state runs. Once the settings' out-animation
// ends, they press A for the state (the input gate), which saves and leaves as
// with its own screen.
//
// The next menu's state waits for the movie on screen to end its own
// out-animation before showing its movie; the hidden movie never would. The
// settings played it, so as the state leaves the hook finishes the hiding
// (the clip, as fn_HideMenu) and leaves no movie on screen.
//
// The menus' data (+72 of the script's current instance), the same in every
// module (the script's arrays keep their length first):
//   +0     the layer on screen          +20    its clip
//   +200   in the title menus           +2124  the Options layer (menu 26 of the layers at +2016)
//   +2984  the paper's layer            +3132  the menus' state
//
// fni_Option_GetValue (NARUTORISE_OPTION_GET) reads the options through an
// array it finds and keeps (Module::options); the hook reports it to the
// executable after each read, so the audio settings follow the game's options
// outside its screen too.
//
// NARUTORISE_OPTIONS_STATE names the module's state and NARUTORISE_SCRIPT_MODULE
// its row of kModules (CMakeLists.txt).
#include <cstdint>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <rex/hook.h>
#include <rex/logging.h>

#include "game_options.h"
#include "narutorise_pch.h"

#define NARUTORISE_CAT_(a, b) a##b
#define NARUTORISE_CAT(a, b) NARUTORISE_CAT_(a, b)
#define NARUTORISE_OPTIONS_STATE_ORIGINAL NARUTORISE_CAT(__imp__, NARUTORISE_OPTIONS_STATE)
#define NARUTORISE_OPTION_GET_ORIGINAL NARUTORISE_CAT(__imp__, NARUTORISE_OPTION_GET)

extern "C" void NARUTORISE_OPTIONS_STATE_ORIGINAL(PPCContext& __restrict, uint8_t*);
extern "C" void NARUTORISE_OPTION_GET_ORIGINAL(PPCContext& __restrict, uint8_t*);

namespace {

namespace options = narutorise::game_options;

// The executable's session, found by name once.
options::Session* Shared() {
  static options::Session* const session = [] {
    using Find = options::Session* (*)();
#if defined(_WIN32)
    const auto find = reinterpret_cast<Find>(GetProcAddress(GetModuleHandleW(nullptr), options::kSharedSymbol));
#else
    const auto find = reinterpret_cast<Find>(dlsym(RTLD_DEFAULT, options::kSharedSymbol));
#endif
    if (!find) REXLOG_WARN("[game-options] {} not found; the game's Options screen stays its own", options::kSharedSymbol);
    return find ? find() : nullptr;
  }();
  return session;
}

// What differs between the modules (found from the state's strings and the
// same code in each).
struct Module {
  uint32_t instance;  // Points to the script's current instance.
  uint32_t object;    // The Option object, once the state found it.
  uint32_t get;       // fni_Option_GetValue(object, &value, option)
  uint32_t set;       // fn_Option_SetValue(object, &changed, option, value)
  uint32_t hide;      // Holds the engine's function hiding a movie's layer.
  uint32_t retire;    // Holds the engine's function hiding a movie's clip.
  uint32_t voices;    // Holds the engine's function the state asks, with 1, before offering the Japanese voices.
  uint32_t options;   // fni_Option_GetValue's array of the options, once it read one.
};
constexpr Module kModules[] = {
    {0x883B2118, 0x883B4DBC, 0x88064500, 0x88061C20, 0x883B38F0, 0x883B3834, 0x883B3304, 0x883B3FD8},  // ai2c
    {0x883B2158, 0x883B4DFC, 0x880644B8, 0x88061BD8, 0x883B3930, 0x883B3874, 0x883B3344, 0x883B4018},  // ai2c1
    {0x883C21B8, 0x883C4E60, 0x88064558, 0x88061C78, 0x883C3990, 0x883C38D4, 0x883C33A4, 0x883C4078},  // ai2c2
    {0x883C21C8, 0x883C4E70, 0x88064558, 0x88061C78, 0x883C39A0, 0x883C38E4, 0x883C33B4, 0x883C4088},  // ai2c3
};
constexpr const Module& kModule = kModules[NARUTORISE_SCRIPT_MODULE];

constexpr uint32_t kShownLayer = 0, kShownClip = 20, kTitleMenus = 200, kOptionsLayer = 2124, kPaperLayer = 2984,
                   kState = 3132;
constexpr uint32_t kNone = 0xFFFFFFFF;

// A frame of the hook's own on the guest stack while it calls the script: the
// callees save into their caller's frame, and the out-arguments live in it.
class Frame {
 public:
  Frame(PPCContext& ctx, uint8_t* base) : ctx_(ctx), stack_(ctx.r1.u32) {
    ctx.r1.u32 = stack_ - kSize;
    REX_STORE_U32(ctx.r1.u32, stack_);  // The back chain.
  }
  ~Frame() { ctx_.r1.u32 = stack_; }
  Frame(const Frame&) = delete;
  Frame& operator=(const Frame&) = delete;
  // Past the callee's save area (+20 to +52 of the frame).
  uint32_t out() const { return stack_ - 16; }

 private:
  static constexpr uint32_t kSize = 128;
  PPCContext& ctx_;
  uint32_t stack_;
};

uint32_t Call(PPCContext& ctx, uint8_t* base, uint32_t function, uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0,
              uint32_t r6 = 0) {
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  ctx.r6.u64 = r6;
  REX_CALL_INDIRECT_FUNC(function);
  return ctx.r3.u32;
}

int GetOption(PPCContext& ctx, uint8_t* base, const Frame& frame, uint32_t object, int option) {
  REX_STORE_U32(frame.out(), 0);
  Call(ctx, base, kModule.get, object, frame.out(), uint32_t(option));
  return int(REX_LOAD_U32(frame.out()));
}

// The state has just shown its screen: the settings take its place.
void StandIn(PPCContext& ctx, uint8_t* base, uint32_t menus, options::Session& session) {
  const uint32_t object = REX_LOAD_U32(kModule.object);
  if (!object) return;
  Frame frame(ctx, base);
  Call(ctx, base, REX_LOAD_U32(kModule.hide), REX_LOAD_U32(menus + kShownLayer));
  const bool title_menus = REX_LOAD_U32(menus + kTitleMenus) != 0;
  if (const uint32_t paper = REX_LOAD_U32(menus + kPaperLayer); title_menus && paper && paper != kNone)
    Call(ctx, base, REX_LOAD_U32(kModule.hide), paper);
  options::Values values;
  for (int option = 0; option < options::kCount; ++option)
    values.option[size_t(option)] = GetOption(ctx, base, frame, object, option);
  values.japanese_voices = Call(ctx, base, REX_LOAD_U32(kModule.voices), 1) != 0 && title_menus;
  REXLOG_INFO("[game-options] Options screen hidden for the settings: music {} voice {} effects {} invert {}/{} "
              "vibration {} subtitles {} Japanese voices {} ({})",
              values.option[options::kMusicVolume], values.option[options::kVoiceVolume],
              values.option[options::kEffectsVolume], values.option[options::kInvertX],
              values.option[options::kInvertY], values.option[options::kVibration],
              values.option[options::kSubtitles], values.option[options::kJapaneseVoices],
              values.japanese_voices ? "offered" : "not offered");
  session.Opened(values);
}

}  // namespace

// The Options screen's state: no arguments; returns 1000 to keep running.
REX_HOOK_RAW(NARUTORISE_OPTIONS_STATE) {
  const uint32_t current = REX_LOAD_U32(kModule.instance);
  const uint32_t instance = current ? REX_LOAD_U32(current) : 0;
  const uint32_t menus = instance ? REX_LOAD_U32(instance + 72) : 0;
  options::Session* session = menus ? Shared() : nullptr;
  static bool reported = false;
  if (!std::exchange(reported, true))
    REXLOG_INFO("[game-options] Options screen state in module {}: menus {:08X}, settings {}", NARUTORISE_SCRIPT_MODULE,
                menus, !session ? "not found" : session->Enabled() ? "standing in" : "off");
  if (!session || !session->Enabled()) return NARUTORISE_OPTIONS_STATE_ORIGINAL(ctx, base);
  if (const uint32_t object = REX_LOAD_U32(kModule.object); object && session->Open()) {
    Frame frame(ctx, base);
    for (int option = 0; option < options::kCount; ++option) {
      const int value = session->TakeChange(options::Option(option));
      if (value == options::Session::kNoChange) continue;
      REX_STORE_U32(frame.out(), 0);
      Call(ctx, base, kModule.set, object, frame.out(), uint32_t(option), uint32_t(value));
    }
  }
  const uint32_t layer = REX_LOAD_U32(menus + kShownLayer), state = REX_LOAD_U32(menus + kState);
  NARUTORISE_OPTIONS_STATE_ORIGINAL(ctx, base);
  const uint64_t result = ctx.r3.u64;
  const uint32_t shown = REX_LOAD_U32(menus + kShownLayer);
  if (shown != layer && shown != kNone && shown == REX_LOAD_U32(menus + kOptionsLayer))
    StandIn(ctx, base, menus, *session);
  if (session->Open() && REX_LOAD_U32(menus + kState) != state) {
    Frame frame(ctx, base);
    Call(ctx, base, REX_LOAD_U32(kModule.retire), REX_LOAD_U32(menus + kShownClip));
    REX_STORE_U32(menus + kShownLayer, kNone);
    REX_STORE_U32(menus + kShownClip, kNone);
    session->Left();
    REXLOG_INFO("[game-options] Options screen left");
  }
  ctx.r3.u64 = result;
}

// fni_Option_GetValue(object, &value, option): reports the options' array.
REX_HOOK_RAW(NARUTORISE_OPTION_GET) {
  NARUTORISE_OPTION_GET_ORIGINAL(ctx, base);
  if (options::Session* session = Shared()) session->ReportOptions(REX_LOAD_U32(kModule.options));
}
