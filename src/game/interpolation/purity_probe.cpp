// The display purity probe (purity_probe.h).

#include "game/interpolation/purity_probe.h"

#include "game/addresses.h"
#include "game/interpolation/diagnostics.h"
#include "game/interpolation/view_block.h"
#include "game/interpolation/world.h"
#include "game/render_packets.h"
#include "host/window.h"
#include "log.h"
#include "memory.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace game::interpolation::purity
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr uint32_t kRuns = 8; // main steps probed per F8 press
        constexpr std::chrono::milliseconds kPacketTimeout{ 500 };
        constexpr uint32_t kPage = mem::kPageSize;
        constexpr uint32_t kSteadyLogLines = 40;

        // Guest ranges whose writes the probe takes: the images and the heaps the game allocates from.
        // Not the physical heap, whose write watch belongs to the GPU's write checks (a written page is
        // reported to one caller only), and not the stacks.
        struct Range
        {
            uint32_t begin, size;
            const char* name;
        };
        constexpr Range kRanges[] = {
            { mem::kImageBegin, mem::kImageEnd - mem::kImageBegin, "image" },
            { mem::kAi2cImageBegin, mem::kAi2cImageEnd - mem::kAi2cImageBegin, "ai2c" },
            { mem::kSystemHeapBegin, mem::kSystemHeapEnd - mem::kSystemHeapBegin, "system heap" },
            { mem::kVirtualHeapBegin, mem::kVirtualHeapEnd - mem::kVirtualHeapBegin, "virtual heap" },
        };

        // A named guest range, to say what a changed word belongs to.
        struct Label
        {
            uint32_t begin, end;
            std::string name;
        };
        // The largest label extent (the view object), which bounds the backwards search.
        constexpr uint32_t kMaxLabelExtent = jade::view::kProbeSize;
        constexpr uint32_t kModifierDataHint = 256; // modifier data sizes are not known
        constexpr uint32_t kMaxModifiers = 64;

        struct WordStats
        {
            uint32_t replays = 0; // replays that changed the word
            uint32_t noise = 0;   // noise windows that changed it
            uint32_t before = 0, after = 0; // the first replay change
            std::string label;
        };

        struct PageStats
        {
            uint32_t count = 0;
            std::string labels;
        };

        enum class Phase
        {
            Idle,    // nothing to do in this main step
            Armed,   // waiting for the main step's view block
            InBlock, // inside it
            Taken,   // its pages are taken; the end of the main step compares and replays
        };

        // Main thread state.
        int g_enabled = -1;
        uint32_t g_presses = 0;
        uint32_t g_report = 0;
        uint32_t g_runsLeft = 0;
        uint32_t g_runsDone = 0;
        Phase g_phase = Phase::Idle;
        bool g_failed = false; // the write watch failed in this main step
        Clock::time_point g_blockStart;
        Clock::duration g_blockTime{};
        std::vector<uint32_t> g_rendered;
        std::vector<uint32_t> g_viewPages;
        std::vector<Label> g_labels;
        std::map<uint32_t, WordStats> g_words;
        std::map<uint32_t, PageStats> g_viewPageStats;
        std::map<uint32_t, PageStats> g_replayPageStats;
        uint64_t g_viewPageTotal = 0;

        // Takes the written pages of the scanned ranges (resetting them). Returns false when the OS
        // refused one range.
        bool TakePages(std::vector<uint32_t>& pages)
        {
            pages.clear();
            bool ok = true;
            for (const Range& range : kRanges)
                ok &= mem::TakeWrittenPages(range.begin, range.size, pages);
            return ok;
        }

        void AddLabel(uint32_t begin, uint32_t size, std::string name)
        {
            if (begin != 0)
                g_labels.push_back({ begin, begin + size, std::move(name) });
        }

        std::string Hex(uint32_t value)
        {
            char text[16];
            std::snprintf(text, sizeof(text), "%08X", value);
            return text;
        }

        // The labels of this view block: the time structure, the view object and every object the
        // display rendered with its matrix, blocks, additional matrices and modifiers.
        void BuildLabels()
        {
            g_labels.clear();
            AddLabel(TimeStructure(), jade::tim::kSize, "tim");
            AddLabel(mem::Load32(jade::kView), jade::view::kProbeSize, "view");
            std::sort(g_rendered.begin(), g_rendered.end());
            g_rendered.erase(std::unique(g_rendered.begin(), g_rendered.end()), g_rendered.end());
            for (uint32_t object : g_rendered)
            {
                std::string id = Hex(object);
                AddLabel(object, jade::object::kProbeSize, "obj " + id);
                AddLabel(mem::Load32(object + jade::object::kMatrix), jade::matrix::kSize, "matrix " + id);
                AddLabel(mem::Load32(object + jade::object::kBase), jade::base::kSize, "base " + id);
                uint32_t extended = mem::Load32(object + jade::object::kExtended);
                AddLabel(extended, jade::extended::kSize, "extended " + id);
                std::vector<uint32_t> bones = AdditionalMatrices(object);
                for (uint32_t i = 0; i < bones.size(); i++)
                    AddLabel(bones[i], jade::matrix::kSize, "bone " + id + "[" + std::to_string(i) + "]");
                uint32_t modifier = extended != 0 ? mem::Load32(extended + jade::extended::kModifiers) : 0;
                for (uint32_t n = 0; modifier != 0 && n < kMaxModifiers; n++)
                {
                    uint32_t face = mem::Load32(modifier + jade::modifier::kInterface);
                    std::string type = "mdf" + std::to_string(face != 0 ? mem::Load32(face) : 0);
                    AddLabel(modifier, jade::modifier::kSize, type + " " + id);
                    AddLabel(mem::Load32(modifier + jade::modifier::kData), kModifierDataHint, type + ".data " + id);
                    modifier = mem::Load32(modifier + jade::modifier::kNext);
                }
            }
            std::sort(g_labels.begin(), g_labels.end(), [](const Label& a, const Label& b) { return a.begin < b.begin; });
        }

        // The narrowest label holding address, as "name+offset", else the global or the range.
        std::string LabelOf(uint32_t address)
        {
            const Label* best = nullptr;
            auto it = std::upper_bound(g_labels.begin(), g_labels.end(), address,
                [](uint32_t a, const Label& label) { return a < label.begin; });
            while (it != g_labels.begin())
            {
                --it;
                if (address - it->begin >= kMaxLabelExtent)
                    break;
                if (address < it->end && (best == nullptr || it->end - it->begin < best->end - best->begin))
                    best = &*it;
            }
            char offset[16];
            if (best != nullptr)
            {
                std::snprintf(offset, sizeof(offset), "+%X", address - best->begin);
                return best->name + offset;
            }
            if (address >= mem::kImageBegin && address < mem::kImageEnd)
                return "global " + Hex(address);
            for (const Range& range : kRanges)
                if (address - range.begin < range.size)
                    return range.name;
            return "other";
        }

        // The distinct label names (without offsets) that touch a page, up to a few.
        std::string LabelsOfPage(uint32_t page)
        {
            std::set<std::string> names;
            for (const Label& label : g_labels)
                if (label.begin < page + kPage && label.end > page)
                {
                    names.insert(label.name.substr(0, label.name.find(' ')));
                    if (names.size() >= 4)
                        break;
                }
            if (names.empty())
            {
                std::string label = LabelOf(page);
                return label.rfind("global", 0) == 0 ? "global" : label;
            }
            std::string text;
            for (const std::string& name : names)
                text += (text.empty() ? "" : " ") + name;
            return text;
        }

        std::vector<uint8_t> Snapshot(const std::vector<uint32_t>& pages)
        {
            std::vector<uint8_t> copy(pages.size() * kPage);
            for (size_t i = 0; i < pages.size(); i++)
                std::memcpy(copy.data() + i * kPage, mem::Ptr(pages[i]), kPage);
            return copy;
        }

        struct Change
        {
            uint32_t address, before, after;
        };

        uint32_t LoadWord(const uint8_t* p)
        {
            uint32_t value;
            std::memcpy(&value, p, 4);
            return __builtin_bswap32(value);
        }

        // The words of pages that differ from the snapshot, with both values.
        std::vector<Change> Changed(const std::vector<uint32_t>& pages, const std::vector<uint8_t>& snapshot)
        {
            std::vector<Change> changes;
            for (size_t i = 0; i < pages.size(); i++)
            {
                const uint8_t* now = mem::Ptr(pages[i]);
                const uint8_t* then = snapshot.data() + i * kPage;
                if (std::memcmp(now, then, kPage) == 0)
                    continue;
                for (uint32_t offset = 0; offset < kPage; offset += 4)
                {
                    uint32_t before = LoadWord(then + offset);
                    uint32_t after = LoadWord(now + offset);
                    if (before != after)
                        changes.push_back({ pages[i] + offset, before, after });
                }
            }
            return changes;
        }

        // One run, at the end of a main step whose view block was taken. False when the series has
        // to stop.
        bool ProbeRun(PPCContext& ctx, uint8_t* base)
        {
            if (!WaitForPacketsDrawn(kPacketTimeout))
            {
                Log("purity: the render thread did not draw the packet in %lld ms; probe stopped",
                    (long long)kPacketTimeout.count());
                return false;
            }
            std::vector<uint32_t>& pages = g_viewPages;
            std::sort(pages.begin(), pages.end());
            g_viewPageTotal += pages.size();
            for (uint32_t page : pages)
            {
                PageStats& stats = g_viewPageStats[page];
                if (stats.count++ == 0)
                    stats.labels = LabelsOfPage(page);
            }

            // Noise window: other threads, for as long as the view block took.
            std::vector<uint8_t> snapshot = Snapshot(pages);
            std::this_thread::sleep_for(g_blockTime);
            for (const Change& change : Changed(pages, snapshot))
            {
                WordStats& stats = g_words[change.address];
                stats.noise++;
                if (stats.label.empty())
                    stats.label = LabelOf(change.address);
            }

            // Replay: the view block again, with the dt at 0 and without the sector switch. The render
            // thread is held before it starts the replayed packet, so what changed is the view block's
            // doing (and other threads'). The probe's own stores to the dt and the cut counter stay
            // outside the window of the write watch, and both are back when the words are compared
            // (the counter, a known per-display writer, is no longer reported).
            snapshot = Snapshot(pages);
            std::vector<uint32_t> replayPages;
            ExtraViewBlockState extraState;
            TakePages(replayPages);
            HoldDrawing();
            RunViewBlock(ctx, base, false, ViewBlockCalls::kOriginals);
            TakePages(replayPages);
            extraState.Restore();
            std::vector<Change> changes = Changed(pages, snapshot);
            ReleaseDrawing();
            if (!WaitForPacketsDrawn(kPacketTimeout))
                Log("purity: the render thread did not draw the replayed packet in %lld ms", (long long)kPacketTimeout.count());

            for (const Change& change : changes)
            {
                WordStats& stats = g_words[change.address];
                if (stats.replays++ == 0)
                {
                    stats.before = change.before;
                    stats.after = change.after;
                }
                if (stats.label.empty())
                    stats.label = LabelOf(change.address);
            }
            for (uint32_t page : replayPages)
            {
                if (std::binary_search(pages.begin(), pages.end(), page))
                    continue;
                PageStats& stats = g_replayPageStats[page];
                if (stats.count++ == 0)
                    stats.labels = LabelsOfPage(page);
            }
            g_runsDone++;
            return true;
        }

        // The label's class: its name up to the first space or offset.
        std::string ClassOf(const std::string& label)
        {
            size_t end = label.find_first_of(" +");
            return label.substr(0, end);
        }

        float AsFloat(uint32_t bits)
        {
            float value;
            std::memcpy(&value, &bits, 4);
            return value;
        }

        void Finish()
        {
            char name[32];
            std::snprintf(name, sizeof(name), "purity-%u.csv", g_report);
            std::FILE* file = OpenDiagnosticsFile(name);
            uint32_t steady = 0;
            std::map<std::string, uint32_t> classes;
            for (const auto& [address, stats] : g_words)
            {
                if (stats.replays == 0)
                    continue;
                classes[ClassOf(stats.label)]++;
                if (stats.replays == g_runsDone && stats.noise == 0)
                    steady++;
            }
            if (file != nullptr)
            {
                std::fputs("kind,address,label,replays,noise,before,after,before_float,after_float\n", file);
                for (const auto& [address, stats] : g_words)
                    std::fprintf(file, "word,%08X,%s,%u,%u,%08X,%08X,%g,%g\n", address, stats.label.c_str(), stats.replays,
                        stats.noise, stats.before, stats.after, AsFloat(stats.before), AsFloat(stats.after));
                for (const auto& [page, stats] : g_replayPageStats)
                    std::fprintf(file, "replay_page,%08X,%s,%u,0,,,,\n", page, stats.labels.c_str(), stats.count);
                for (const auto& [page, stats] : g_viewPageStats)
                    std::fprintf(file, "view_page,%08X,%s,%u,0,,,,\n", page, stats.labels.c_str(), stats.count);
                std::fclose(file);
            }

            Log("purity %u: %u runs, %.0f pages written per view block, %zu replay-only pages, %u words changed by "
                "every replay and by no noise window",
                g_report, g_runsDone, g_runsDone != 0 ? double(g_viewPageTotal) / g_runsDone : 0.0,
                g_replayPageStats.size(), steady);
            for (const auto& [label, count] : classes)
                Log("purity %u:   %-14s %u words changed by a replay", g_report, label.c_str(), count);
            uint32_t lines = 0;
            for (const auto& [address, stats] : g_words)
            {
                if (stats.replays != g_runsDone || stats.noise != 0)
                    continue;
                if (lines++ == kSteadyLogLines)
                {
                    Log("purity %u:   ... (the rest in %s)", g_report, name);
                    break;
                }
                Log("purity %u:   %08X %-36s %08X -> %08X (%g -> %g)", g_report, address, stats.label.c_str(),
                    stats.before, stats.after, AsFloat(stats.before), AsFloat(stats.after));
            }
        }
    }

    bool Enabled()
    {
        if (g_enabled < 0)
        {
            g_enabled = EnvironmentFlag("ROAN_PURITY_PROBE") ? 1 : 0;
            if (g_enabled == 1)
            {
                g_presses = host::DiagnosticKeyPresses();
                Log("purity: armed, press F8 in the game window to probe the next %u main steps", kRuns);
            }
        }
        return g_enabled == 1;
    }

    void MainStepStart()
    {
        // A press while a series runs is consumed without starting another one.
        uint32_t presses = host::DiagnosticKeyPresses();
        bool pressed = presses != g_presses;
        g_presses = presses;
        if (pressed && g_runsLeft == 0)
        {
            g_runsLeft = kRuns;
            g_runsDone = 0;
            g_report++;
            g_words.clear();
            g_viewPageStats.clear();
            g_replayPageStats.clear();
            g_viewPageTotal = 0;
            Log("purity %u: probing the next %u main steps", g_report, kRuns);
        }
        g_phase = g_runsLeft != 0 ? Phase::Armed : Phase::Idle;
    }

    void ViewBlockStart()
    {
        if (g_phase != Phase::Armed)
            return;
        TakePages(g_viewPages);
        g_rendered.clear();
        g_labels.clear();
        g_blockStart = Clock::now();
        g_phase = Phase::InBlock;
    }

    void DisplayEnd()
    {
        if (g_phase == Phase::InBlock)
            BuildLabels();
    }

    void ViewBlockEnd()
    {
        if (g_phase != Phase::InBlock)
            return;
        if (!TakePages(g_viewPages))
        {
            Log("purity: the OS refused the write watch; probe stopped");
            g_failed = true;
            g_phase = Phase::Idle;
            return;
        }
        g_blockTime = Clock::now() - g_blockStart;
        g_phase = Phase::Taken;
    }

    void NoteRendered(uint32_t object)
    {
        if (g_phase == Phase::InBlock)
            g_rendered.push_back(object);
    }

    void MainStepEnd(PPCContext& ctx, uint8_t* base)
    {
        // A main step without a view block (views off) does not count as a run.
        bool counted = g_phase == Phase::Taken;
        bool stop = g_failed || (counted && !ProbeRun(ctx, base));
        if (counted || stop)
        {
            g_runsLeft = stop ? 0 : g_runsLeft - 1;
            if (g_runsLeft == 0)
                Finish();
        }
        g_failed = false;
        g_phase = Phase::Idle;
    }
}
