#pragma once

#include "gpu/renderer/renderer.h"
#include "gpu/renderer/shader_cache.h"

#include <plume_render_interface.h>

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <memory>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace gpu
{
    // Everything a pipeline of a guest draw depends on. Plain data without padding, hashed, compared
    // and stored in the pipeline list as bytes: a change of its layout, or of what a field means,
    // needs a new kListVersion.
    struct PipelineKey
    {
        uint64_t vertexShader = 0;
        uint64_t pixelShader = 0; // 0: depth only
        uint64_t input = 0;       // VertexInput::id
        RenderState state;
        uint32_t samples = 0;
        uint32_t targets = 0; // bit 0: color, bit 1: depth-stencil

        bool operator==(const PipelineKey&) const = default;
    };

    // The pipelines of the guest draws, by key. The list file remembers every pipeline created, and
    // at the next start a few background threads create the listed ones again before the draws ask
    // for them, so the frame that first uses a pipeline seldom waits for the driver.
    //
    // Get runs on the owner thread; the background threads only create pipelines. A pipeline lives
    // as long as this object, which joins its threads before the device goes.
    class Pipelines
    {
    public:
        // listFile: the pipeline list, read now and appended to (empty: no list).
        Pipelines(plume::RenderDevice& device, const plume::RenderPipelineLayout& layout, std::filesystem::path listFile);
        ~Pipelines();

        // The pipeline of a key: ready, waited for while a background thread creates it, or created
        // here (then added to the list). Null when its shaders are missing or the driver refuses it
        // (logged once per key). A new key keeps a copy of input. Owner thread only.
        const plume::RenderPipeline* Get(const PipelineKey& key, const VertexInput& input);

    private:
        struct KeyHash
        {
            size_t operator()(const PipelineKey& key) const;
        };
        enum class State
        {
            Queued,   // listed, no thread has taken it yet
            Creating, // a thread is creating it
            Ready,    // created (the pipeline is null when that failed)
        };
        struct Entry
        {
            State state = State::Queued;
            std::shared_ptr<const VertexInput> input;
            std::unique_ptr<plume::RenderPipeline> pipeline;
        };

        // The shader of a cache key, or null when the cache has none (logged once per key). Any thread.
        const plume::RenderShader* Shader(uint64_t key);
        // Creates a pipeline. Any thread.
        std::unique_ptr<plume::RenderPipeline> Create(const PipelineKey& key, const VertexInput& input);
        void ReadList(const std::filesystem::path& listFile);
        void AppendToList(const PipelineKey& key, const VertexInput& input);
        void WorkerMain();

        plume::RenderDevice& device_;
        const plume::RenderPipelineLayout& layout_;
        ShaderCache cache_;

        std::mutex shaderMutex_;
        std::unordered_map<uint64_t, std::unique_ptr<plume::RenderShader>> shaders_;

        std::mutex mutex_; // the entries, the queue, stopping_, busy_ and reported_
        std::condition_variable created_;
        std::unordered_map<PipelineKey, Entry, KeyHash> entries_;
        std::deque<PipelineKey> queue_;
        bool stopping_ = false;
        unsigned busy_ = 0;     // threads creating a listed pipeline
        bool reported_ = false; // the time the list took is logged
        std::vector<std::thread> workers_;

        std::FILE* list_ = nullptr; // owner thread

        // What Get cost the draws (owner thread): pipelines created there and waits for a background
        // thread still creating one, logged per 10 s window that had any, and each one over 20 ms.
        struct DrawCost
        {
            uint32_t created = 0, waited = 0;
            double createdMs = 0, waitedMs = 0, maxMs = 0;
            std::chrono::steady_clock::time_point since{};
        };
        DrawCost cost_;
        void NoteDrawCost(bool created, double ms, const PipelineKey& key);
    };
}
