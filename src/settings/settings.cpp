// The port's settings (settings.h).

#include "settings/settings.h"

#include "host/paths.h"
#include "log.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <span>
#include <sstream>
#include <string_view>
#include <windows.h>

namespace settings
{
    namespace
    {
        // The file's format version. A file with a higher one is read but never written.
        constexpr int64_t kVersion = 1;
        // The state interpolation rates; 30 and below is the game's own rate.
        constexpr int64_t kMinRate = 31;
        constexpr int64_t kMaxRate = 240;
        // The stored rate of "display".
        constexpr int64_t kDisplayRate = -1;
        // The stored position of "center".
        constexpr int64_t kCenter = INT64_MIN;

        enum class Kind : uint8_t
        {
            Bool,     // true, false
            Rate,     // "game" (0), "display", or an integer: 30 and below is "game", above kMaxRate is cut
            Integer,  // an integer from minimum to maximum; outside, clamped
            Position, // an integer from minimum to maximum, or "center"
            Text,     // a string
            Choice,   // one of the option's choices (or its alias), stored as its index
            Keys,     // an array of one or two SDL scancode names ("" for an empty slot), stored packed
        };

        // When a change applies, for the menu pages (phase 6).
        enum class Apply : uint8_t
        {
            Live,
            Restart,
        };

        struct Option
        {
            const char* table;
            const char* key;
            const char* variable = nullptr; // the environment variable that overrides it, if any
            Kind kind = Kind::Bool;
            Apply apply = Apply::Live;
            int64_t defaultNumber = 0;          // every kind but Text
            const char* defaultText = nullptr;  // Text
            std::span<const char* const> choices = {};
            const char* alias = nullptr; // Choice: another name read as choices[aliasIndex]
            int64_t aliasIndex = 0;
            int64_t minimum = 0, maximum = 0; // Integer, Position
            const char* comment = nullptr;    // written above the key; lines separated by '\n'
        };

        constexpr int64_t Keys(SDL_Scancode first, SDL_Scancode second = SDL_SCANCODE_UNKNOWN)
        {
            return int64_t(first) | (int64_t(second) << 16);
        }

        // In the order of Language.
        constexpr const char* kLanguages[] = { "english", "french", "spanish", "italian", "german" };
        // The XDK language of each (ExGetXConfigSetting(3, 9)): English 1, French 4, Spanish 5,
        // Italian 6, German 3 (game-side-findings.md section 8).
        constexpr uint32_t kXdkLanguages[] = { 1, 4, 5, 6, 3 };
        // In the order of WindowMode.
        constexpr const char* kWindowModes[] = { "windowed", "borderless" };

        enum Id : size_t
        {
            kVSync,
            kFrameRate,
            kAdapter,
            kWindowMode,
            kWindowWidth,
            kWindowHeight,
            kWindowX,
            kWindowY,
            kWindowMaximized,
            kVolume,
            kAudioDevice,
            kMuteInBackground,
            kDeadZone,
            kKeyboardFirst, // the PadInput order
            kLanguage = kKeyboardFirst + size_t(PadInput::Count),
            kCount,
        };

        constexpr Option Key(const char* key, int64_t keys, const char* comment = nullptr)
        {
            return { .table = "keyboard", .key = key, .kind = Kind::Keys, .defaultNumber = keys, .comment = comment };
        }

