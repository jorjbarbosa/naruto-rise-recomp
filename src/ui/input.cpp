// SDL input for the host UI (input.h).

#include "ui/input.h"

#include "host/timing.h"

#include <SDL3/SDL.h>

#include <bit>

namespace ui
{
    namespace
    {
        // Stick thresholds, as fractions of full travel: on above the first, off below the second.
        constexpr float kStickOn = 0.5f;
        constexpr float kStickOff = 0.35f;
        // A held direction repeats after the first delay, then every second one (seconds).
        constexpr double kRepeatDelay = 0.4;
        constexpr double kRepeatPeriod = 0.1;

        uint32_t Bit(Action action)
        {
            return 1u << int(action);
        }

        // One axis of the stick with hysteresis: the direction it holds on that axis (0 for none).
        uint32_t StickAxis(float value, uint32_t held, Action negative, Action positive)
        {
            uint32_t out = 0;
            if (value >= kStickOn || (value > kStickOff && (held & Bit(positive)) != 0))
                out |= Bit(positive);
            if (value <= -kStickOn || (value < -kStickOff && (held & Bit(negative)) != 0))
                out |= Bit(negative);
            return out;
        }
    }

    bool KeyAction(int scancode, Action& action)
    {
        switch (scancode)
        {
        case SDL_SCANCODE_UP: action = Action::Up; return true;
        case SDL_SCANCODE_DOWN: action = Action::Down; return true;
        case SDL_SCANCODE_LEFT: action = Action::Left; return true;
        case SDL_SCANCODE_RIGHT: action = Action::Right; return true;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
        case SDL_SCANCODE_SPACE: action = Action::Accept; return true;
        case SDL_SCANCODE_ESCAPE:
        case SDL_SCANCODE_BACKSPACE: action = Action::Back; return true;
        case SDL_SCANCODE_Q:
        case SDL_SCANCODE_PAGEUP: action = Action::PreviousTab; return true;
        case SDL_SCANCODE_E:
        case SDL_SCANCODE_PAGEDOWN: action = Action::NextTab; return true;
        case SDL_SCANCODE_DELETE: action = Action::Reset; return true;
        default: return false;
        }
    }

