// narutorise - mouse camera, in a script module (built into each AI2C DLL:
// the base game's and the three DLCs').
//
// The exploration camera is scripted. One script (sub_8822B590 in the base
// module) reads the right stick into the instance (+1548 x, +1552 y) and moves
// the vertical angle (+196) and the height offset (+1532) in fixed steps. The
// orbit routine (sub_882227F8) then turns the camera: past 0.25 of stick it
// rotates its input vector, the camera's offset from its target, around the
// vertical axis by an angle that grows with the stick and the frame time, and
// places the camera at target + offset. A mouse standing in for the stick can
// therefore only reach the stick's speeds.
//
// This hook runs at the entry of the orbit routine: it rotates the offset by
// the mouse's horizontal motion and moves the vertical angle and the height by
// its vertical motion, at a fixed angle per pixel. Meanwhile the keyboard and
// mouse driver leaves the mouse out of the right stick (NoteMouseCameraHook),
// so the controller's stick keeps the game's own behavior.
//
// The instance's fields (the same in every module):
//   +52   invert the horizontal turn      +188  invert the vertical turn
//   +196  vertical angle (rad), clamped to [0, +860]
//   +1532 height offset, clamped to [-0.75, 0.95]; the stick moves it 2.5 times
//         the vertical angle's step, the other way
//   +1752 slow mode: the script scales the stick by 0.25
//
// NARUTORISE_CAMERA_ORBIT names the module's orbit routine (CMakeLists.txt).
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>

#include "narutorise_pch.h"

#define NARUTORISE_CAT_(a, b) a##b
#define NARUTORISE_CAT(a, b) NARUTORISE_CAT_(a, b)
#define NARUTORISE_CAMERA_ORBIT_ORIGINAL NARUTORISE_CAT(__imp__, NARUTORISE_CAMERA_ORBIT)

extern "C" void NARUTORISE_CAMERA_ORBIT_ORIGINAL(PPCContext& __restrict, uint8_t*);