        // The option table, in the order of Id. Tables are written in the order they first appear.
        constexpr Option kOptions[] = {
            { .table = "video", .key = "vsync", .variable = "ROAN_VSYNC", .kind = Kind::Bool, .defaultNumber = 1,
                .comment = "Wait for the vertical blank when presenting. true, false. ROAN_VSYNC=0|1" },
            { .table = "video", .key = "frame_rate", .variable = "ROAN_FRAME_RATE", .kind = Kind::Rate,
                .comment = "\"game\": the game's own rate; \"display\": the monitor's refresh rate; 31 to 240: state\n"
                           "interpolation at that rate. ROAN_FRAME_RATE" },
            { .table = "video", .key = "adapter", .variable = "ROAN_ADAPTER", .kind = Kind::Text,
                .apply = Apply::Restart, .defaultText = "",
                .comment = "Graphics adapter, as named in the log; \"\": the default one. Applies at the next start.\n"
                           "ROAN_ADAPTER" },

            { .table = "window", .key = "mode", .kind = Kind::Choice, .choices = kWindowModes,
                .alias = "fullscreen", .aliasIndex = 1,
                .comment = "windowed, borderless (fullscreen on the window's monitor). Alt+Enter switches; kept on exit." },
            { .table = "window", .key = "width", .kind = Kind::Integer, .defaultNumber = 1280, .minimum = 320,
                .maximum = 16384,
                .comment = "The window's size and position in windowed mode, kept on exit; \"center\" centres it on the\n"
                           "primary monitor. The monitor is the one the position falls on; a position on no connected\n"
                           "monitor is centred again." },
            { .table = "window", .key = "height", .kind = Kind::Integer, .defaultNumber = 720, .minimum = 200,
                .maximum = 16384 },
            { .table = "window", .key = "x", .kind = Kind::Position, .defaultNumber = kCenter, .minimum = -1000000,
                .maximum = 1000000 },
            { .table = "window", .key = "y", .kind = Kind::Position, .defaultNumber = kCenter, .minimum = -1000000,
                .maximum = 1000000 },
            { .table = "window", .key = "maximized", .kind = Kind::Bool },

            { .table = "audio", .key = "volume", .kind = Kind::Integer, .defaultNumber = 100, .minimum = 0,
                .maximum = 100,
                .comment = "Master volume, 0 to 100, over the game's own music, voice and effects volumes." },
            { .table = "audio", .key = "device", .kind = Kind::Text, .defaultText = "",
                .comment = "Output device, as named in the log; \"\": the system's default (and it follows the default)." },
            { .table = "audio", .key = "mute_in_background", .kind = Kind::Bool,
                .comment = "Silence the game while its window is not focused. true, false." },

            { .table = "gamepad", .key = "dead_zone", .kind = Kind::Integer, .minimum = 0, .maximum = 50,
                .comment = "Stick dead zone in percent of full deflection, 0 to 50, before the game's own." },

            Key("a", Keys(SDL_SCANCODE_SPACE),
                "One or two keys per pad input, by SDL's key names; \"\" leaves a slot empty. Alt+Enter is kept\n"
                "for the window mode."),
            Key("b", Keys(SDL_SCANCODE_LSHIFT)),
            Key("x", Keys(SDL_SCANCODE_F)),
            Key("y", Keys(SDL_SCANCODE_R)),
            Key("start", Keys(SDL_SCANCODE_RETURN)),
            Key("back", Keys(SDL_SCANCODE_BACKSPACE)),
            Key("lb", Keys(SDL_SCANCODE_Q)),
            Key("rb", Keys(SDL_SCANCODE_E)),
            Key("ls", Keys(SDL_SCANCODE_TAB)),
            Key("rs", Keys(SDL_SCANCODE_O)),
            Key("lt", Keys(SDL_SCANCODE_Z)),
            Key("rt", Keys(SDL_SCANCODE_C)),
            Key("dpad_up", Keys(SDL_SCANCODE_UP)),
            Key("dpad_down", Keys(SDL_SCANCODE_DOWN)),
            Key("dpad_left", Keys(SDL_SCANCODE_LEFT)),
            Key("dpad_right", Keys(SDL_SCANCODE_RIGHT)),
            Key("left_stick_up", Keys(SDL_SCANCODE_W)),
            Key("left_stick_down", Keys(SDL_SCANCODE_S)),
            Key("left_stick_left", Keys(SDL_SCANCODE_A)),
            Key("left_stick_right", Keys(SDL_SCANCODE_D)),
            Key("right_stick_up", Keys(SDL_SCANCODE_I)),
            Key("right_stick_down", Keys(SDL_SCANCODE_K)),
            Key("right_stick_left", Keys(SDL_SCANCODE_J)),
            Key("right_stick_right", Keys(SDL_SCANCODE_L)),

            { .table = "game", .key = "language", .variable = "ROAN_LANGUAGE", .kind = Kind::Choice,
                .apply = Apply::Restart, .choices = kLanguages,
                .comment = "english, french (texts, voices, movies), spanish, italian, german (texts only).\n"
                           "Applies at the next start. ROAN_LANGUAGE" },
        };
        static_assert(std::size(kOptions) == kCount);
        static_assert(std::size(kLanguages) == std::size(kXdkLanguages));

