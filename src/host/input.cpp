// Gamepads and keyboard merged into the four XInput player slots.

#include "host/input.h"

#include "log.h"

#include <SDL3/SDL.h>

#include <bitset>
#include <mutex>

namespace host
{
    namespace
    {
        // X_INPUT_GAMEPAD_* button bits.
        constexpr uint16_t kDpadUp = 0x0001;
        constexpr uint16_t kDpadDown = 0x0002;
        constexpr uint16_t kDpadLeft = 0x0004;
        constexpr uint16_t kDpadRight = 0x0008;
        constexpr uint16_t kStart = 0x0010;
        constexpr uint16_t kBack = 0x0020;
        constexpr uint16_t kLeftThumb = 0x0040;
        constexpr uint16_t kRightThumb = 0x0080;
        constexpr uint16_t kLeftShoulder = 0x0100;
        constexpr uint16_t kRightShoulder = 0x0200;
        constexpr uint16_t kA = 0x1000;
        constexpr uint16_t kB = 0x2000;
        constexpr uint16_t kX = 0x4000;
        constexpr uint16_t kY = 0x8000;

        // SDL stops a rumble after at most this long; the game sends a new state whenever it changes
        // the motors, so only a rumble held longer than this ends early.
        constexpr uint32_t kRumbleDurationMs = 0xFFFF;

        struct ButtonBinding
        {
            SDL_GamepadButton button;
            uint16_t bit;
        };

        // SDL names the face buttons by position, which matches the Xbox layout.
        constexpr ButtonBinding kGamepadButtons[] = {
            { SDL_GAMEPAD_BUTTON_DPAD_UP, kDpadUp },
            { SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDpadDown },
            { SDL_GAMEPAD_BUTTON_DPAD_LEFT, kDpadLeft },
            { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kDpadRight },
            { SDL_GAMEPAD_BUTTON_START, kStart },
            { SDL_GAMEPAD_BUTTON_BACK, kBack },
            { SDL_GAMEPAD_BUTTON_LEFT_STICK, kLeftThumb },
            { SDL_GAMEPAD_BUTTON_RIGHT_STICK, kRightThumb },
            { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kLeftShoulder },
            { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kRightShoulder },
            { SDL_GAMEPAD_BUTTON_SOUTH, kA },
            { SDL_GAMEPAD_BUTTON_EAST, kB },
            { SDL_GAMEPAD_BUTTON_WEST, kX },
            { SDL_GAMEPAD_BUTTON_NORTH, kY },
        };

        struct KeyBinding
        {
            SDL_Scancode key;
            uint16_t bit;
        };

        // Default keyboard map. Arrows drive the D-pad and WASD the left stick, so the menus work
        // whichever of the two they read.
        constexpr KeyBinding kKeyButtons[] = {
            { SDL_SCANCODE_UP, kDpadUp },
            { SDL_SCANCODE_DOWN, kDpadDown },
            { SDL_SCANCODE_LEFT, kDpadLeft },
            { SDL_SCANCODE_RIGHT, kDpadRight },
            { SDL_SCANCODE_RETURN, kStart },
            { SDL_SCANCODE_BACKSPACE, kBack },
            { SDL_SCANCODE_TAB, kLeftThumb },
            { SDL_SCANCODE_O, kRightThumb },
            { SDL_SCANCODE_Q, kLeftShoulder },
            { SDL_SCANCODE_E, kRightShoulder },
            { SDL_SCANCODE_SPACE, kA },
            { SDL_SCANCODE_LSHIFT, kB },
            { SDL_SCANCODE_F, kX },
            { SDL_SCANCODE_R, kY },
        };
        constexpr SDL_Scancode kKeyLeftTrigger = SDL_SCANCODE_Z;
        constexpr SDL_Scancode kKeyRightTrigger = SDL_SCANCODE_C;

        struct StickKeys
        {
            SDL_Scancode left, right, down, up;
        };
        constexpr StickKeys kKeyLeftStick = { SDL_SCANCODE_A, SDL_SCANCODE_D, SDL_SCANCODE_S, SDL_SCANCODE_W };
        constexpr StickKeys kKeyRightStick = { SDL_SCANCODE_J, SDL_SCANCODE_L, SDL_SCANCODE_K, SDL_SCANCODE_I };

        struct Slot
        {
            SDL_Gamepad* gamepad = nullptr;
            SDL_JoystickID id = 0;
            // Last state read from the gamepad (packet unused).
            PadState fromGamepad;
            // Gamepad and keyboard merged, as the game sees it.
            PadState state;
            uint16_t rumbleLeft = 0;
            uint16_t rumbleRight = 0;
        };