namespace {

// Camera angle per mouse pixel at 100% sensitivity: one turn every ~2100 px.
constexpr float kRadiansPerPixel = 0.003f;
constexpr float kHeightPerPitch = 2.5f;

// The game's camera follows its target with a lag, and when the lag grows too
// large it snaps back into place: turned faster than about 0.25 rad per logic
// tick, the camera jumps and Naruto seems to teleport on screen (measured with
// the motion record: none at 0.20, a jump every few ticks at 0.30 and 0.40).
// The turn per tick stays at 0.20 rad, about 340 degrees a second, and the
// rest of the motion carries over to the next ticks.
constexpr float kMaxYawPerTick = 0.20f;
constexpr float kMaxPitchPerTick = 0.10f;
// The pending turn is dropped after a pause in the hook (a menu, a fight).
constexpr int64_t kPendingResetMs = 250;

// Only the game's logic thread runs the orbit routine.
struct Pending {
  float yaw = 0.0f;
  float pitch = 0.0f;
  int64_t last_ms = 0;
};
Pending g_pending;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The game's own options (sub_82551AF0 in the executable): input off, invert
// X, invert Y.
constexpr uint32_t kInputDisabled = 0x82C0F0D8;
constexpr uint32_t kInvertX = 0x82C0F0DC;
constexpr uint32_t kInvertY = 0x82C0F0E0;

float LoadFloat(uint8_t* base, uint32_t address) {
  return std::bit_cast<float>(static_cast<uint32_t>(REX_LOAD_U32(address)));
}

void StoreFloat(uint8_t* base, uint32_t address, float value) {
  REX_STORE_U32(address, std::bit_cast<uint32_t>(value));
}

void TurnCamera(uint8_t* base, uint32_t instance, uint32_t offset) {
  rex::input::mnk::NoteMouseCameraHook();
  float dx = 0.0f;
  float dy = 0.0f;
  rex::input::mnk::TakeMouseCameraDelta(dx, dy);
  const int64_t now = NowMs();
  if (now - g_pending.last_ms > kPendingResetMs) g_pending = {};
  g_pending.last_ms = now;
  if (REX_LOAD_U32(kInputDisabled) != 0) {
    g_pending = {};
    g_pending.last_ms = now;
    return;
  }
  if (dx == 0.0f && dy == 0.0f && g_pending.yaw == 0.0f && g_pending.pitch == 0.0f) return;

  float scale = kRadiansPerPixel * static_cast<float>(rex::input::mnk::MouseCameraSensitivity());
  if (REX_LOAD_U32(instance + 1752) != 0) scale *= 0.25f;  // the script's slow mode
  // As the stick: x > 0 is right, y > 0 is up (the mouse moving up).
  float x = dx;
  float y = -dy;
  if (REX_LOAD_U32(kInvertX) != 0) x = -x;
  if (REX_LOAD_U32(kInvertY) != 0) y = -y;
  if (REX_LOAD_U32(instance + 52) != 0) x = -x;
  if (REX_LOAD_U32(instance + 188) != 0) y = -y;

  // The stick turns the offset by a negative angle when pushed right, and
  // lowers the vertical angle when pushed up.
  g_pending.yaw += -x * scale;
  g_pending.pitch += y * scale;
  const float angle = std::clamp(g_pending.yaw, -kMaxYawPerTick, kMaxYawPerTick);
  const float step = std::clamp(g_pending.pitch, -kMaxPitchPerTick, kMaxPitchPerTick);
  g_pending.yaw -= angle;
  g_pending.pitch -= step;

  const float ox = LoadFloat(base, offset);
  const float oy = LoadFloat(base, offset + 4);
  const float pitch_before = LoadFloat(base, instance + 196);
  if (angle != 0.0f) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    StoreFloat(base, offset, ox * c - oy * s);
    StoreFloat(base, offset + 4, ox * s + oy * c);
  }
  if (step != 0.0f) {
    const float pitch_max = LoadFloat(base, instance + 860);
    const float pitch = pitch_before - step;
    StoreFloat(base, instance + 196, std::fmin(std::fmax(pitch, 0.0f), pitch_max));
    // At a limit, the motion past it is dropped, so turning back is immediate.
    if (pitch < 0.0f || pitch > pitch_max) g_pending.pitch = 0.0f;
    const float height = LoadFloat(base, instance + 1532) + kHeightPerPitch * step;
    StoreFloat(base, instance + 1532, std::fmin(std::fmax(height, -0.75f), 0.95f));
  }
  if (rex::cvar::Query<bool>("mouse_camera_trace")) {
    static int lines = 0;
    if (lines++ < 2000) {
      REXLOG_INFO("[mouse-camera] dx {} dy {} scale {} turn {} pending {} offset ({}, {}) -> ({}, {}) pitch {} -> {}",
                  dx, dy, scale, angle, g_pending.yaw, ox, oy, LoadFloat(base, offset), LoadFloat(base, offset + 4),
                  pitch_before, LoadFloat(base, instance + 196));
    }
  }
}

}  // namespace

// The orbit routine: r3 the script's instance, r6 the camera's offset (x, y, z).
REX_HOOK_RAW(NARUTORISE_CAMERA_ORBIT) {
  if (rex::cvar::Query<bool>("mouse_camera_direct")) {
    const uint32_t a = ctx.r3.u32 ? REX_LOAD_U32(ctx.r3.u32 + 32) : 0;
    const uint32_t b = a ? REX_LOAD_U32(a + 4) : 0;
    const uint32_t instance = b ? REX_LOAD_U32(b + 72) : 0;
    if (instance && ctx.r6.u32) TurnCamera(base, instance, ctx.r6.u32);
  }
  NARUTORISE_CAMERA_ORBIT_ORIGINAL(ctx, base);
}