        constexpr const char* kHeader =
            "# roan-xenon settings. Edit with the game closed; missing or invalid keys take their defaults.\n"
            "# The environment variables in the comments override a value for one run and are never written here.\n"
            "# This file is rewritten by the port: comments added here are not kept.\n";

        struct Value
        {
            int64_t number = 0; // every kind but Text: Bool 0/1, Rate (0 game, -1 display), Choice index, Keys packed
            std::string text;   // Text

            bool operator==(const Value&) const = default;
        };

        using Values = std::array<Value, kCount>;

        Values Defaults()
        {
            Values values;
            for (size_t i = 0; i < kCount; i++)
            {
                values[i].number = kOptions[i].defaultNumber;
                values[i].text = kOptions[i].defaultText != nullptr ? kOptions[i].defaultText : "";
            }
            return values;
        }

        // Main thread, set by Load and by the setters.
        Values g_stored = Defaults();
        Values g_effective = Defaults();
        std::array<bool, kCount> g_overridden{};
        std::filesystem::path g_file;
        toml::table g_document;    // the file as read: the keys this build does not know are kept from it
        bool g_newer = false;      // the file has a higher version: never written
        bool g_unreadable = false; // the file exists but could not be read: never written
        bool g_changed = false;    // a setter changed a value since the last write
        uint32_t (*g_displayRefresh)() = nullptr;

        std::string Name(const Option& option)
        {
            return std::string(option.table) + "." + option.key;
        }

