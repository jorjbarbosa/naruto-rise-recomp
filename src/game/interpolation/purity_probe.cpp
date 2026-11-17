// The display purity probe (purity_probe.h).

#include "game/interpolation/purity_probe.h"

#include "game/addresses.h"
#include "game/interpolation/diagnostics.h"
#include "game/interpolation/matrices.h"
#include "game/interpolation/transforms.h"
#include "game/interpolation/view_block.h"
#include "game/interpolation/view_clock.h"
#include "game/interpolation/world.h"
#include "game/render_packets.h"
#include "host/window.h"
#include "log.h"
#include "memory.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
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

        constexpr uint32_t kRuns = 8; // main steps (replay) or ticks (carry) probed per F8 press
        constexpr std::chrono::milliseconds kPacketTimeout{ 500 };
        constexpr std::chrono::milliseconds kMaxNoiseWindow{ 100 };
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
            uint32_t replays = 0; // replays (carry: intervals with the transforms) that changed the word
            uint32_t noise = 0;   // noise windows that changed it
            uint32_t off = 0;     // carry: intervals with the transforms suspended that changed it
            uint32_t before = 0, after = 0; // the first change counted in replays
            std::string label;
        };

        struct PageStats
        {
            uint32_t count = 0;
            std::string labels;
        };

        enum class Mode
        {
            kUnread,
            kOff,
            kReplay,
            kCarry,
        };

        // The replay's main step, and the carry's tick and interval.
        enum class Phase
        {
            Idle,     // nothing to do in this main step
            Armed,    // replay: waiting for the main step's view block; carry: for a tick
            Tick,     // carry: this iteration runs the logic, waiting for its view block
            InBlock,  // inside the view block
            Taken,    // its pages are taken; the end of the main step compares and replays (replay) or
                      // takes the tick's state (carry)
            Interval, // carry: between the tick and the next one
        };

        // Main thread state.
        Mode g_mode = Mode::kUnread;
        uint32_t g_presses = 0;
        uint32_t g_report = 0;
        uint32_t g_runsLeft = 0;
        uint32_t g_runsDone = 0;
        uint32_t g_offRunsDone = 0; // carry: intervals with the transforms suspended
        Phase g_phase = Phase::Idle;
        std::vector<uint8_t> g_tickState; // carry: the tick's pages when its main step ended
        Clock::time_point g_tickEnd;
        std::vector<uint32_t> g_scratchPages;
        std::set<uint32_t> g_intervalPages; // carry: written between the ticks outside the tick's pages
        uint32_t g_intervalBlocks = 0;      // carry: view blocks between the ticks
        bool g_intervalOff = false;         // carry: the interval runs with the transforms suspended
        // carry: what the second half suspends (ROAN_CARRY_PROBE=1: everything, =objects: the objects).
        transforms::Suspended g_carryHolds = transforms::Suspended::kAll;
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

        // The view block's pages, sorted, into the per-page counts.
        void CountViewPages(std::vector<uint32_t>& pages)
        {
            std::sort(pages.begin(), pages.end());
            g_viewPageTotal += pages.size();
            for (uint32_t page : pages)
            {
                PageStats& stats = g_viewPageStats[page];
                if (stats.count++ == 0)
                    stats.labels = LabelsOfPage(page);
            }
        }

        // The words of pages other threads change in a window of the given length.
        void NoiseWindow(const std::vector<uint32_t>& pages, Clock::duration length)
        {
            std::vector<uint8_t> snapshot = Snapshot(pages);
            std::this_thread::sleep_for(length);
            for (const Change& change : Changed(pages, snapshot))
            {
                WordStats& stats = g_words[change.address];
                stats.noise++;
                if (stats.label.empty())
                    stats.label = LabelOf(change.address);
            }
        }

        // A run's changes into the per-word counts: a replay or an interval with the transforms, or
        // (off) an interval with the transforms suspended.
        void CountChanges(const std::vector<Change>& changes, bool off = false)
        {
            for (const Change& change : changes)
            {
                WordStats& stats = g_words[change.address];
                if (off)
                    stats.off++;
                else if (stats.replays++ == 0)
                {
                    stats.before = change.before;
                    stats.after = change.after;
                }
                if (stats.label.empty())
                    stats.label = LabelOf(change.address);
            }
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
            CountViewPages(pages);

            // Noise window: other threads, for as long as the view block took.
            NoiseWindow(pages, g_blockTime);

            // Replay: the view block again, with the dt at 0 and without the sector switch. The render
            // thread is held before it starts the replayed packet, so what changed is the view block's
            // doing (and other threads'). The probe's own stores to the dt and the cut counter stay
            // outside the window of the write watch, and both are back when the words are compared
            // (the counter, a known per-display writer, is no longer reported).
            std::vector<uint8_t> snapshot = Snapshot(pages);
            std::vector<uint32_t> replayPages;
            ExtraViewBlockState extraState(ExtraDt::kZero);
            view_clock::ReplayScope replayTime; // under the gate: the phases at the image time replayed
            transforms::ReplayScope replayTransforms; // ... and the objects and camera it drew
            TakePages(replayPages);
            HoldDrawing();
            RunViewBlock(ctx, base, false, ViewBlockCalls::kOriginals);
            TakePages(replayPages);
            replayTransforms.Restore();
            extraState.Restore();
            std::vector<Change> changes = Changed(pages, snapshot);
            ReleaseDrawing();
            if (!WaitForPacketsDrawn(kPacketTimeout))
                Log("purity: the render thread did not draw the replayed packet in %lld ms", (long long)kPacketTimeout.count());

            CountChanges(changes);
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

        // The carry's run, when the iteration after the tick's interval starts (before its logic): the
        // tick's pages against their state when the tick's main step ended, then a noise window as long
        // as the interval (capped). off: the interval ran with the transforms suspended.
        void CarryRun(bool off)
        {
            Clock::duration interval = Clock::now() - g_tickEnd;
            CountChanges(Changed(g_viewPages, g_tickState), off);
            for (uint32_t page : g_intervalPages)
            {
                PageStats& stats = g_replayPageStats[page];
                if (stats.count++ == 0)
                    stats.labels = LabelsOfPage(page);
            }
            NoiseWindow(g_viewPages, std::min<Clock::duration>(interval, kMaxNoiseWindow));
            (off ? g_offRunsDone : g_runsDone)++;
        }

        // The label's class: its name up to the first space or offset.
        std::string ClassOf(const std::string& label)
        {
            size_t end = label.find_first_of(" +");
            return label.substr(0, end);
        }

        void Finish()
        {
            // The words of the replay and the carry: a run is a replay or an interval.
            const char* probe = g_mode == Mode::kCarry ? "carry" : "purity";
            const char* run = g_mode == Mode::kCarry ? "interval" : "replay";
            char name[32];
            std::snprintf(name, sizeof(name), "%s-%u.csv", probe, g_report);
            std::FILE* file = OpenDiagnosticsFile(name);
            // Steady: changed by every replay (carry: every interval with the transforms, and by no
            // interval without them), by no noise window.
            auto steadyWord = [](const WordStats& stats) {
                return stats.replays == g_runsDone && stats.noise == 0 && stats.off == 0;
            };
            uint32_t steady = 0, perFrame = 0;
            std::map<std::string, uint32_t> classes;
            for (const auto& [address, stats] : g_words)
            {
                if (stats.replays == 0)
                    continue;
                if (steadyWord(stats))
                {
                    steady++;
                    classes[ClassOf(stats.label)]++;
                }
                else if (g_offRunsDone != 0 && stats.replays == g_runsDone && stats.off == g_offRunsDone && stats.noise == 0)
                    perFrame++;
            }
            if (file != nullptr)
            {
                std::fputs("kind,address,label,replays,noise,before,after,before_float,after_float,off\n", file);
                for (const auto& [address, stats] : g_words)
                    std::fprintf(file, "word,%08X,%s,%u,%u,%08X,%08X,%g,%g,%u\n", address, stats.label.c_str(),
                        stats.replays, stats.noise, stats.before, stats.after, matrices::Float(stats.before), matrices::Float(stats.after),
                        stats.off);
                for (const auto& [page, stats] : g_replayPageStats)
                    std::fprintf(file, "replay_page,%08X,%s,%u,0,,,,,\n", page, stats.labels.c_str(), stats.count);
                for (const auto& [page, stats] : g_viewPageStats)
                    std::fprintf(file, "view_page,%08X,%s,%u,0,,,,,\n", page, stats.labels.c_str(), stats.count);
                std::fclose(file);
            }

            uint32_t runs = g_runsDone + g_offRunsDone;
            Log("%s %u: %u runs (%u with the transforms suspended), %.0f pages written per view block, %zu %s-only "
                "pages, %u words changed by every %s%s and by no noise window, %u by every interval of both",
                probe, g_report, runs, g_offRunsDone, runs != 0 ? double(g_viewPageTotal) / runs : 0.0,
                g_replayPageStats.size(), run, steady, run,
                g_offRunsDone != 0 ? " with the transforms, by none without them," : "", perFrame);
            for (const auto& [label, count] : classes)
                Log("%s %u:   %-14s %u of them", probe, g_report, label.c_str(), count);
            uint32_t lines = 0;
            for (const auto& [address, stats] : g_words)
            {
                if (!steadyWord(stats))
                    continue;
                if (lines++ == kSteadyLogLines)
                {
                    Log("%s %u:   ... (the rest in %s)", probe, g_report, name);
                    break;
                }
                Log("%s %u:   %08X %-36s %08X -> %08X (%g -> %g)", probe, g_report, address, stats.label.c_str(),
                    stats.before, stats.after, matrices::Float(stats.before), matrices::Float(stats.after));
            }
        }

        // An F8 press starts a series; a press while one runs is consumed without starting another.
        bool StartSeries(const char* what)
        {
            uint32_t presses = host::DiagnosticKeyPresses();
            bool pressed = presses != g_presses;
            g_presses = presses;
            if (!pressed || g_runsLeft != 0)
                return false;
            // The carry: kRuns intervals with the transforms, then kRuns with them suspended.
            g_runsLeft = g_mode == Mode::kCarry ? 2 * kRuns : kRuns;
            g_runsDone = 0;
            g_offRunsDone = 0;
            g_report++;
            g_words.clear();
            g_viewPageStats.clear();
            g_replayPageStats.clear();
            g_viewPageTotal = 0;
            Log("%s %u: probing the next %u %s", g_mode == Mode::kCarry ? "carry" : "purity", g_report, kRuns, what);
            return true;
        }

        void BlockStarts()
        {
            TakePages(g_viewPages);
            g_rendered.clear();
            g_labels.clear();
            g_blockStart = Clock::now();
            g_phase = Phase::InBlock;
        }
    }

    bool Enabled()
    {
        if (g_mode == Mode::kUnread)
        {
            bool replay = EnvironmentFlag("ROAN_PURITY_PROBE");
            const char* carryValue = std::getenv("ROAN_CARRY_PROBE");
            bool carryObjects = carryValue != nullptr && std::strcmp(carryValue, "objects") == 0;
            bool carry = EnvironmentFlag("ROAN_CARRY_PROBE") || carryObjects;
            if (carryObjects)
                g_carryHolds = transforms::Suspended::kObjects;
            g_mode = replay ? Mode::kReplay : carry ? Mode::kCarry : Mode::kOff;
            if (replay && carry)
                Log("purity: ROAN_PURITY_PROBE and ROAN_CARRY_PROBE both set; the replay runs");
            if (g_mode != Mode::kOff)
            {
                g_presses = host::DiagnosticKeyPresses();
                if (g_mode == Mode::kReplay)
                    Log("purity: armed, press F8 in the game window to probe the next %u main steps", kRuns);
                else
                    Log("carry: armed, press F8 in the game window to probe the next %u ticks, then %u with %s "
                        "suspended", kRuns, kRuns, carryObjects ? "the objects" : "the transforms");
            }
        }
        return g_mode == Mode::kReplay || g_mode == Mode::kCarry;
    }

    void IterationStart(bool logic)
    {
        if (g_mode != Mode::kCarry)
            return;
        if (StartSeries("ticks"))
            g_phase = Phase::Armed;
        if (!logic)
        {
            // A tick whose main step had no view block (views off): its state was not taken, and a view
            // block between the ticks is not the tick's. The next tick.
            if (g_phase == Phase::Tick)
                g_phase = Phase::Armed;
            return;
        }
        // An interval without a view block (a late frame after the noise window, the gate off) leaves
        // the tick's state as the console would: not a run.
        if (g_phase == Phase::Interval && g_intervalBlocks == 0)
            g_phase = Phase::Armed;
        if (g_phase == Phase::Interval)
        {
            CarryRun(g_intervalOff);
            if (--g_runsLeft == 0)
            {
                transforms::Suspend(transforms::Suspended::kNone);
                Finish();
                g_phase = Phase::Idle;
                return;
            }
            g_phase = Phase::Armed;
        }
        // The second half of the series runs from here with the transforms suspended (the gate alone).
        if (g_phase == Phase::Armed || g_phase == Phase::Tick)
        {
            g_phase = Phase::Tick;
            if (g_runsDone == kRuns)
                transforms::Suspend(g_carryHolds);
        }
    }

    void MainStepStart()
    {
        if (g_mode != Mode::kReplay)
            return;
        StartSeries("main steps");
        g_phase = g_runsLeft != 0 ? Phase::Armed : Phase::Idle;
    }

    void ViewBlockStart()
    {
        if (g_mode == Mode::kReplay ? g_phase == Phase::Armed : g_phase == Phase::Tick)
            BlockStarts();
        else if (g_mode == Mode::kCarry && g_phase == Phase::Interval)
            TakePages(g_scratchPages); // from here, the pages of a view block between the ticks
    }

    void DisplayEnd()
    {
        if (g_phase == Phase::InBlock)
            BuildLabels();
    }

    void ViewBlockEnd()
    {
        if (g_mode == Mode::kCarry && g_phase == Phase::Interval)
        {
            g_intervalBlocks++;
            if (!TakePages(g_scratchPages))
            {
                Log("carry: the OS refused the write watch; probe stopped");
                g_failed = true;
                return;
            }
            for (uint32_t page : g_scratchPages)
                if (!std::binary_search(g_viewPages.begin(), g_viewPages.end(), page))
                    g_intervalPages.insert(page);
            return;
        }
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
        if (g_mode == Mode::kCarry)
        {
            if (g_failed)
            {
                g_runsLeft = 0;
                transforms::Suspend(transforms::Suspended::kNone);
                Finish();
                g_failed = false;
                g_phase = Phase::Idle;
            }
            else if (g_phase == Phase::Taken)
            {
                // The tick's state as its logic, view block and end of frame left it.
                CountViewPages(g_viewPages);
                g_tickState = Snapshot(g_viewPages);
                g_intervalPages.clear();
                g_intervalBlocks = 0;
                g_intervalOff = g_runsDone == kRuns;
                g_tickEnd = Clock::now();
                g_phase = Phase::Interval;
            }
            return;
        }

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
