// Poll on the UI thread, where SDL is initialized and the SDK pumps events.
#include "menu/pad_input.h"
#include "controls/service.h"
#include <algorithm>
#include <cmath>
namespace narutorise::menu {
bool PadInput::Initialize() { initialized_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD); return initialized_; }
void PadInput::Shutdown() {
  for (auto* pad : pads_) SDL_CloseGamepad(pad);
  pads_.clear();
  if (initialized_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
  initialized_ = false;
}
void PadInput::Navigate(uint32_t buttons, std::chrono::steady_clock::time_point now,
                         const std::function<void(rex::ui::VirtualKey)>& key) {
  using rex::ui::VirtualKey;
  constexpr VirtualKey keys[] = {VirtualKey::kUp, VirtualKey::kDown, VirtualKey::kLeft, VirtualKey::kRight,
                                 VirtualKey::kReturn, VirtualKey::kEscape, VirtualKey::kPrior, VirtualKey::kNext, VirtualKey::kDelete};
  for (size_t i = 0; i < 9; ++i) {
    const bool down = (buttons & (1u << i)) != 0, was = (previous_ & (1u << i)) != 0;
    if (down && (!was || (i < 4 && now >= repeat_[i]))) {
      key(keys[i]); repeat_[i] = now + std::chrono::milliseconds(was ? 100 : 350);
    }
  }
  previous_ = buttons;
}
void PadInput::Poll(bool takes_input, const std::function<void(rex::ui::VirtualKey)>& key) {
  if (!initialized_) return;
  SDL_PumpEvents();
  SDL_UpdateGamepads();
  int count = 0;
  auto* ids = SDL_GetGamepads(&count);
  std::erase_if(pads_, [](SDL_Gamepad* pad) {
    if (SDL_GamepadConnected(pad)) return false;
    SDL_CloseGamepad(pad); return true;
  });
  for (int i = 0; ids && i < count; ++i) {
    if (std::none_of(pads_.begin(), pads_.end(), [&](SDL_Gamepad* pad) { return SDL_GetGamepadID(pad) == ids[i]; }))
      if (auto* pad = SDL_OpenGamepad(ids[i])) pads_.push_back(pad);
  }
  SDL_free(ids);
  uint32_t buttons = 0;
  bool shortcut = false;
  for (auto* pad : pads_) {
    const auto down = [&](SDL_GamepadButton b) { return SDL_GetGamepadButton(pad, b); };
    shortcut |= chord_ == "L3+R3" ? down(SDL_GAMEPAD_BUTTON_LEFT_STICK) && down(SDL_GAMEPAD_BUTTON_RIGHT_STICK) :
                chord_ == "LB+RB+Start" ? down(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) && down(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) && down(SDL_GAMEPAD_BUTTON_START) :
                down(SDL_GAMEPAD_BUTTON_BACK) && down(SDL_GAMEPAD_BUTTON_START);
    const auto x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    buttons |= (down(SDL_GAMEPAD_BUTTON_DPAD_UP) || y < -16000) ? 1u : 0;
    buttons |= (down(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || y > 16000) ? 2u : 0;
    buttons |= (down(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || x < -16000) ? 4u : 0;
    buttons |= (down(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || x > 16000) ? 8u : 0;
    buttons |= down(SDL_GAMEPAD_BUTTON_SOUTH) ? 16u : 0;
    buttons |= down(SDL_GAMEPAD_BUTTON_EAST) ? 32u : 0;
    buttons |= down(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) ? 64u : 0;
    buttons |= down(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) ? 128u : 0;
    buttons |= down(SDL_GAMEPAD_BUTTON_WEST) ? 256u : 0;
  }
  Shortcut(shortcut);
  Navigate(takes_input ? buttons : 0, std::chrono::steady_clock::now(), key);
}
void PadInput::Shortcut(bool held) {
  const bool pressed = held && !held_;
  held_ = held;
  if (pressed && toggle_) toggle_();
}
void PadInput::PollDevices(const controls::Inventory& inventory, bool takes_input,
                           const std::function<void(rex::ui::VirtualKey)>& key, bool allow_sticks) {
  // Testing axes must not also move focus. Require neutral on leaving the
  // preview, so a stick held at the edge does not suddenly navigate again.
  if (!allow_sticks) sticks_need_release_ = true;
  else if (sticks_need_release_ && std::none_of(inventory.devices.begin(), inventory.devices.end(),
      [](const auto& d) { return !d.keyboard &&
          (std::abs(d.raw.axes[0]) > .35 || std::abs(d.raw.axes[1]) > .35); }))
    sticks_need_release_ = false;
  const bool use_sticks = allow_sticks && !sticks_need_release_;
  uint32_t buttons=0;bool shortcut=false;
  for(const auto& d:inventory.devices)if(!d.keyboard){
    const auto bits=d.raw.buttons;const auto& axes=d.raw.axes;
    const uint16_t chord=chord_=="L3+R3"?0xc0:chord_=="LB+RB+Start"?0x310:0x30;
    shortcut|=(bits&chord)==chord;
    buttons|=((bits&1)||(use_sticks&&axes[1]>.49))?1u:0;
    buttons|=((bits&2)||(use_sticks&&axes[1]<-.49))?2u:0;
    buttons|=((bits&4)||(use_sticks&&axes[0]<-.49))?4u:0;
    buttons|=((bits&8)||(use_sticks&&axes[0]>.49))?8u:0;
    buttons|=(bits&0x1000)?16u:0;buttons|=(bits&0x2000)?32u:0;
    buttons|=(bits&0x100)?64u:0;buttons|=(bits&0x200)?128u:0;buttons|=(bits&0x4000)?256u:0;
  }
  auto now=std::chrono::steady_clock::now();Shortcut(shortcut);Navigate(takes_input?buttons:0,now,key);
}
}  // namespace narutorise::menu