    Rml::Input::KeyIdentifier KeyIdentifier(int scancode)
    {
        using namespace Rml::Input;
        if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z)
            return KeyIdentifier(KI_A + (scancode - SDL_SCANCODE_A));
        if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9)
            return KeyIdentifier(KI_1 + (scancode - SDL_SCANCODE_1));
        if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12)
            return KeyIdentifier(KI_F1 + (scancode - SDL_SCANCODE_F1));
        if (scancode >= SDL_SCANCODE_KP_1 && scancode <= SDL_SCANCODE_KP_9)
            return KeyIdentifier(KI_NUMPAD1 + (scancode - SDL_SCANCODE_KP_1));
        switch (scancode)
        {
        case SDL_SCANCODE_0: return KI_0;
        case SDL_SCANCODE_KP_0: return KI_NUMPAD0;
        case SDL_SCANCODE_RETURN: return KI_RETURN;
        case SDL_SCANCODE_KP_ENTER: return KI_NUMPADENTER;
        case SDL_SCANCODE_ESCAPE: return KI_ESCAPE;
        case SDL_SCANCODE_BACKSPACE: return KI_BACK;
        case SDL_SCANCODE_TAB: return KI_TAB;
        case SDL_SCANCODE_SPACE: return KI_SPACE;
        case SDL_SCANCODE_DELETE: return KI_DELETE;
        case SDL_SCANCODE_INSERT: return KI_INSERT;
        case SDL_SCANCODE_HOME: return KI_HOME;
        case SDL_SCANCODE_END: return KI_END;
        case SDL_SCANCODE_PAGEUP: return KI_PRIOR;
        case SDL_SCANCODE_PAGEDOWN: return KI_NEXT;
        case SDL_SCANCODE_LEFT: return KI_LEFT;
        case SDL_SCANCODE_RIGHT: return KI_RIGHT;
        case SDL_SCANCODE_UP: return KI_UP;
        case SDL_SCANCODE_DOWN: return KI_DOWN;
        case SDL_SCANCODE_LSHIFT: return KI_LSHIFT;
        case SDL_SCANCODE_RSHIFT: return KI_RSHIFT;
        case SDL_SCANCODE_LCTRL: return KI_LCONTROL;
        case SDL_SCANCODE_RCTRL: return KI_RCONTROL;
        case SDL_SCANCODE_LALT: return KI_LMENU;
        case SDL_SCANCODE_RALT: return KI_RMENU;
        default: return KI_UNKNOWN;
        }
    }

    int KeyModifiers(uint16_t sdl)
    {
        using namespace Rml::Input;
        int modifiers = 0;
        if (sdl & SDL_KMOD_CTRL)
            modifiers |= KM_CTRL;
        if (sdl & SDL_KMOD_SHIFT)
            modifiers |= KM_SHIFT;
        if (sdl & SDL_KMOD_ALT)
            modifiers |= KM_ALT;
        if (sdl & SDL_KMOD_GUI)
            modifiers |= KM_META;
        if (sdl & SDL_KMOD_CAPS)
            modifiers |= KM_CAPSLOCK;
        if (sdl & SDL_KMOD_NUM)
            modifiers |= KM_NUMLOCK;
        if (sdl & SDL_KMOD_SCROLL)
            modifiers |= KM_SCROLLLOCK;
        return modifiers;
    }

    bool IsDirection(Action action)
    {
        return action == Action::Up || action == Action::Down || action == Action::Left || action == Action::Right;
    }

    void PadActions::HandleEvent(const SDL_Event& event, int64_t now, std::vector<Action>& actions)
    {
        if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP)
        {
            bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
            Pad& pad = pads_[uint32_t(event.gbutton.which)];
            auto direction = [&](Action action) {
                pad.dpad = down ? (pad.dpad | Bit(action)) : (pad.dpad & ~Bit(action));
            };
            switch (event.gbutton.button)
            {
            case SDL_GAMEPAD_BUTTON_DPAD_UP: direction(Action::Up); break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN: direction(Action::Down); break;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT: direction(Action::Left); break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: direction(Action::Right); break;
            case SDL_GAMEPAD_BUTTON_SOUTH: if (down) actions.push_back(Action::Accept); break;
            case SDL_GAMEPAD_BUTTON_EAST: if (down) actions.push_back(Action::Back); break;
            case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: if (down) actions.push_back(Action::PreviousTab); break;
            case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: if (down) actions.push_back(Action::NextTab); break;
            case SDL_GAMEPAD_BUTTON_NORTH: if (down) actions.push_back(Action::Reset); break;
            default: break;
            }
            UpdateHeld(now, actions);
        }
        else if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION)
        {
            if (event.gaxis.axis != SDL_GAMEPAD_AXIS_LEFTX && event.gaxis.axis != SDL_GAMEPAD_AXIS_LEFTY)
                return;
            Pad& pad = pads_[uint32_t(event.gaxis.which)];
            float value = float(event.gaxis.value) / 32767.0f;
            if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX)
                pad.stickX = value;
            else
                pad.stickY = value; // SDL's Y grows downwards
            pad.stick = StickAxis(pad.stickX, pad.stick, Action::Left, Action::Right) |
                        StickAxis(pad.stickY, pad.stick, Action::Up, Action::Down);
            UpdateHeld(now, actions);
        }
        else if (event.type == SDL_EVENT_GAMEPAD_REMOVED)
        {
            pads_.erase(uint32_t(event.gdevice.which));
            UpdateHeld(now, actions);
        }
    }

    void PadActions::UpdateHeld(int64_t now, std::vector<Action>& actions)
    {
        uint32_t after = 0;
        for (const auto& [id, pad] : pads_)
            after |= pad.dpad | pad.stick;
        uint32_t pressed = after & ~held_;
        held_ = after;
        for (int action = int(Action::Up); action <= int(Action::Right); action++)
        {
            if ((pressed & (1u << action)) == 0)
                continue;
            actions.push_back(Action(action));
            repeating_ = action;
            nextRepeat_ = now + host::CounterTicks(kRepeatDelay);
        }
        if (repeating_ >= 0 && (after & (1u << repeating_)) == 0)
        {
            // Another held direction takes over the repeat, from its own delay.
            repeating_ = after != 0 ? std::countr_zero(after) : -1;
            nextRepeat_ = now + host::CounterTicks(kRepeatDelay);
        }
    }

    void PadActions::Tick(int64_t now, std::vector<Action>& actions)
    {
        if (repeating_ < 0 || now < nextRepeat_)
            return;
        actions.push_back(Action(repeating_));
        // A late tick repeats once, and the next repeat is a whole period after it.
        int64_t period = host::CounterTicks(kRepeatPeriod);
        nextRepeat_ = now - nextRepeat_ >= period ? now + period : nextRepeat_ + period;
    }

    void PadActions::Reset()
    {
        pads_.clear();
        held_ = 0;
        repeating_ = -1;
    }

    void PadActions::SuppressRepeat()
    {
        repeating_ = -1;
    }
}