        // Guards everything below. Lock order: g_mutex, then SDL's joystick lock (taken inside the
        // SDL gamepad calls); SDL never calls back into this file while holding its lock.
        std::mutex g_mutex;
        Slot g_slots[kInputSlots];
        std::bitset<SDL_SCANCODE_COUNT> g_keys;
        // Starts false: SDL reports the focus the window gets when it is created.
        bool g_focused = false;
        // Set by StopInput: the process is about to exit, so the motors stay off.
        bool g_stopped = false;

        bool SameInput(const PadState& a, const PadState& b)
        {
            return a.buttons == b.buttons && a.leftTrigger == b.leftTrigger && a.rightTrigger == b.rightTrigger &&
                   a.thumbLX == b.thumbLX && a.thumbLY == b.thumbLY && a.thumbRX == b.thumbRX && a.thumbRY == b.thumbRY;
        }

        // SDL's Y axes grow downwards. -32768 has no positive counterpart, so it becomes 32767.
        int16_t FlipAxis(int16_t v)
        {
            return v == -32768 ? int16_t(32767) : int16_t(-v);
        }

        PadState ReadGamepad(SDL_Gamepad* gamepad)
        {
            PadState pad;
            for (const ButtonBinding& binding : kGamepadButtons)
            {
                if (SDL_GetGamepadButton(gamepad, binding.button))
                    pad.buttons |= binding.bit;
            }
            // Triggers range over 0-32767.
            pad.leftTrigger = uint8_t(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7);
            pad.rightTrigger = uint8_t(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7);
            pad.thumbLX = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
            pad.thumbLY = FlipAxis(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY));
            pad.thumbRX = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
            pad.thumbRY = FlipAxis(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY));
            return pad;
        }

        // Full deflection towards the held key; opposite keys cancel out.
        int16_t KeyAxis(SDL_Scancode negative, SDL_Scancode positive)
        {
            return int16_t((g_keys[positive] ? 32767 : 0) - (g_keys[negative] ? 32767 : 0));
        }

        // Overlays the keyboard on a pad state: buttons add up, a held key wins over the analog value.
        void ApplyKeyboard(PadState& pad)
        {
            for (const KeyBinding& binding : kKeyButtons)
            {
                if (g_keys[binding.key])
                    pad.buttons |= binding.bit;
            }
            if (g_keys[kKeyLeftTrigger])
                pad.leftTrigger = 255;
            if (g_keys[kKeyRightTrigger])
                pad.rightTrigger = 255;
            if (int16_t x = KeyAxis(kKeyLeftStick.left, kKeyLeftStick.right))
                pad.thumbLX = x;
            if (int16_t y = KeyAxis(kKeyLeftStick.down, kKeyLeftStick.up))
                pad.thumbLY = y;
            if (int16_t x = KeyAxis(kKeyRightStick.left, kKeyRightStick.right))
                pad.thumbRX = x;
            if (int16_t y = KeyAxis(kKeyRightStick.down, kKeyRightStick.up))
                pad.thumbRY = y;
        }

        // Recomputes what the game sees for a slot and starts a new packet if it changed.
        void UpdateSlot(int index)
        {
            Slot& slot = g_slots[index];
            PadState merged = slot.fromGamepad;
            if (index == 0)
                ApplyKeyboard(merged);
            if (SameInput(merged, slot.state))
                return;
            merged.packet = slot.state.packet + 1;
            slot.state = merged;
        }

        void ApplyRumble(const Slot& slot)
        {
            if (slot.gamepad == nullptr)
                return;
            if (g_focused && !g_stopped)
                SDL_RumbleGamepad(slot.gamepad, slot.rumbleLeft, slot.rumbleRight, kRumbleDurationMs);
            else
                SDL_RumbleGamepad(slot.gamepad, 0, 0, 0);
        }

        int FindSlot(SDL_JoystickID id)
        {
            for (int i = 0; i < kInputSlots; i++)
            {
                if (g_slots[i].gamepad != nullptr && g_slots[i].id == id)
                    return i;
            }
            return -1;
        }

        const char* GamepadName(SDL_Gamepad* gamepad)
        {
            const char* name = SDL_GetGamepadName(gamepad);
            return name != nullptr ? name : "unnamed gamepad";
        }

        void AddGamepad(SDL_JoystickID id)
        {
            if (FindSlot(id) >= 0)
                return;
            int index = 0;
            while (index < kInputSlots && g_slots[index].gamepad != nullptr)
                index++;
            if (index == kInputSlots)
            {
                Log("input: gamepad %u ignored, all %d slots are taken", id, kInputSlots);
                return;
            }
            SDL_Gamepad* gamepad = SDL_OpenGamepad(id);
            if (gamepad == nullptr)
            {
                Log("error: SDL_OpenGamepad failed: %s", SDL_GetError());
                return;
            }
            Slot& slot = g_slots[index];
            slot.gamepad = gamepad;
            slot.id = id;
            slot.fromGamepad = ReadGamepad(gamepad);
            UpdateSlot(index);
            // Slot 0 stays connected without a gamepad, so the game may have set its motors already.
            ApplyRumble(slot);
            Log("input: %s connected to slot %d", GamepadName(gamepad), index);
        }

        // Gives a freed slot to a gamepad that was turned away while all the slots were taken.
        void AssignWaitingGamepads()
        {
            int count = 0;
            SDL_JoystickID* ids = SDL_GetGamepads(&count);
            if (ids == nullptr)
                return;
            for (int i = 0; i < count; i++)
                AddGamepad(ids[i]);
            SDL_free(ids);
        }

        void RemoveGamepad(SDL_JoystickID id)
        {
            int index = FindSlot(id);
            if (index < 0)
                return;
            Slot& slot = g_slots[index];
            Log("input: %s disconnected from slot %d", GamepadName(slot.gamepad), index);
            SDL_CloseGamepad(slot.gamepad);
            slot.gamepad = nullptr;
            slot.id = 0;
            slot.fromGamepad = {};
            // The game sees slots 1-3 disconnect and sets their motors again on reconnection; slot 0
            // stays connected through the keyboard and keeps them for the next gamepad.
            if (index != 0)
            {
                slot.rumbleLeft = 0;
                slot.rumbleRight = 0;
            }
            UpdateSlot(index);
            AssignWaitingGamepads();
        }

        void RefreshGamepad(SDL_JoystickID id)
        {
            int index = FindSlot(id);
            if (index < 0)
                return;
            g_slots[index].fromGamepad = ReadGamepad(g_slots[index].gamepad);
            UpdateSlot(index);
        }

        // Keys held when the focus leaves never get their key-up; gamepads report no events while
        // unfocused, so their state is read again on return.
        void SetFocus(bool focused)
        {
            g_focused = focused;
            g_keys.reset();
            for (int i = 0; i < kInputSlots; i++)
            {
                Slot& slot = g_slots[i];
                if (focused && slot.gamepad != nullptr)
                    slot.fromGamepad = ReadGamepad(slot.gamepad);
                UpdateSlot(i);
                ApplyRumble(slot);
            }
        }
    }

    void HandleInputEvent(const SDL_Event& event)
    {
        std::lock_guard lock(g_mutex);
        switch (event.type)
        {
        case SDL_EVENT_GAMEPAD_ADDED:
            AddGamepad(event.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            RemoveGamepad(event.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            RefreshGamepad(event.gaxis.which);
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            RefreshGamepad(event.gbutton.which);
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            if (event.key.scancode < SDL_SCANCODE_COUNT)
            {
                g_keys[event.key.scancode] = event.type == SDL_EVENT_KEY_DOWN;
                UpdateSlot(0);
            }
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            SetFocus(true);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            SetFocus(false);
            break;
        default:
            break;
        }
    }

    bool GetPadState(int slot, PadState& out)
    {
        if (slot < 0 || slot >= kInputSlots)
            return false;
        std::lock_guard lock(g_mutex);
        if (slot != 0 && g_slots[slot].gamepad == nullptr)
            return false;
        out = g_slots[slot].state;
        return true;
    }

    bool SetPadRumble(int slot, uint16_t left, uint16_t right)
    {
        if (slot < 0 || slot >= kInputSlots)
            return false;
        std::lock_guard lock(g_mutex);
        Slot& target = g_slots[slot];
        if (slot != 0 && target.gamepad == nullptr)
            return false;
        target.rumbleLeft = left;
        target.rumbleRight = right;
        ApplyRumble(target);
        return true;
    }

    void StopInput()
    {
        std::lock_guard lock(g_mutex);
        g_stopped = true;
        for (const Slot& slot : g_slots)
            ApplyRumble(slot);
    }
}
