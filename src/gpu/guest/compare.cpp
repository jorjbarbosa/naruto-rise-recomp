// The game side of the comparison harness: keys pressed at fixed frames, and frames recorded.

#include "gpu/guest/compare.h"

#include "gpu/renderer/renderer.h"
#include "host/input.h"
#include "host/window.h"
#include "log.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace gpu::guest
{
    namespace
    {
        // Frames a scripted key stays down: enough for a title that polls the pad every other frame.
        constexpr uint64_t kScriptHoldFrames = 4;

        struct ScriptKey
        {
            std::string name; // SDL key name
            uint64_t frame = 0;
        };

        struct Harness
        {
            bool enabled = false;
            std::vector<ScriptKey> keys;
            std::filesystem::path directory;
            std::vector<uint64_t> records; // frames to record, ascending
            uint64_t frame = 0;            // Swaps so far
        };

        // The items of a list "a,b,c".
        std::vector<std::string> Split(std::string_view list)
        {
            std::vector<std::string> items;
            while (!list.empty())
            {
                size_t comma = list.find(',');
                std::string_view item = list.substr(0, comma);
                if (!item.empty())
                    items.emplace_back(item);
                list = comma == std::string_view::npos ? std::string_view() : list.substr(comma + 1);
            }
            return items;
        }

        Harness ReadEnvironment()
        {
            Harness harness;
            if (const char* script = std::getenv("ROAN_SCRIPT"))
            {
                for (const std::string& item : Split(script))
                {
                    size_t at = item.find('@');
                    ScriptKey key;
                    key.name = item.substr(0, at);
                    key.frame = at != std::string::npos ? std::strtoull(item.c_str() + at + 1, nullptr, 10) : 0;
                    // Letting go of a key that is up checks its name and changes nothing.
                    if (key.frame == 0 || !host::SetScriptedKey(key.name.c_str(), false))
                    {
                        Log("compare: script item \"%s\" is not a key@frame; it is ignored", item.c_str());
                        continue;
                    }
                    harness.keys.push_back(std::move(key));
                }
            }
            const char* directory = std::getenv("ROAN_RECORD_DIR");
            const char* frames = std::getenv("ROAN_RECORD_FRAMES");
            if (directory != nullptr && frames != nullptr)
            {
                harness.directory = directory;
                for (const std::string& item : Split(frames))
                    if (uint64_t frame = std::strtoull(item.c_str(), nullptr, 10); frame > 1)
                        harness.records.push_back(frame);
                std::sort(harness.records.begin(), harness.records.end());
            }
            harness.enabled = !harness.keys.empty() || !harness.records.empty();
            if (harness.enabled)
                Log("compare: %zu scripted keys, %zu frames to record", harness.keys.size(), harness.records.size());
            return harness;
        }

        Harness& State()
        {
            static Harness harness = ReadEnvironment();
            return harness;
        }
    }

    void EndCompareFrame(Renderer* renderer)
    {
        Harness& harness = State();
        if (!harness.enabled)
            return;
        harness.frame++;
        for (const ScriptKey& key : harness.keys)
        {
            if (key.frame == harness.frame)
                host::SetScriptedKey(key.name.c_str(), true);
            else if (key.frame + kScriptHoldFrames == harness.frame)
                host::SetScriptedKey(key.name.c_str(), false);
        }
        if (renderer == nullptr || harness.records.empty())
            return;
        // Frame n is recorded from the end of frame n - 1 to its Present, written by then.
        if (std::binary_search(harness.records.begin(), harness.records.end(), harness.frame + 1))
            renderer->RecordNextFrame(harness.directory / ("frame-" + std::to_string(harness.frame + 1) + ".rxf"));
        if (harness.frame == harness.records.back())
        {
            Log("compare: last frame recorded; quitting");
            host::RequestQuit();
        }
    }
}
