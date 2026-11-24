// The port menu's pages (pages.h).

#include "ui/pages.h"

#include "game/game_options.h"
#include "game/interpolation/clock.h"
#include "game/menu_entries.h"
#include "gpu/gpu.h"
#include "gpu/renderer/renderer.h"
#include "gpu/settings.h"
#include "host/audio.h"
#include "host/input.h"
#include "host/window.h"
#include "log.h"
#include "settings/settings.h"
#include "ui/ui.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace ui::pages
{
    namespace
    {
        using settings::PadInput;
        using settings::Setting;

        enum class Control
        {
            Choice, // one of a list, with arrows
            Bar,    // 0 to steps
            Toggle, // yes (1) or no (0)
            Keys,   // two key slots of a pad input
            Action, // a row that does something when chosen (its data-action)
        };

        struct Row
        {
            std::string label;
            std::string description;
            Control control = Control::Choice;
            bool game = false;         // one of the game's options
            bool restart = false;      // applies at the next start
            std::function<std::vector<std::string>()> choices; // Choice
            int steps = 0;                                     // Bar
            std::function<int()> get;                          // the index, step or 0/1
            std::function<void(int)> set;
            std::function<std::string(int)> number; // Bar: the text beside it
            std::function<void()> reset;
            std::function<std::string()> locked; // why it cannot change now, or ""
            PadInput input = PadInput::A;        // Keys
            const char* action = nullptr;        // Action
        };

        constexpr const char* kTabs[] = { "GAME", "VIDEO", "AUDIO", "CONTROLS" };
        constexpr int kTabCount = 4;

        // The pad inputs, as the CONTROLS page names them, in the PadInput order.
        constexpr const char* kInputNames[] = { "A", "B", "X", "Y", "START", "BACK", "LB", "RB",
            "LEFT STICK PRESS", "RIGHT STICK PRESS", "LT", "RT", "D-PAD UP", "D-PAD DOWN", "D-PAD LEFT",
            "D-PAD RIGHT", "LEFT STICK UP", "LEFT STICK DOWN", "LEFT STICK LEFT", "LEFT STICK RIGHT",
            "RIGHT STICK UP", "RIGHT STICK DOWN", "RIGHT STICK LEFT", "RIGHT STICK RIGHT" };
        static_assert(std::size(kInputNames) == size_t(PadInput::Count));

        class Listener final : public Rml::EventListener
        {
        public:
            void ProcessEvent(Rml::Event& event) override;
        };

        struct State
        {
            Rml::ElementDocument* menu = nullptr;   // the open menu.rml
            Rml::ElementDocument* rebind = nullptr; // the open rebind.rml
            Listener listener;
            int tab = 0;
            int pendingTab = -1;      // a tab to show at the next Update
            std::vector<Row> rows;    // the shown tab's rows
            std::vector<int> shown;   // each row's value as last drawn
            std::vector<std::string> shownLock; // each row's lock as last drawn
            std::vector<bool> dirty;
            int current = -1;         // the row described
            bool rebuild = false;
            int keySlot = 0;          // the slot a keys row acts on
            bool builtJapanese = false; // the AUDIO page was built with the Japanese voices' row
            std::chrono::steady_clock::time_point nextCheck; // the next look for values changed elsewhere
            PadInput rebindInput = PadInput::A;
            int rebindSlot = 0;
        };
        State g_pages;

        std::string Escape(const std::string& text)
        {
            return Rml::StringUtilities::EncodeRml(text);
        }

        std::string Upper(std::string text)
        {
            for (char& c : text)
                if (c >= 'a' && c <= 'z')
                    c = char(c - 'a' + 'A');
            return text;
        }

        std::string KeyName(uint16_t key)
        {
            if (key == 0)
                return "-";
            const char* name = SDL_GetScancodeName(SDL_Scancode(key));
            return name != nullptr && *name != 0 ? Upper(name) : "?";
        }

        // ---- What changes apply to ----

        void ApplyVSync()
        {
            if (gpu::Renderer* renderer = gpu::ActiveRenderer())
                renderer->SetVSync(settings::VSync());
        }

        void ApplyFrameRate()
        {
            game::interpolation::clock::RequestRate(settings::FrameRate());
        }

        void ApplyResolutionScale()
        {
            if (gpu::Renderer* renderer = gpu::ActiveRenderer())
                renderer->SetResolutionScale(settings::ResolutionScale());
        }

        void ApplyMultisampling()
        {
            if (gpu::Renderer* renderer = gpu::ActiveRenderer())
                renderer->SetMultisampling(settings::Multisampling());
        }

        void ApplyAnisotropy()
        {
            if (gpu::Renderer* renderer = gpu::ActiveRenderer())
                renderer->SetAnisotropy(settings::Anisotropy());
        }

        void ApplyTransparencyAa()
        {
            if (gpu::Renderer* renderer = gpu::ActiveRenderer())
                renderer->SetTransparencyAa(settings::TransparencyAa());
        }

        void ApplyImageFit()
        {
            if (gpu::Renderer* renderer = gpu::ActiveRenderer())
                renderer->SetImageFit(gpu::ToImageFit(settings::GetImageFit()));
        }

        std::string LockedBy(Setting setting)
        {
            if (!settings::IsOverridden(setting))
                return {};
            return std::string("SET BY ") + settings::OverrideVariable(setting) + " FOR THIS RUN";
        }

        // The Japanese voices' row: only where the game shows it, its Options from the main menu, and
        // when it offers them.
        bool JapaneseVoicesShown()
        {
            return ui::CurrentEntry() == ui::Entry::Options && game::menu::OptionsFromMainMenu() &&
                   game::options::JapaneseVoicesOffered();
        }

        // ---- The rows ----

        Row PortChoice(std::string label, std::string description, Setting setting,
            std::function<std::vector<std::string>()> choices, std::function<int()> get, std::function<void(int)> set,
            std::function<void()> apply)
        {
            Row row;
            row.label = std::move(label);
            row.description = std::move(description);
            row.control = Control::Choice;
            row.restart = settings::AppliesAtRestart(setting);
            row.choices = std::move(choices);
            row.get = std::move(get);
            row.set = [set = std::move(set), apply](int index) {
                set(index);
                if (apply)
                    apply();
            };
            row.reset = [setting, apply] {
                settings::ResetToDefault(setting);
                if (apply)
                    apply();
            };
            row.locked = [setting] { return LockedBy(setting); };
            return row;
        }

        Row GameToggle(std::string label, std::string description, int index, int defaultValue)
        {
            Row row;
            row.label = std::move(label);
            row.description = std::move(description);
            row.control = Control::Toggle;
            row.game = true;
            row.get = [index] { return game::options::Value(index) != 0 ? 1 : 0; };
            row.set = [index](int value) { game::options::Set(index, value); };
            row.reset = [index, defaultValue] { game::options::Set(index, defaultValue); };
            return row;
        }

        // A bar of the game's: 10 steps over its range (from, from + 10 * step).
        Row GameBar(std::string label, std::string description, int index, int from, int step, int defaultValue)
        {
            Row row;
            row.label = std::move(label);
            row.description = std::move(description);
            row.control = Control::Bar;
            row.game = true;
            row.steps = 10;
            row.get = [index, from, step] { return std::clamp((game::options::Value(index) - from) / step, 0, 10); };
            row.set = [index, from, step](int value) { game::options::Set(index, from + value * step); };
            row.number = [](int value) { return std::to_string(value); };
            row.reset = [index, defaultValue] { game::options::Set(index, defaultValue); };
            return row;
        }

        std::vector<int32_t> FrameRates()
        {
            std::vector<int32_t> rates = { 0, -1, 60, 120, 144, 165, 240 };
            int32_t stored = settings::FrameRateSetting();
            if (std::find(rates.begin(), rates.end(), stored) == rates.end())
            {
                auto at = std::find_if(rates.begin() + 2, rates.end(), [&](int32_t rate) { return rate > stored; });
                rates.insert(at, stored);
            }
            return rates;
        }

        // The resolution scales offered (0: auto), with a stored one that is not among them in its place.
        std::vector<float> ResolutionScales()
        {
            std::vector<float> scales = { 0.0f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f };
            float stored = settings::ResolutionScale();
            if (std::find(scales.begin(), scales.end(), stored) == scales.end())
                scales.insert(std::find_if(scales.begin() + 1, scales.end(), [&](float scale) { return scale > stored; }),
                    stored);
            return scales;
        }

        std::string ScaleName(float scale)
        {
            if (scale == 0.0f)
                return "AUTO";
            char text[32];
            std::snprintf(text, sizeof(text), "%gX (%dP)", double(scale), int(std::lround(720.0 * scale)));
            return text;
        }

        // A row choosing one of a list of numbers, 0 shown as zeroName and the others as "<n>X".
        Row LevelChoice(std::string label, std::string description, Setting setting, std::vector<uint32_t> levels,
            const char* zeroName, uint32_t (*get)(), void (*set)(uint32_t), void (*apply)())
        {
            auto names = [levels, zeroName] {
                std::vector<std::string> out;
                for (uint32_t level : levels)
                    out.push_back(level == 0 ? zeroName : level == 1 ? "OFF" : std::to_string(level) + "X");
                return out;
            };
            auto index = [levels, get] {
                auto at = std::find(levels.begin(), levels.end(), get());
                return at == levels.end() ? 0 : int(at - levels.begin());
            };
            auto choose = [levels, set](int i) {
                if (i >= 0 && i < int(levels.size()))
                    set(levels[size_t(i)]);
            };
            return PortChoice(std::move(label), std::move(description), setting, names, index, choose, apply);
        }

        std::vector<Row> GameRows()
        {
            std::vector<Row> rows;
            rows.push_back(PortChoice("LANGUAGE",
                "The game's texts. French also changes the voices and the movies; Spanish, Italian and German "
                "change the texts only.",
                Setting::Language, [] { return std::vector<std::string>{ "ENGLISH", "FRENCH", "SPANISH", "ITALIAN", "GERMAN" }; },
                [] { return int(settings::StoredGameLanguage()); },
                [](int index) { settings::SetGameLanguage(settings::Language(index)); }, nullptr));
            rows.push_back(GameToggle("SUBTITLES", "Subtitles in the cutscenes.", game::options::kSubtitles, 0));
            Row quit;
            quit.label = "QUIT TO DESKTOP";
            quit.description = "Closes the game, after asking. Progress since the last save is lost.";
            quit.control = Control::Action;
            quit.action = "push:quit.rml";
            rows.push_back(std::move(quit));
            return rows;
        }

        std::vector<Row> VideoRows()
        {
            std::vector<Row> rows;
            Row mode;
            mode.label = "WINDOW MODE";
            mode.description = "Windowed, or borderless: the whole monitor, without changing its mode. Alt+Enter "
                               "switches too.";
            mode.choices = [] { return std::vector<std::string>{ "WINDOWED", "BORDERLESS" }; };
            mode.get = [] { return host::IsBorderless() ? 1 : 0; };
            mode.set = [](int index) { host::SetBorderless(index == 1); };
            mode.reset = [] { host::SetBorderless(false); };
            rows.push_back(std::move(mode));

            Row size;
            size.label = "WINDOW SIZE";
            size.description = "The window's size in windowed mode.";
            size.choices = [] {
                std::vector<std::string> names;
                for (const host::WindowSize& size : host::WindowSizes())
                    names.push_back(std::to_string(size.width) + " X " + std::to_string(size.height));
                return names;
            };
            size.get = [] {
                std::vector<host::WindowSize> sizes = host::WindowSizes();
                auto at = std::find(sizes.begin(), sizes.end(), host::CurrentWindowSize());
                return at == sizes.end() ? 0 : int(at - sizes.begin());
            };
            size.set = [](int index) {
                std::vector<host::WindowSize> sizes = host::WindowSizes();
                if (index >= 0 && index < int(sizes.size()))
                    host::SetWindowSize(sizes[size_t(index)]);
            };
            size.reset = [] { host::SetWindowSize({ 1280, 720 }); };
            size.locked = [] { return host::IsBorderless() ? std::string("BORDERLESS: THE WHOLE MONITOR") : std::string(); };
            rows.push_back(std::move(size));

            Row monitor;
            monitor.label = "MONITOR";
            monitor.description = "The monitor the game is shown on.";
            monitor.choices = [] {
                std::vector<std::string> names = host::DisplayNames();
                for (size_t i = 0; i < names.size(); i++)
                    names[i] = std::to_string(i + 1) + " - " + Upper(names[i]);
                return names;
            };
            monitor.get = [] { return std::max(0, host::WindowDisplay()); };
            monitor.set = [](int index) { host::MoveToDisplay(index); };
            monitor.reset = [] {};
            rows.push_back(std::move(monitor));

            Row vsync = PortChoice("VSYNC",
                "Waits for the monitor's refresh before showing a frame: no tearing. Off, frames are paced by "
                "the port's limiter.",
                Setting::VSync, nullptr, [] { return settings::VSync() ? 1 : 0; },
                [](int value) { settings::SetVSync(value != 0); }, ApplyVSync);
            vsync.control = Control::Toggle;
            rows.push_back(std::move(vsync));

            rows.push_back(PortChoice("FRAME RATE",
                "Draws extra frames between the game's ticks, so the picture moves smoothly while the game keeps "
                "its own speed. GAME: the game's own rate (30 in the world, 60 in fights). DISPLAY: the "
                "monitor's refresh rate.",
                Setting::FrameRate,
                [] {
                    std::vector<std::string> names;
                    for (int32_t rate : FrameRates())
                        names.push_back(rate == 0 ? "GAME" : rate == -1 ? "DISPLAY" : std::to_string(rate) + " FPS");
                    return names;
                },
                [] {
                    std::vector<int32_t> rates = FrameRates();
                    return int(std::find(rates.begin(), rates.end(), settings::FrameRateSetting()) - rates.begin());
                },
                [](int index) {
                    std::vector<int32_t> rates = FrameRates();
                    if (index >= 0 && index < int(rates.size()))
                        settings::SetFrameRateSetting(rates[size_t(index)]);
                },
                ApplyFrameRate));

            rows.push_back(GameBar("BRIGHTNESS", "The game's brightness.", game::options::kBrightness, 6, 2, 10));

            rows.push_back(PortChoice("RESOLUTION",
                "The resolution the game is drawn at, as a multiple of its own 1280 x 720; in a window wider than "
                "16:9 the width grows with the picture. AUTO: the size the picture is shown at in the window. "
                "Higher is sharper and smoother, and costs more of the graphics card.",
                Setting::ResolutionScale,
                [] {
                    std::vector<std::string> names;
                    for (float scale : ResolutionScales())
                        names.push_back(ScaleName(scale));
                    return names;
                },
                [] {
                    std::vector<float> scales = ResolutionScales();
                    return int(std::find(scales.begin(), scales.end(), settings::ResolutionScale()) - scales.begin());
                },
                [](int index) {
                    std::vector<float> scales = ResolutionScales();
                    if (index >= 0 && index < int(scales.size()))
                        settings::SetResolutionScale(scales[size_t(index)]);
                },
                ApplyResolutionScale));

            rows.push_back(LevelChoice("ANTI-ALIASING",
                "Multisampling: smooths the edges of polygons. GAME: the game's own (4x on the scene). Lowered to "
                "what the graphics card supports.",
                Setting::Multisampling, { 0, 1, 2, 4, 8 }, "GAME", settings::Multisampling,
                settings::SetMultisampling, ApplyMultisampling));

            Row transparency = PortChoice("TRANSPARENCY AA",
                "Smooths the cut-out edges of leaves, grass and hair, where anti-aliasing is on.",
                Setting::TransparencyAa, nullptr, [] { return settings::TransparencyAa() ? 1 : 0; },
                [](int value) { settings::SetTransparencyAa(value != 0); }, ApplyTransparencyAa);
            transparency.control = Control::Toggle;
            rows.push_back(std::move(transparency));

            rows.push_back(LevelChoice("TEXTURE FILTERING",
                "Anisotropic filtering: keeps the ground and walls sharp when seen at a grazing angle. GAME: the "
                "game's own filters.",
                Setting::Anisotropy, { 0, 1, 2, 4, 8, 16 }, "GAME", settings::Anisotropy, settings::SetAnisotropy,
                ApplyAnisotropy));

            Row wide = PortChoice("WIDE PICTURE",
                "In a window wider than 16:9 (up to 32:9), the picture fills it and shows more on the sides. Off, "
                "the picture keeps 16:9.",
                Setting::Ultrawide, nullptr, [] { return settings::Ultrawide() ? 1 : 0; },
                [](int value) { settings::SetUltrawide(value != 0); }, nullptr);
            wide.control = Control::Toggle;
            rows.push_back(std::move(wide));

            rows.push_back(PortChoice("IMAGE FIT",
                "How the picture fills the window. KEEP ASPECT: as large as its shape allows. STRETCH: the whole "
                "window. INTEGER: a whole multiple of its size, for sharp pixels.",
                Setting::ImageFit, [] { return std::vector<std::string>{ "KEEP ASPECT", "STRETCH", "INTEGER" }; },
                [] { return int(settings::GetImageFit()); },
                [](int index) { settings::SetImageFit(settings::ImageFit(index)); }, ApplyImageFit));

            rows.push_back(PortChoice("GRAPHICS CARD", "The graphics card the game draws with.", Setting::Adapter,
                [] {
                    std::vector<std::string> names = { "DEFAULT" };
                    for (const std::string& name : gpu::Renderer::AdapterNames())
                        names.push_back(Upper(name));
                    const std::string& stored = settings::StoredAdapter();
                    std::vector<std::string> adapters = gpu::Renderer::AdapterNames();
                    if (!stored.empty() && std::find(adapters.begin(), adapters.end(), stored) == adapters.end())
                        names.push_back(Upper(stored));
                    return names;
                },
                [] {
                    const std::string& stored = settings::StoredAdapter();
                    if (stored.empty())
                        return 0;
                    std::vector<std::string> adapters = gpu::Renderer::AdapterNames();
                    auto at = std::find(adapters.begin(), adapters.end(), stored);
                    return 1 + int(at - adapters.begin());
                },
                [](int index) {
                    std::vector<std::string> adapters = gpu::Renderer::AdapterNames();
                    if (index == 0)
                        settings::SetAdapter("");
                    else if (index - 1 < int(adapters.size()))
                        settings::SetAdapter(adapters[size_t(index - 1)]);
                },
                nullptr));
            return rows;
        }

        std::vector<Row> AudioRows()
        {
            std::vector<Row> rows;
            Row volume = PortChoice("MASTER VOLUME", "The volume of everything, over the game's own volumes.",
                Setting::Volume, nullptr, [] { return int(settings::Volume() / 5); },
                [](int step) { settings::SetVolume(uint32_t(step * 5)); }, host::ApplyAudioVolume);
            volume.control = Control::Bar;
            volume.steps = 20;
            volume.number = [](int step) { return std::to_string(step * 5); };
            rows.push_back(std::move(volume));

            rows.push_back(GameBar("MUSIC", "The game's music volume.", game::options::kMusic, 0, 1, 10));
            rows.push_back(GameBar("VOICES", "The game's voice volume.", game::options::kVoice, 0, 1, 10));
            rows.push_back(GameBar("EFFECTS", "The game's sound effects volume.", game::options::kEffects, 0, 1, 10));
            if (JapaneseVoicesShown())
                rows.push_back(GameToggle("JAPANESE VOICES", "The Japanese voices, downloadable content of the game.",
                    game::options::kJapaneseVoices, 0));

            rows.push_back(PortChoice("OUTPUT DEVICE", "Where the sound is played. DEFAULT follows the system's default.",
                Setting::AudioDevice,
                [] {
                    std::vector<std::string> names = { "DEFAULT" };
                    for (const std::string& name : host::AudioDeviceNames())
                        names.push_back(Upper(name));
                    return names;
                },
                [] {
                    const std::string& wanted = settings::AudioDevice();
                    if (wanted.empty())
                        return 0;
                    std::vector<std::string> devices = host::AudioDeviceNames();
                    auto at = std::find(devices.begin(), devices.end(), wanted);
                    return at == devices.end() ? 0 : 1 + int(at - devices.begin());
                },
                [](int index) {
                    std::vector<std::string> devices = host::AudioDeviceNames();
                    std::string device = index > 0 && index - 1 < int(devices.size()) ? devices[size_t(index - 1)] : "";
                    settings::SetAudioDevice(device);
                },
                [] { host::RequestAudioDevice(settings::AudioDevice()); }));

            Row mute = PortChoice("MUTE IN BACKGROUND", "Silences the game while its window is not in front.",
                Setting::MuteInBackground, nullptr, [] { return settings::MuteInBackground() ? 1 : 0; },
                [](int value) { settings::SetMuteInBackground(value != 0); }, host::ApplyAudioVolume);
            mute.control = Control::Toggle;
            rows.push_back(std::move(mute));
            return rows;
        }

        std::vector<Row> ControlRows()
        {
            std::vector<Row> rows;
            rows.push_back(GameToggle("VIBRATION", "The controller's vibration.", game::options::kVibration, 1));
            rows.push_back(GameToggle("INVERT X", "Inverts the camera's horizontal movement.", game::options::kInvertX, 0));
            rows.push_back(GameToggle("INVERT Y", "Inverts the camera's vertical movement.", game::options::kInvertY, 0));
            Row deadZone = PortChoice("DEAD ZONE",
                "How far a stick moves before the game sees it, in percent of its travel, before the game's own.",
                Setting::DeadZone, nullptr, [] { return int(settings::DeadZone() / 5); },
                [](int step) { settings::SetDeadZone(uint32_t(step * 5)); }, host::ApplyInputSettings);
            deadZone.control = Control::Bar;
            deadZone.steps = 10;
            deadZone.number = [](int step) { return std::to_string(step * 5) + "%"; };
            rows.push_back(std::move(deadZone));

            for (size_t i = 0; i < size_t(PadInput::Count); i++)
            {
                Row keys;
                keys.label = kInputNames[i];
                keys.description = std::string("The keys that press the controller's ") + kInputNames[i] +
                                   ". Choose a slot to press a new key; a key bound elsewhere moves here.";
                keys.control = Control::Keys;
                keys.input = PadInput(i);
                keys.get = [input = PadInput(i)] {
                    std::array<uint16_t, 2> slots = settings::KeyboardKeys(input);
                    return int(slots[0]) | (int(slots[1]) << 16);
                };
                keys.reset = [input = PadInput(i)] {
                    settings::SetKeyboardKeys(input, settings::DefaultKeyboardKeys(input));
                    host::ApplyInputSettings();
                };
                rows.push_back(std::move(keys));
            }
            return rows;
        }

        std::vector<Row> TabRows(int tab)
        {
            switch (tab)
            {
            case 0: return GameRows();
            case 1: return VideoRows();
            case 2: return AudioRows();
            default: return ControlRows();
            }
        }

        // ---- Drawing ----

        std::string Lock(const Row& row)
        {
            if (row.game && !game::options::Loaded())
                return "AVAILABLE AFTER PRESS START";
            if (row.game && !game::options::Ready())
                return "READING THE GAME'S VALUE";
            return row.locked ? row.locked() : std::string();
        }

        int Value(const Row& row)
        {
            if (row.game && !game::options::Ready())
                return 0;
            return row.get ? row.get() : 0;
        }


        std::string ValueMarkup(size_t index, const Row& row)
        {
            int value = Value(row);
            switch (row.control)
            {
            case Control::Choice:
            {
                std::vector<std::string> choices = row.choices();
                std::string text = value >= 0 && value < int(choices.size()) ? choices[size_t(value)] : "-";
                return "<span class=\"arrow left\" data-step=\"-1\"></span><span class=\"text\">" + Escape(text) +
                       "</span><span class=\"arrow right\" data-step=\"1\"></span>";
            }
            case Control::Toggle:
                return std::string("<span class=\"pick") + (value != 0 ? " on" : "") +
                       "\" data-set=\"1\">YES</span><span class=\"pick" + (value == 0 ? " on" : "") +
                       "\" data-set=\"0\">NO</span>";
            case Control::Bar:
            {
                char width[32];
                std::snprintf(width, sizeof(width), "%.2f%%", 100.0 * value / std::max(1, row.steps));
                return std::string("<div class=\"bar\" data-bar=\"1\"><div class=\"fill\" style=\"width: ") + width +
                       ";\"><div class=\"full\"></div></div></div><span class=\"number\">" +
                       Escape(row.number ? row.number(value) : std::to_string(value)) + "</span>";
            }
            case Control::Keys:
            {
                std::array<uint16_t, 2> slots = settings::KeyboardKeys(row.input);
                std::string out;
                for (int slot = 0; slot < 2; slot++)
                    out += "<span class=\"slot\" id=\"slot-" + std::to_string(index) + "-" + std::to_string(slot) +
                           "\" data-slot=\"" + std::to_string(slot) + "\">" + Escape(KeyName(slots[size_t(slot)])) +
                           "</span>";
                return out;
            }
            case Control::Action:
                return {};
            }
            return {};
        }

        std::string RowMarkup(size_t index, const Row& row)
        {
            std::string lock = Lock(row);
            std::string classes = "row";
            if (row.control == Control::Keys)
                classes += " keys";
            if (row.control == Control::Action)
                classes += " action";
            if (!lock.empty())
                classes += " locked";
            std::string out = "<div class=\"" + classes + "\" id=\"row-" + std::to_string(index) + "\" data-row=\"" +
                              std::to_string(index) + "\"";
            if (row.action != nullptr)
                out += std::string(" data-action=\"") + row.action + "\"";
            out += "><div class=\"cloud\"></div><span class=\"name\">" + Escape(row.label) + "</span>";
            if (row.game)
                out += "<span class=\"tag\">GAME</span>";
            out += "<div class=\"value\" id=\"value-" + std::to_string(index) + "\">" + ValueMarkup(index, row) + "</div></div>";
            return out;
        }

        void Describe(State& pages)
        {
            if (pages.menu == nullptr)
                return;
            Rml::Element* title = pages.menu->GetElementById("desc-title");
            Rml::Element* text = pages.menu->GetElementById("desc-text");
            Rml::Element* values = pages.menu->GetElementById("desc-values");
            Rml::Element* note = pages.menu->GetElementById("desc-note");
            if (title == nullptr || text == nullptr || values == nullptr || note == nullptr)
                return;
            if (pages.current < 0 || pages.current >= int(pages.rows.size()))
            {
                title->SetInnerRML(kTabs[pages.tab]);
                text->SetInnerRML("");
                values->SetInnerRML("");
                note->SetInnerRML("");
                return;
            }
            const Row& row = pages.rows[size_t(pages.current)];
            title->SetInnerRML(Escape(row.label));
            text->SetInnerRML(Escape(Upper(row.description)));
            std::string list;
            if (row.control == Control::Choice)
            {
                std::vector<std::string> choices = row.choices();
                if (choices.size() <= 8)
                    for (size_t i = 0; i < choices.size(); i++)
                        list += (i == 0 ? "" : " &nbsp; ") + Escape(choices[i]);
            }
            values->SetInnerRML(list);
            std::string lock = Lock(row);
            std::string noteText = !lock.empty()           ? lock
                                   : row.control == Control::Action ? ""
                                   : row.game              ? "THE GAME'S OWN OPTION, APPLIES AT ONCE"
                                   : row.restart           ? "APPLIES AT THE NEXT START"
                                                           : "APPLIES AT ONCE";
            note->SetInnerRML(Escape(noteText));
            note->SetClass("restart", row.restart || !lock.empty());
        }

        void SetCurrent(State& pages, int index)
        {
            if (index == pages.current || pages.menu == nullptr)
                return;
            if (Rml::Element* old = pages.menu->GetElementById("row-" + std::to_string(pages.current)))
                old->SetClass("current", false);
            pages.current = index;
            if (Rml::Element* row = pages.menu->GetElementById("row-" + std::to_string(index)))
                row->SetClass("current", true);
            // A keys row starts on its first slot.
            pages.keySlot = 0;
            for (int slot = 0; slot < 2; slot++)
                if (Rml::Element* element = pages.menu->GetElementById(
                        "slot-" + std::to_string(index) + "-" + std::to_string(slot)))
                    element->SetClass("chosen", slot == 0);
            Describe(pages);
        }

        // Builds the shown tab's rows into #rows.
        void Build(State& pages)
        {
            Rml::Element* container = pages.menu != nullptr ? pages.menu->GetElementById("rows") : nullptr;
            if (container == nullptr)
                return;
            pages.rows = TabRows(pages.tab);
            pages.builtJapanese = JapaneseVoicesShown();
            pages.shown.assign(pages.rows.size(), 0);
            pages.shownLock.assign(pages.rows.size(), {});
            pages.dirty.assign(pages.rows.size(), false);
            std::string markup;
            for (size_t i = 0; i < pages.rows.size(); i++)
            {
                markup += RowMarkup(i, pages.rows[i]);
                pages.shown[i] = Value(pages.rows[i]);
                pages.shownLock[i] = Lock(pages.rows[i]);
            }
            container->SetInnerRML(markup);
            container->SetScrollTop(0.0f);
            for (int tab = 0; tab < kTabCount; tab++)
                if (Rml::Element* element = pages.menu->GetElementById("tab-" + std::to_string(tab)))
                    element->SetClass("selected", tab == pages.tab);
            pages.current = -1;
            SetCurrent(pages, 0);
        }

        void Refresh(State& pages, size_t index)
        {
            const Row& row = pages.rows[index];
            pages.shown[index] = Value(row);
            pages.shownLock[index] = Lock(row);
            Rml::Element* element = pages.menu->GetElementById("row-" + std::to_string(index));
            if (element == nullptr)
                return;
            element->SetClass("locked", !pages.shownLock[index].empty());
            if (row.control == Control::Keys)
            {
                // The slots keep the focus: only their texts change.
                std::array<uint16_t, 2> slots = settings::KeyboardKeys(row.input);
                for (int slot = 0; slot < 2; slot++)
                    if (Rml::Element* text = pages.menu->GetElementById(
                            "slot-" + std::to_string(index) + "-" + std::to_string(slot)))
                        text->SetInnerRML(Escape(KeyName(slots[size_t(slot)])));
            }
            else if (Rml::Element* value = pages.menu->GetElementById("value-" + std::to_string(index)))
                value->SetInnerRML(ValueMarkup(index, row));
            if (int(index) == pages.current)
                Describe(pages);
        }

        // ---- Changing values ----

        int RowIndexOf(Rml::Element* element)
        {
            for (; element != nullptr; element = element->GetParentNode())
            {
                if (const Rml::Variant* index = element->GetAttribute("data-row"))
                    return index->Get<int>();
                if (element == element->GetOwnerDocument())
                    break;
            }
            return -1;
        }

        Rml::Element* WithAttribute(Rml::Element* element, const char* name)
        {
            for (; element != nullptr; element = element->GetParentNode())
            {
                if (element->HasAttribute(name))
                    return element;
                if (element->HasAttribute("data-row") || element == element->GetOwnerDocument())
                    break;
            }
            return nullptr;
        }

        bool Changeable(State& pages, int index)
        {
            return index >= 0 && index < int(pages.rows.size()) && Lock(pages.rows[size_t(index)]).empty();
        }

        // Sets a row's value (an index, a step or 0/1), within its range.
        void SetValue(State& pages, int index, int value)
        {
            Row& row = pages.rows[size_t(index)];
            int last = row.control == Control::Choice ? int(row.choices().size()) - 1
                       : row.control == Control::Bar  ? row.steps
                                                      : 1;
            value = std::clamp(value, 0, std::max(0, last));
            if (value == Value(row) || !row.set)
                return;
            row.set(value);
            pages.dirty[size_t(index)] = true;
        }

        void Step(State& pages, int index, int delta)
        {
            Row& row = pages.rows[size_t(index)];
            if (row.control == Control::Toggle)
                SetValue(pages, index, delta < 0 ? 1 : 0); // YES on the left, NO on the right
            else
                SetValue(pages, index, Value(row) + delta);
        }

        void Reset(State& pages, int index)
        {
            if (!Changeable(pages, index))
                return;
            Row& row = pages.rows[size_t(index)];
            if (row.reset)
                row.reset();
            if (row.control == Control::Keys)
                std::fill(pages.dirty.begin(), pages.dirty.end(), true);
            else
                pages.dirty[size_t(index)] = true;
        }

        void ShowTab(State& pages, int tab)
        {
            pages.pendingTab = (tab % kTabCount + kTabCount) % kTabCount;
        }

        // The key slot a keys row acts on: left and right choose it, the mouse too.
        void ChooseSlot(State& pages, int index, int slot)
        {
            pages.keySlot = slot;
            for (int i = 0; i < 2; i++)
                if (Rml::Element* element = pages.menu->GetElementById(
                        "slot-" + std::to_string(index) + "-" + std::to_string(i)))
                    element->SetClass("chosen", i == slot);
        }

        // The next key for the slot being rebound (ui::CaptureKey).
        void OnRebindKey(int scancode)
        {
            State& pages = g_pages;
            if (scancode != SDL_SCANCODE_ESCAPE)
            {
                std::array<uint16_t, 2> keys = settings::KeyboardKeys(pages.rebindInput);
                if (scancode == SDL_SCANCODE_BACKSPACE)
                    keys[size_t(pages.rebindSlot)] = 0;
                else
                {
                    // A key bound elsewhere moves here.
                    for (size_t i = 0; i < size_t(PadInput::Count); i++)
                    {
                        if (PadInput(i) == pages.rebindInput)
                            continue;
                        std::array<uint16_t, 2> other = settings::KeyboardKeys(PadInput(i));
                        bool changed = false;
                        for (uint16_t& key : other)
                            if (key == uint16_t(scancode))
                            {
                                key = 0;
                                changed = true;
                            }
                        if (changed)
                            settings::SetKeyboardKeys(PadInput(i), other);
                    }
                    size_t otherSlot = size_t(1 - pages.rebindSlot);
                    if (keys[otherSlot] == uint16_t(scancode))
                        keys[otherSlot] = 0;
                    keys[size_t(pages.rebindSlot)] = uint16_t(scancode);
                }
                settings::SetKeyboardKeys(pages.rebindInput, keys);
                host::ApplyInputSettings();
                Log("menu: %s keys: %s, %s", kInputNames[size_t(pages.rebindInput)],
                    KeyName(settings::KeyboardKeys(pages.rebindInput)[0]).c_str(),
                    KeyName(settings::KeyboardKeys(pages.rebindInput)[1]).c_str());
                std::fill(pages.dirty.begin(), pages.dirty.end(), true);
            }
            ui::Back();
        }

        void StartRebind(State& pages, int index, int slot)
        {
            pages.rebindInput = pages.rows[size_t(index)].input;
            pages.rebindSlot = slot;
            if (!ui::Push("rebind.rml"))
                return;
            if (pages.rebind != nullptr)
                if (Rml::Element* name = pages.rebind->GetElementById("rebind-input"))
                    name->SetInnerRML(Escape(kInputNames[size_t(pages.rebindInput)]));
            ui::CaptureKey(OnRebindKey);
        }

        void Listener::ProcessEvent(Rml::Event& event)
        {
            State& pages = g_pages;
            if (pages.menu == nullptr)
                return;
            Rml::Element* target = event.GetTargetElement();
            const Rml::String& type = event.GetType();
            int index = RowIndexOf(target);

            if (type == "focus" || type == "mouseover")
            {
                if (index >= 0)
                {
                    SetCurrent(pages, index);
                    if (type == "focus")
                        target->ScrollIntoView(Rml::ScrollAlignment::Nearest);
                }
                return;
            }
            if (type == "tabprevious" || type == "tabnext")
            {
                ShowTab(pages, pages.tab + (type == "tabnext" ? 1 : -1));
                event.StopPropagation();
                return;
            }
            if (type == "reset")
            {
                Reset(pages, index >= 0 ? index : pages.current);
                event.StopPropagation();
                return;
            }
            if (type == "step")
            {
                if (index < 0)
                    return;
                if (pages.rows[size_t(index)].control == Control::Keys)
                    ChooseSlot(pages, index, event.GetParameter<int>("delta", 0) < 0 ? 0 : 1);
                else if (Changeable(pages, index) && pages.rows[size_t(index)].control != Control::Action)
                    Step(pages, index, event.GetParameter<int>("delta", 0));
                event.StopPropagation();
                return;
            }
            if (type != "click")
                return;

            if (Rml::Element* tab = WithAttribute(target, "data-tab"))
            {
                ShowTab(pages, tab->GetAttribute<int>("data-tab", 0));
                return;
            }
            if (Rml::Element* step = WithAttribute(target, "data-tab-step"))
            {
                ShowTab(pages, pages.tab + step->GetAttribute<int>("data-tab-step", 0));
                return;
            }
            if (WithAttribute(target, "data-reset") != nullptr)
            {
                Reset(pages, pages.current);
                return;
            }
            if (!Changeable(pages, index))
                return;
            Row& row = pages.rows[size_t(index)];
            if (Rml::Element* step = WithAttribute(target, "data-step"))
                Step(pages, index, step->GetAttribute<int>("data-step", 0));
            else if (Rml::Element* pick = WithAttribute(target, "data-set"))
                SetValue(pages, index, pick->GetAttribute<int>("data-set", 0));
            else if (Rml::Element* bar = WithAttribute(target, "data-bar"))
            {
                float left = bar->GetAbsoluteOffset(Rml::BoxArea::Border).x;
                float width = bar->GetBox().GetSize(Rml::BoxArea::Border).x;
                float x = event.GetParameter<float>("mouse_x", left);
                if (width > 0.0f)
                    SetValue(pages, index, int(std::lround((x - left) / width * float(row.steps))));
            }
            else if (Rml::Element* slot = WithAttribute(target, "data-slot"))
            {
                ChooseSlot(pages, index, slot->GetAttribute<int>("data-slot", 0));
                StartRebind(pages, index, pages.keySlot);
            }
            else if (row.control == Control::Keys)
                StartRebind(pages, index, pages.keySlot);
            else if (row.control == Control::Toggle)
                SetValue(pages, index, Value(row) != 0 ? 0 : 1);
            else if (row.control == Control::Choice)
            {
                // Accept steps through the choices, round.
                int count = int(row.choices().size());
                if (count > 0)
                    SetValue(pages, index, (Value(row) + 1) % count);
            }
        }
    }

    void Attach(Rml::ElementDocument* document)
    {
        State& pages = g_pages;
        if (document->GetElementById("rebind-input") != nullptr)
        {
            pages.rebind = document;
            return;
        }
        if (document->GetElementById("rows") == nullptr)
            return;
        pages.menu = document;
        for (const char* type : { "click", "step", "reset", "tabprevious", "tabnext", "mouseover" })
            document->AddEventListener(type, &pages.listener);
        // Focus does not bubble: taken on the way down.
        document->AddEventListener("focus", &pages.listener, true);
        pages.pendingTab = -1;
        Build(pages);
    }

    void Opened()
    {
        game::options::Watch(true);
    }

    void Closed()
    {
        State& pages = g_pages;
        pages.menu = nullptr;
        pages.rebind = nullptr;
        pages.rows.clear();
        game::options::Watch(false);
        if (game::options::Changed())
            game::options::RequestSave();
    }

    void Update()
    {
        State& pages = g_pages;
        if (pages.menu == nullptr)
            return;
        if (pages.pendingTab >= 0)
        {
            pages.tab = pages.pendingTab;
            pages.pendingTab = -1;
            Build(pages);
            // The focus goes to the first row unless the mouse drives.
            if (Rml::Element* first = pages.menu->GetElementById("row-0"))
                if (pages.menu->GetAttribute<Rml::String>("input", "mouse") != "mouse")
                    first->Focus(true);
            return;
        }
        // The game answers whether it offers the Japanese voices after the menu opened.
        if (pages.tab == 2 && JapaneseVoicesShown() != pages.builtJapanese)
        {
            ShowTab(pages, pages.tab);
            return;
        }
        for (size_t i = 0; i < pages.rows.size(); i++)
            if (pages.dirty[i])
            {
                pages.dirty[i] = false;
                Refresh(pages, i);
            }
        // Values changed elsewhere (the game's, the window's, a device): a few looks a second, as the
        // rows' values ask SDL for displays and devices.
        auto now = std::chrono::steady_clock::now();
        if (now < pages.nextCheck)
            return;
        pages.nextCheck = now + std::chrono::milliseconds(250);
        for (size_t i = 0; i < pages.rows.size(); i++)
            if (Value(pages.rows[i]) != pages.shown[i] || Lock(pages.rows[i]) != pages.shownLock[i])
                Refresh(pages, i);
    }
}
