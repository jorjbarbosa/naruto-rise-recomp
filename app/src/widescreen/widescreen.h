#pragma once

// The widescreen mode of the native renderer: the options, the window size
// that the automatic aspect follows, and the logical screen the game runs at.
// The game hooks (game_hooks.cpp, live.cpp) and the native units
// (gpu/d3d/resources.cpp, gpu/d3d/pass.cpp) read it; at 1280 x 720 every one
// of them leaves the game as it ships.
#include <array>
#include <cstdint>
#include <optional>
#include <rex/ppc/context.h>

#include "widescreen/layout.h"

namespace widescreen {
// Guest addresses the hooks share: the device owner (+16 the device, +504 ..
// +524 the game's targets, +528/+532 the copies that become S, +536 S) and
// the GDI (+244/+248 the camera's W/H, +260 its ratio, +280 its index).
inline constexpr uint32_t kDeviceOwner = 0x82C0F008;
inline constexpr uint32_t kGdi = 0x82C0EFA4;

// The options (narutorise.toml and the settings menu): aspect_ratio,
// hud_world, hud_fight. InitializeOptions, on the UI thread before the game
// runs, reads them and follows their changes (the cvars' change callbacks);
// the game threads read the copies, never the cvars' strings.
void InitializeOptions();
AspectMode ConfiguredAspect();
HudPlacement WorldHud();
HudPlacement FightHud();

// steady_clock milliseconds: the short holds of the hooks.
int64_t NowMs();

// The client size of the game window (the monitor's in fullscreen), from the
// UI thread. The automatic aspect follows it.
void SetWindowSize(uint32_t width, uint32_t height);
// True once the window kept its size for a moment: a drag of its edge asks
// for one change at its end, not one per size it passes.
bool WindowSettled();
// The logical screen the options, the window and the resolution scale ask
// for now.
Size Requested();
// The logical screen of the game's targets (the render side: surfaces,
// tiles, S, the thumbnail). Set when the game fills its presentation
// parameters (820E6D68) and when a change recreates the targets (live.cpp,
// step B). 1280 x 720 before the session.
Size Current();
// The logical screen of what the main thread builds (the camera, the Flash
// layout and the Bink quad): it moves to a new screen one step earlier
// (live.cpp, step A), so the list built for the new targets already has it.
Size Layout();
// True when the native renderer runs the game's targets (Enabled) or its
// layout (LayoutEnabled) at a screen other than 1280 x 720.
bool Enabled();
bool LayoutEnabled();
// The session started (820E6D68 ran with the native renderer).
bool SessionStarted();
void SetCurrent(Size s);
void SetLayout(Size s);
void StartSession(Size s);
// The game's read-only data of a screen: the 16/9 of the effect 8211C278
// (W/H) and the thumbnail entry of the pool fixed at 320 x 180. False when the
// heap refuses the write. At 1280 x 720 the effect gets 16/9 back.
bool PatchScreenData(uint8_t* base, Size s);
// The camera's screen in the GDI: W x H, Hor+ by the free ratio (index 0,
// ratio H/W); 1280 x 720 puts back the game's own (index 3, 16:9, ratio 1.0
// as the GDI's constructor leaves it).
void WriteCamera(uint8_t* base, Size s);

// The game's display lists, numbered by the frame deliveries (820ED9D8, in
// the delivering thread's device lock while the render thread waits): after
// delivery N the render thread draws list N (DrawnList) while the main thread
// builds list N + 1 (BuildingList).
void NoteDelivery();
uint64_t DrawnList();
uint64_t BuildingList();

// Native units. Before the original of CreateTexture (8212D210) and of
// CreateRenderTarget / CreateDepthStencilSurface (8212D330): the sizes of the
// game's own targets for the logical screen, chosen by the call site (LR).
void AdjustTextureCreate(PPCContext& ctx, uint8_t* base);
void AdjustSurfaceCreate(PPCContext& ctx, uint8_t* base);
// The tile rectangles of the main pass (BeginTiling) for the logical screen,
// or none at 1280 x 720 (the game's table then).
std::optional<std::array<std::array<uint32_t, 4>, 3>> TilingRects();
}  // namespace widescreen
