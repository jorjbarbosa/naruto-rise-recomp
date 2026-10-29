#pragma once

// The transforms between two logic ticks (state interpolation, tick.h).
//
// In the iterations the gate decides, the view block of the main step draws
// the objects and the camera between the last two ticks instead of the last
// one. At the start of the view block (82684050, the view of the main step),
// in an iteration with a tick, the matrix of each object the last display
// mounted (825E2A08: flag 0x40000 without 0x200004), [object + 20] (16
// floats, position at +48..+56), becomes the current transform, and the
// previous current one the previous; so does the camera, the matrix at the
// start of the view's entry (the 172-byte entries of world + 48, the same
// layout), at the display's camera (82659B88, which builds the view's camera
// from it). Then, in every iteration of the gate, the transform at the tick's
// alpha (tick.h) is written over the live one (the objects before the view
// block, the camera before 82659B88) and the live one comes back after the
// delivery (8259FF40), unless something wrote the place meanwhile (but for the
// turned matrices below). The display copies the matrices into the frame's
// packet (P2 of the study), so the render thread does not read them.
//
// An engine matrix has a type word at +64; with its scale flag the fourth
// column scales the rows. The blend preserves the column's values with or
// without the flag: the renderer also reads it when inverting the full matrix
// for lighting. When the flag changes between ticks, the matrix stays at the
// tick. The display turns some matrices in place (the billboards: the
// indicators over the characters), so the write does not come
// back; the logic of the next tick reads them (the indicators animate their
// scale from the matrix). Where the view block only turned a written matrix,
// the tick's scale and position go back under the turn, and a frame without
// a tick writes the interpolated value under it again.
// REX_NATIVE_INTERPOLATION_TURNED=0 leaves them as the view block left them
// (with the diagnostics, F7 switches it at run time).
//
// Rows of the rotation are interpolated linearly and keep the interpolated
// length; the position linearly. Cuts, without interpolation until the next
// tick: an object whose matrix moved more than kCutDistance or turned more
// than ~60 degrees in one tick, or whose matrix pointer changed; the camera
// when it moved 10 units or turned 30 degrees in one tick (the thresholds of
// 8267A2A0) or while the game's cut counter 82C0EB34 is set.
//
// The pose of the skinned meshes: the display (825802B0, the render of a
// visual object) copies, for each bone of a mesh, its matrix relative to the
// object into the frame's packet (82580968 -> [display state + 64] =
// 820E91E0, the bone array at packet + 2900); the render thread turns that
// array into the bone palette (82106BC8: the vertex constants, or the 1D
// half-float texture of 820F3868). The bone matrices are the global matrices
// of the bones' objects [bone + 20] (relative to [object + 20]) or matrices
// of the object's bone array, written by the logic in the tick. Around each
// copy (82580968), the matrices it reads go at the tick's alpha, tracked by
// address with the same cuts as the objects, and come back right after it.
//
// On with the gate of tick.h (frame_rate above 30, or
// REX_NATIVE_STATE_INTERPOLATION=1); REX_NATIVE_INTERPOLATION_TRANSFORMS=0
// leaves the frames between the ticks
// as the last tick (the gate alone); REX_NATIVE_INTERPOLATION_BONES=0 leaves
// the poses at the last tick. Main thread.
#include <cstdint>

namespace gpu::interpolation {
bool Enabled();
// Main thread, outside a view block, when the pacing mode changes.
void ResetHistory(uint8_t* base);
// The last view block: the matrices written (objects), a camera entry written
// and a camera cut at the tick, and the object cuts of the last tick; the
// poses copied on the main thread, the bone matrices written, cut at the tick
// and lost (written outside the logic), and the copies of another thread (not
// interpolated); the wait for the effects' update of the tick before the
// block's writes (it runs on its work thread), and whether it hit its bound.
struct Applied {
  uint32_t objects = 0, object_cuts = 0;
  bool camera = false, camera_cut = false;
  uint32_t poses = 0, bones = 0, bone_cuts = 0, bones_lost = 0, pose_other_thread = 0;
  uint32_t effects_wait_us = 0;
  bool effects_timeout = false;
};
const Applied& LastApplied();
// Mounts (825E2A08) seen from another thread than the view block's while
// collecting, since the start: not tracked.
uint32_t MountsOtherThread();
// The object of the pose copy running on this thread (82580968), or 0.
uint32_t PoseObject();
// A pointer the engine frees (820C0018, crash_dump.cpp), any thread: a freed
// object leaves the tracks before the next write.
void NoteFree(uint32_t pointer);
}  // namespace gpu::interpolation