        bool Equal(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); i++)
            {
                char x = a[i] >= 'A' && a[i] <= 'Z' ? char(a[i] - 'A' + 'a') : a[i];
                char y = b[i] >= 'A' && b[i] <= 'Z' ? char(b[i] - 'A' + 'a') : b[i];
                if (x != y)
                    return false;
            }
            return true;
        }

        // A TOML basic string.
        std::string Quote(std::string_view text)
        {
            std::string out = "\"";
            for (char c : text)
            {
                if (c == '"' || c == '\\')
                {
                    out += '\\';
                    out += c;
                }
                else if (uint8_t(c) < 0x20 || c == 0x7F)
                {
                    char escape[8];
                    std::snprintf(escape, sizeof(escape), "\\u%04X", unsigned(uint8_t(c)));
                    out += escape;
                }
                else
                    out += c;
            }
            return out + "\"";
        }

        uint16_t KeyAt(int64_t keys, int slot)
        {
            return uint16_t(keys >> (16 * slot));
        }

        std::string KeyName(uint16_t key)
        {
            return key == 0 ? "" : SDL_GetScancodeName(SDL_Scancode(key));
        }

        // A value as written in the file (and in the log).
        std::string ValueText(const Option& option, const Value& value)
        {
            switch (option.kind)
            {
            case Kind::Bool: return value.number != 0 ? "true" : "false";
            case Kind::Rate:
                return value.number == 0 ? "\"game\"" : value.number == kDisplayRate ? "\"display\"" : std::to_string(value.number);
            case Kind::Integer: return std::to_string(value.number);
            case Kind::Position: return value.number == kCenter ? "\"center\"" : std::to_string(value.number);
            case Kind::Text: return Quote(value.text);
            case Kind::Choice: return Quote(option.choices[size_t(value.number)]);
            case Kind::Keys:
            {
                std::string text = "[" + Quote(KeyName(KeyAt(value.number, 0)));
                if (KeyAt(value.number, 1) != 0)
                    text += ", " + Quote(KeyName(KeyAt(value.number, 1)));
                return text + "]";
            }
            }
            return {};
        }

        // A TOML table without headers for its own values, as text ending in a newline; "" if empty.
        std::string Format(const toml::table& table)
        {
            if (table.empty())
                return {};
            std::ostringstream stream;
            stream << toml::toml_formatter{ table, toml::format_flags::none };
            std::string text = stream.str();
            if (!text.empty() && text.back() != '\n')
                text += '\n';
            return text;
        }

        // A node as one short line, for the log.
        std::string NodeText(const toml::node& node)
        {
            std::ostringstream stream;
            stream << toml::toml_formatter{ node, toml::format_flags::none };
            std::string text = stream.str();
            for (char& c : text)
                if (c == '\n' || c == '\r')
                    c = ' ';
            return text.size() > 60 ? text.substr(0, 57) + "..." : text;
        }

        enum class Parsed
        {
            Ok,
            Adjusted, // valid, but stored differently (clamped): logged, the file rewritten
            Invalid,
        };

        Parsed RateFromNumber(const Option& option, int64_t rate, Value& value)
        {
            if (rate > kMaxRate)
            {
                Log("settings: %s %lld above %lld, using %lld", Name(option).c_str(), (long long)rate,
                    (long long)kMaxRate, (long long)kMaxRate);
                value.number = kMaxRate;
                return Parsed::Adjusted;
            }
            value.number = rate >= kMinRate ? rate : 0;
            return Parsed::Ok;
        }

        Parsed IntegerFromNumber(const Option& option, int64_t number, Value& value)
        {
            int64_t clamped = std::clamp(number, option.minimum, option.maximum);
            value.number = clamped;
            if (clamped == number)
                return Parsed::Ok;
            Log("settings: %s %lld outside %lld to %lld, using %lld", Name(option).c_str(), (long long)number,
                (long long)option.minimum, (long long)option.maximum, (long long)clamped);
            return Parsed::Adjusted;
        }

        bool ParseInteger(std::string_view text, int64_t& number)
        {
            std::string digits(text);
            char* end = nullptr;
            number = std::strtoll(digits.c_str(), &end, 10);
            return !digits.empty() && *end == '\0';
        }

        // A value from text: the environment, and the strings of the file.
        Parsed FromText(const Option& option, std::string_view text, Value& value)
        {
            int64_t number = 0;
            switch (option.kind)
            {
            case Kind::Bool:
                value.number = text != "0" ? 1 : 0; // the variables' rule: 0 off, anything else on
                return Parsed::Ok;
            case Kind::Rate:
                if (Equal(text, "game"))
                {
                    value.number = 0;
                    return Parsed::Ok;
                }
                if (Equal(text, "display"))
                {
                    value.number = kDisplayRate;
                    return Parsed::Ok;
                }
                return ParseInteger(text, number) ? RateFromNumber(option, number, value) : Parsed::Invalid;
            case Kind::Position:
                if (Equal(text, "center"))
                {
                    value.number = kCenter;
                    return Parsed::Ok;
                }
                [[fallthrough]];
            case Kind::Integer:
                return ParseInteger(text, number) ? IntegerFromNumber(option, number, value) : Parsed::Invalid;
            case Kind::Text:
                value.text = text;
                return Parsed::Ok;
            case Kind::Choice:
                for (size_t i = 0; i < option.choices.size(); i++)
                {
                    if (Equal(text, option.choices[i]))
                    {
                        value.number = int64_t(i);
                        return Parsed::Ok;
                    }
                }
                if (option.alias != nullptr && Equal(text, option.alias))
                {
                    value.number = option.aliasIndex;
                    return Parsed::Ok;
                }
                return Parsed::Invalid;
            case Kind::Keys:
                return Parsed::Invalid;
            }
            return Parsed::Invalid;
        }

        Parsed KeysFromArray(const toml::array& array, Value& value)
        {
            if (array.empty() || array.size() > 2)
                return Parsed::Invalid;
            int64_t keys = 0;
            for (size_t i = 0; i < array.size(); i++)
            {
                const auto* name = array.get(i)->as_string();
                if (name == nullptr)
                    return Parsed::Invalid;
                if (name->get().empty())
                    continue;
                SDL_Scancode key = SDL_GetScancodeFromName(name->get().c_str());
                if (key == SDL_SCANCODE_UNKNOWN)
                    return Parsed::Invalid;
                keys |= int64_t(key) << (16 * i);
            }
            value.number = keys;
            return Parsed::Ok;
        }

        // A value from the file.
        Parsed FromNode(const Option& option, const toml::node& node, Value& value)
        {
            switch (option.kind)
            {
            case Kind::Bool:
                if (const auto* boolean = node.as_boolean())
                {
                    value.number = boolean->get() ? 1 : 0;
                    return Parsed::Ok;
                }
                return Parsed::Invalid;
            case Kind::Rate:
                if (const auto* integer = node.as_integer())
                    return RateFromNumber(option, integer->get(), value);
                // "game" or "display"; a number in a string is not accepted.
                if (const auto* string = node.as_string(); string != nullptr &&
                    (Equal(string->get(), "game") || Equal(string->get(), "display")))
                    return FromText(option, string->get(), value);
                return Parsed::Invalid;
            case Kind::Integer:
            case Kind::Position:
                if (const auto* integer = node.as_integer())
                    return IntegerFromNumber(option, integer->get(), value);
                if (const auto* string = node.as_string(); string != nullptr && option.kind == Kind::Position &&
                    Equal(string->get(), "center"))
                {
                    value.number = kCenter;
                    return Parsed::Ok;
                }
                return Parsed::Invalid;
            case Kind::Text:
            case Kind::Choice:
                if (const auto* string = node.as_string())
                    return FromText(option, string->get(), value);
                return Parsed::Invalid;
            case Kind::Keys:
                if (const auto* array = node.as_array())
                    return KeysFromArray(*array, value);
                return Parsed::Invalid;
            }
            return Parsed::Invalid;
        }

        bool IsKnownTable(std::string_view name)
        {
            for (const Option& option : kOptions)
                if (name == option.table)
                    return true;
            return false;
        }

        bool IsKnownKey(std::string_view table, std::string_view key)
        {
            for (const Option& option : kOptions)
                if (table == option.table && key == option.key)
                    return true;
            return false;
        }

        // Whether a node is written as a table of its own ([name] or [[name]]) rather than as a value.
        bool IsTableLike(const toml::node& node)
        {
            if (node.is_array_of_tables())
                return true;
            const toml::table* table = node.as_table();
            return table != nullptr && !table->is_inline();
        }

        // Reads the stored values from g_document. True when the file should be rewritten.
        bool ReadDocument()
        {
            bool rewrite = false;
            int64_t version = 1;
            if (const toml::node* node = g_document.get("version"))
            {
                if (const auto* integer = node->as_integer())
                    version = integer->get();
                else
                {
                    Log("settings: version %s is not an integer, reading it as 1", NodeText(*node).c_str());
                    rewrite = true;
                }
            }
            else
                rewrite = true;
            if (version > kVersion)
            {
                g_newer = true;
                Log("settings: version %lld is newer than this build's (%lld): the known keys are read, the "
                    "file is not written", (long long)version, (long long)kVersion);
            }

            for (size_t i = 0; i < kCount; i++)
            {
                const Option& option = kOptions[i];
                const toml::node* tableNode = g_document.get(option.table);
                const toml::table* table = tableNode != nullptr ? tableNode->as_table() : nullptr;
                const toml::node* node = table != nullptr ? table->get(option.key) : nullptr;
                if (node == nullptr)
                {
                    if (tableNode != nullptr && table == nullptr)
                        Log("settings: %s is not a table; %s takes its default", option.table, Name(option).c_str());
                    rewrite = true;
                    continue;
                }
                Value value = g_stored[i];
                switch (FromNode(option, *node, value))
                {
                case Parsed::Ok:
                    g_stored[i] = value;
                    break;
                case Parsed::Adjusted:
                    g_stored[i] = value;
                    rewrite = true;
                    break;
                case Parsed::Invalid:
                    Log("settings: %s = %s is not valid, using %s", Name(option).c_str(), NodeText(*node).c_str(),
                        ValueText(option, g_stored[i]).c_str());
                    rewrite = true;
                    break;
                }
            }
            return rewrite;
        }

        enum class Read
        {
            Ok,
            Missing,
            Failed,
        };

        Read ReadText(const std::filesystem::path& file, std::string& text)
        {
            std::ifstream stream(file, std::ios::binary);
            if (!stream)
            {
                std::error_code error;
                return std::filesystem::exists(file, error) || error ? Read::Failed : Read::Missing;
            }
            text.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
            return stream.bad() ? Read::Failed : Read::Ok;
        }

        // The file's text: the known keys from the option table with their comments, then the keys of
        // g_document this build does not know (values in their table, tables after the known ones).
        std::string Text()
        {
            std::string out = kHeader;
            out += "version = " + std::to_string(kVersion) + "\n";

            toml::table rootValues, rest;
            for (auto&& [key, node] : g_document)
            {
                std::string_view name = key.str();
                if (name == "version" || IsKnownTable(name))
                    continue;
                (IsTableLike(node) ? rest : rootValues).insert(name, node);
            }
            out += Format(rootValues);

            for (size_t i = 0; i < kCount; i++)
            {
                const char* table = kOptions[i].table;
                bool first = true;
                for (size_t j = 0; j < i; j++)
                    first = first && std::string_view(kOptions[j].table) != table;
                if (!first)
                    continue;

                out += "\n[";
                out += table;
                out += "]\n";
                for (size_t j = i; j < kCount; j++)
                {
                    const Option& option = kOptions[j];
                    if (std::string_view(option.table) != table)
                        continue;
                    std::string_view comment = option.comment != nullptr ? option.comment : "";
                    while (!comment.empty())
                    {
                        size_t end = comment.find('\n');
                        out += "# ";
                        out += comment.substr(0, end);
                        out += '\n';
                        comment = end == std::string_view::npos ? std::string_view() : comment.substr(end + 1);
                    }
                    out += option.key;
                    out += " = " + ValueText(option, g_stored[j]) + "\n";
                }

                const toml::node* tableNode = g_document.get(table);
                if (const toml::table* known = tableNode != nullptr ? tableNode->as_table() : nullptr)
                {
                    toml::table values, tables;
                    for (auto&& [key, node] : *known)
                    {
                        if (IsKnownKey(table, key.str()))
                            continue;
                        (IsTableLike(node) ? tables : values).insert(key.str(), node);
                    }
                    out += Format(values);
                    if (!tables.empty())
                        rest.insert(table, std::move(tables));
                }
            }

            if (!rest.empty())
                out += "\n" + Format(rest);
            return out;
        }

        // A setter: the stored and the effective value (the window's options have no variable).
        void Set(Id id, const Value& value)
        {
            if (g_stored[id] == value)
                return;
            g_stored[id] = value;
            g_effective[id] = value;
            g_changed = true;
        }
    }

    void Load(const std::filesystem::path& file)
    {
        g_file = file;
        g_stored = Defaults();
        g_overridden = {};
        g_document = {};
        g_newer = false;
        g_unreadable = false;
        g_changed = false;
        std::string path = host::PathText(file);

        bool rewrite = false;
        std::string text;
        switch (ReadText(file, text))
        {
        case Read::Missing:
            Log("settings: %s not found, using the defaults", path.c_str());
            rewrite = true;
            break;
        case Read::Failed:
            Log("settings: error: cannot read %s; using the defaults, the file is left as it is", path.c_str());
            g_unreadable = true;
            break;
        case Read::Ok:
            try
            {
                g_document = toml::parse(text, path);
                rewrite = ReadDocument();
            }
            catch (const toml::parse_error& error)
            {
                const toml::source_position& at = error.source().begin;
                std::filesystem::path bad = file;
                bad += ".bad";
                bool kept = MoveFileExW(file.c_str(), bad.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
                Log("settings: error: %s line %u column %u: %.*s; using the defaults%s", path.c_str(),
                    unsigned(at.line), unsigned(at.column), int(error.description().size()),
                    error.description().data(),
                    kept ? (", the file is kept as " + host::PathText(bad)).c_str() : "");
                if (!kept)
                {
                    Log("settings: error: cannot rename it (error %lu); it is left as it is", GetLastError());
                    g_unreadable = true;
                }
                g_document = {};
                g_stored = Defaults();
                rewrite = kept;
            }
            break;
        }

        g_effective = g_stored;
        for (size_t i = 0; i < kCount; i++)
        {
            const Option& option = kOptions[i];
            const char* variable = option.variable != nullptr ? std::getenv(option.variable) : nullptr;
            if (variable == nullptr)
                continue;
            Value value = g_effective[i];
            if (FromText(option, variable, value) == Parsed::Invalid)
            {
                Log("settings: %s=%s is not valid for %s, ignored", option.variable, variable, Name(option).c_str());
                continue;
            }
            g_effective[i] = value;
            g_overridden[i] = true;
        }

        // The values not at their defaults, so the line stays short.
        Values defaults = Defaults();
        std::string summary;
        for (size_t i = 0; i < kCount; i++)
        {
            if (g_effective[i] == defaults[i] && !g_overridden[i])
                continue;
            summary += summary.empty() ? "" : ", ";
            summary += Name(kOptions[i]) + " = " + ValueText(kOptions[i], g_effective[i]);
            if (g_overridden[i])
                summary += std::string(" (") + kOptions[i].variable + ")";
        }
        Log("settings: %s", summary.empty() ? "all at their defaults" : (summary + "; the rest at their defaults").c_str());

        if (rewrite && !g_newer && !g_unreadable)
            Save();
    }

    bool Save()
    {
        if (g_file.empty())
            return false;
        std::string path = host::PathText(g_file);
        if (g_newer || g_unreadable)
        {
            Log("settings: %s not written (%s)", path.c_str(),
                g_newer ? "a newer version" : "it could not be read");
            return false;
        }

        std::error_code error;
        std::filesystem::create_directories(g_file.parent_path(), error);
        std::filesystem::path temporary = g_file;
        temporary += ".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream << Text();
            stream.flush();
            if (!stream)
            {
                Log("settings: error: cannot write %s", host::PathText(temporary).c_str());
                return false;
            }
        }
        if (!MoveFileExW(temporary.c_str(), g_file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            Log("settings: error: cannot replace %s (error %lu)", path.c_str(), GetLastError());
            return false;
        }
        g_changed = false;
        Log("settings: wrote %s", path.c_str());
        return true;
    }

    bool SaveIfChanged()
    {
        return g_changed && Save();
    }

    void SetDisplayRefreshSource(uint32_t (*millihertz)())
    {
        g_displayRefresh = millihertz;
    }

    bool VSync()
    {
        return g_effective[kVSync].number != 0;
    }

    uint32_t FrameRate()
    {
        int64_t rate = g_effective[kFrameRate].number;
        if (rate != kDisplayRate)
            return uint32_t(rate);
        uint32_t hertz = g_displayRefresh != nullptr ? (g_displayRefresh() + 500) / 1000 : 0;
        return hertz < kMinRate ? 0 : std::min<uint32_t>(hertz, kMaxRate);
    }

    const std::string& Adapter()
    {
        return g_effective[kAdapter].text;
    }

    WindowMode GetWindowMode()
    {
        return WindowMode(g_effective[kWindowMode].number);
    }

    WindowRect GetWindowRect()
    {
        WindowRect rect;
        int64_t x = g_effective[kWindowX].number, y = g_effective[kWindowY].number;
        rect.centerX = x == kCenter;
        rect.centerY = y == kCenter;
        rect.x = rect.centerX ? 0 : int32_t(x);
        rect.y = rect.centerY ? 0 : int32_t(y);
        rect.width = int32_t(g_effective[kWindowWidth].number);
        rect.height = int32_t(g_effective[kWindowHeight].number);
        return rect;
    }

    bool WindowMaximized()
    {
        return g_effective[kWindowMaximized].number != 0;
    }

    void SetWindowMode(WindowMode mode)
    {
        Set(kWindowMode, Value{ int64_t(mode) });
    }

    void SetWindowRect(int32_t x, int32_t y, int32_t width, int32_t height)
    {
        auto clamp = [](Id id, int64_t number) {
            return Value{ std::clamp(number, kOptions[id].minimum, kOptions[id].maximum) };
        };
        Set(kWindowX, clamp(kWindowX, x));
        Set(kWindowY, clamp(kWindowY, y));
        Set(kWindowWidth, clamp(kWindowWidth, width));
        Set(kWindowHeight, clamp(kWindowHeight, height));
    }

    void SetWindowMaximized(bool maximized)
    {
        Set(kWindowMaximized, Value{ maximized ? 1 : 0 });
    }

    uint32_t Volume()
    {
        return uint32_t(g_effective[kVolume].number);
    }

    const std::string& AudioDevice()
    {
        return g_effective[kAudioDevice].text;
    }

    bool MuteInBackground()
    {
        return g_effective[kMuteInBackground].number != 0;
    }

    uint32_t DeadZone()
    {
        return uint32_t(g_effective[kDeadZone].number);
    }

    std::array<uint16_t, 2> KeyboardKeys(PadInput input)
    {
        int64_t keys = g_effective[kKeyboardFirst + size_t(input)].number;
        return { KeyAt(keys, 0), KeyAt(keys, 1) };
    }

    Language GameLanguage()
    {
        return Language(g_effective[kLanguage].number);
    }

    uint32_t XdkLanguage()
    {
        return kXdkLanguages[size_t(g_effective[kLanguage].number)];
    }
}
