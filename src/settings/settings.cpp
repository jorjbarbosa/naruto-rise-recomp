// The port's settings (settings.h).

#include "settings/settings.h"

#include "host/paths.h"
#include "log.h"

#include <toml++/toml.hpp>

#include <array>
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

        enum class Kind : uint8_t
        {
            Bool,   // true, false
            Rate,   // "game" (0), or an integer: 30 and below is "game", above kMaxRate is cut to it
            Text,   // a string
            Choice, // one of the option's choices, stored as its index
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
            const char* variable; // the environment variable that overrides it
            Kind kind;
            Apply apply;
            int64_t defaultNumber;   // Bool, Rate, Choice
            const char* defaultText; // Text
            std::span<const char* const> choices;
            const char* comment; // written above the key; lines separated by '\n'
        };

        // In the order of Language.
        constexpr const char* kLanguages[] = { "english", "french", "spanish", "italian", "german" };
        // The XDK language of each (ExGetXConfigSetting(3, 9)): English 1, French 4, Spanish 5,
        // Italian 6, German 3 (game-side-findings.md section 8).
        constexpr uint32_t kXdkLanguages[] = { 1, 4, 5, 6, 3 };

        enum Id : size_t
        {
            kVSync,
            kFrameRate,
            kAdapter,
            kLanguage,
            kCount,
        };

        // The option table, in the order of Id. Tables are written in the order they first appear.
        constexpr Option kOptions[] = {
            { "video", "vsync", "ROAN_VSYNC", Kind::Bool, Apply::Live, 1, nullptr, {},
                "Wait for the vertical blank when presenting. true, false. ROAN_VSYNC=0|1" },
            { "video", "frame_rate", "ROAN_FRAME_RATE", Kind::Rate, Apply::Live, 0, nullptr, {},
                "\"game\": the game's own rate; 31 to 240: state interpolation at that rate. ROAN_FRAME_RATE" },
            { "video", "adapter", "ROAN_ADAPTER", Kind::Text, Apply::Restart, 0, "", {},
                "Graphics adapter, as named in the log; \"\": the default one. Applies at the next start.\n"
                "ROAN_ADAPTER" },
            { "game", "language", "ROAN_LANGUAGE", Kind::Choice, Apply::Restart, 0, nullptr, kLanguages,
                "english, french (texts, voices, movies), spanish, italian, german (texts only).\n"
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
            int64_t number = 0; // Bool (0, 1), Rate (0: the game's), Choice (index)
            std::string text;   // Text
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

        // Main thread, set by Load (stored, effective) and later by the menu (stored).
        Values g_stored = Defaults();
        Values g_effective = Defaults();
        std::array<bool, kCount> g_overridden{};
        std::filesystem::path g_file;
        toml::table g_document;   // the file as read: the keys this build does not know are kept from it
        bool g_newer = false;     // the file has a higher version: never written
        bool g_unreadable = false; // the file exists but could not be read: never written

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

        // A value as written in the file (and in the log).
        std::string ValueText(const Option& option, const Value& value)
        {
            switch (option.kind)
            {
            case Kind::Bool: return value.number != 0 ? "true" : "false";
            case Kind::Rate: return value.number == 0 ? "\"game\"" : std::to_string(value.number);
            case Kind::Text: return Quote(value.text);
            case Kind::Choice: return Quote(option.choices[size_t(value.number)]);
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
            Adjusted, // valid, but stored differently (a rate cut to kMaxRate): logged, the file rewritten
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

        // A value from text: the environment, and the strings of the file.
        Parsed FromText(const Option& option, std::string_view text, Value& value)
        {
            switch (option.kind)
            {
            case Kind::Bool:
                value.number = text != "0" ? 1 : 0; // the variables' rule: 0 off, anything else on
                return Parsed::Ok;
            case Kind::Rate:
            {
                if (Equal(text, "game"))
                {
                    value.number = 0;
                    return Parsed::Ok;
                }
                std::string digits(text);
                char* end = nullptr;
                long long rate = std::strtoll(digits.c_str(), &end, 10);
                if (digits.empty() || *end != '\0')
                    return Parsed::Invalid;
                return RateFromNumber(option, rate, value);
            }
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
                return Parsed::Invalid;
            }
            return Parsed::Invalid;
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
                if (const auto* string = node.as_string(); string != nullptr && Equal(string->get(), "game"))
                {
                    value.number = 0;
                    return Parsed::Ok;
                }
                return Parsed::Invalid;
            case Kind::Text:
            case Kind::Choice:
                if (const auto* string = node.as_string())
                    return FromText(option, string->get(), value);
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
                    std::string_view comment = option.comment;
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
    }

    void Load(const std::filesystem::path& file)
    {
        g_file = file;
        g_stored = Defaults();
        g_overridden = {};
        g_document = {};
        g_newer = false;
        g_unreadable = false;
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
            const char* variable = std::getenv(option.variable);
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

        std::string summary;
        for (size_t i = 0; i < kCount; i++)
        {
            summary += summary.empty() ? "" : ", ";
            summary += Name(kOptions[i]) + " = " + ValueText(kOptions[i], g_effective[i]);
            if (g_overridden[i])
                summary += std::string(" (") + kOptions[i].variable + ")";
        }
        Log("settings: %s", summary.c_str());

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
        Log("settings: wrote %s", path.c_str());
        return true;
    }

    bool VSync()
    {
        return g_effective[kVSync].number != 0;
    }

    uint32_t FrameRate()
    {
        return uint32_t(g_effective[kFrameRate].number);
    }

    const std::string& Adapter()
    {
        return g_effective[kAdapter].text;
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
