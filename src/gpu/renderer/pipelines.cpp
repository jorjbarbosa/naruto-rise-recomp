// Pipelines of the guest draws: translation of the guest state into pipeline descriptions, the
// pipeline list file and its background creation.

#include "gpu/renderer/pipelines.h"

#include "gpu/renderer/targets.h"
#include "log.h"

#include <plume_d3d12.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <system_error>

namespace gpu
{
    namespace
    {
        // The list file: a header, then one record per pipeline (the key's bytes, the element count,
        // the elements, the stride count, the strides).
        constexpr uint32_t kListMagic = 0x4C505852; // "RXPL"
        constexpr uint32_t kListVersion = 2;
        static_assert(sizeof(PipelineKey) == 3 * 8 + 8 * 4 + 2 * 4, "PipelineKey changed: bump kListVersion");
        static_assert(sizeof(VertexElement) == 8);
        constexpr uint32_t kMaxListed = 64; // elements or strides of one record

        // D3DDECLUSAGE, as the translated shaders name their inputs (the guest drops other usages).
        const char* const kSemantics[] = { "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD",
            "TANGENT", "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE" };

        constexpr plume::RenderFormat kElementFormats[] = { plume::RenderFormat::R32_UINT, plume::RenderFormat::R32G32_UINT,
            plume::RenderFormat::R32G32B32_UINT, plume::RenderFormat::R32G32B32A32_UINT };

        // RB_DEPTHCONTROL fields.
        constexpr uint32_t kStencilEnable = 0x1;
        constexpr uint32_t kDepthEnable = 0x2;
        constexpr uint32_t kDepthWrite = 0x4;
        constexpr uint32_t kBackFaceStencil = 0x80;
        // PA_SU_SC_MODE_CNTL.
        constexpr uint32_t kCullFront = 0x1;
        constexpr uint32_t kCullBack = 0x2;
        constexpr uint32_t kFrontClockwise = 0x4;

        // GPU compare functions (NEVER, LESS, EQUAL, LEQUAL, GREATER, NOTEQUAL, GEQUAL, ALWAYS)
        // and stencil operations (KEEP, ZERO, REPLACE, INCR_SAT, DECR_SAT, INVERT, INCR, DECR) are
        // in the order of Plume's, after its UNKNOWN.
        plume::RenderComparisonFunction Compare(uint32_t function)
        {
            return plume::RenderComparisonFunction(function + 1);
        }

        plume::RenderStencilFaceDesc StencilFace(uint32_t fields)
        {
            plume::RenderStencilFaceDesc face;
            face.compareFunction = Compare(fields & 7);
            face.failOp = plume::RenderStencilOp(((fields >> 3) & 7) + 1);
            face.passOp = plume::RenderStencilOp(((fields >> 6) & 7) + 1);
            face.depthFailOp = plume::RenderStencilOp(((fields >> 9) & 7) + 1);
            return face;
        }

        // GPU blend factors. The constant alpha factors (14, 15) take the blend color, which the
        // draw sets to its alpha when they are used. In the alpha factors, a color factor stands for
        // its alpha.
        plume::RenderBlend BlendFactor(uint32_t factor, bool alpha)
        {
            switch (factor)
            {
            case 0: return plume::RenderBlend::ZERO;
            case 1: return plume::RenderBlend::ONE;
            case 4: return alpha ? plume::RenderBlend::SRC_ALPHA : plume::RenderBlend::SRC_COLOR;
            case 5: return alpha ? plume::RenderBlend::INV_SRC_ALPHA : plume::RenderBlend::INV_SRC_COLOR;
            case 6: return plume::RenderBlend::SRC_ALPHA;
            case 7: return plume::RenderBlend::INV_SRC_ALPHA;
            case 8: return alpha ? plume::RenderBlend::DEST_ALPHA : plume::RenderBlend::DEST_COLOR;
            case 9: return alpha ? plume::RenderBlend::INV_DEST_ALPHA : plume::RenderBlend::INV_DEST_COLOR;
            case 10: return plume::RenderBlend::DEST_ALPHA;
            case 11: return plume::RenderBlend::INV_DEST_ALPHA;
            case 12: case 14: return plume::RenderBlend::BLEND_FACTOR;
            case 13: case 15: return plume::RenderBlend::INV_BLEND_FACTOR;
            case 16: return plume::RenderBlend::SRC_ALPHA_SAT;
            default:
                LOG_ONCE("gpu: blend factor %u is not covered; it blends as ONE", factor);
                return plume::RenderBlend::ONE;
            }
        }

        plume::RenderBlendOperation BlendOperation(uint32_t operation)
        {
            switch (operation)
            {
            case 0: return plume::RenderBlendOperation::ADD;
            case 1: return plume::RenderBlendOperation::SUBTRACT;
            case 2: return plume::RenderBlendOperation::MIN;
            case 3: return plume::RenderBlendOperation::MAX;
            case 4: return plume::RenderBlendOperation::REV_SUBTRACT;
            default:
                LOG_ONCE("gpu: blend operation %u is not covered; it blends as ADD", operation);
                return plume::RenderBlendOperation::ADD;
            }
        }

        // RB_BLENDCONTROL: color source, operation and destination at bits 0, 5 and 8; alpha at
        // 16, 21 and 24.
        plume::RenderBlendDesc Blend(uint32_t control, uint32_t colorMask)
        {
            plume::RenderBlendDesc blend;
            blend.srcBlend = BlendFactor(control & 0x1F, false);
            blend.blendOp = BlendOperation((control >> 5) & 7);
            blend.dstBlend = BlendFactor((control >> 8) & 0x1F, false);
            blend.srcBlendAlpha = BlendFactor((control >> 16) & 0x1F, true);
            blend.blendOpAlpha = BlendOperation((control >> 21) & 7);
            blend.dstBlendAlpha = BlendFactor((control >> 24) & 0x1F, true);
            // ONE, ADD, ZERO for both: no blending.
            blend.blendEnabled = (control & 0x1FFF1FFF) != 0x00010001;
            blend.renderTargetWriteMask = uint8_t(colorMask);
            return blend;
        }

        // Background threads: a few, leaving the game's threads most of the CPU.
        unsigned WorkerCount()
        {
            unsigned cores = std::max(std::thread::hardware_concurrency(), 1u);
            return std::clamp(cores / 4, 1u, 4u);
        }
    }

    size_t Pipelines::KeyHash::operator()(const PipelineKey& key) const
    {
        // FNV-1a over the bytes: the key has no padding.
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&key);
        uint64_t hash = 0xCBF29CE484222325ull;
        for (size_t i = 0; i < sizeof(key); i++)
            hash = (hash ^ bytes[i]) * 0x100000001B3ull;
        return size_t(hash);
    }

    Pipelines::Pipelines(plume::RenderDevice& device, const plume::RenderPipelineLayout& layout, std::filesystem::path listFile)
        : device_(device), layout_(layout)
    {
        if (listFile.empty())
            return;
        ReadList(listFile);
        if (!queue_.empty())
        {
            Log("gpu: %zu pipelines in the list, created in the background", queue_.size());
            for (unsigned i = 0; i < WorkerCount(); i++)
                workers_.emplace_back([this] { WorkerMain(); });
        }
    }

    Pipelines::~Pipelines()
    {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        for (std::thread& worker : workers_)
            worker.join();
        if (list_ != nullptr)
            std::fclose(list_);
    }

    void Pipelines::ReadList(const std::filesystem::path& listFile)
    {
        if (std::FILE* file = _wfopen(listFile.c_str(), L"rb"))
        {
            uint32_t header[2] = {};
            bool valid = std::fread(header, sizeof(header), 1, file) == 1 && header[0] == kListMagic &&
                header[1] == kListVersion;
            while (valid)
            {
                PipelineKey key;
                uint32_t count = 0;
                if (std::fread(&key, sizeof(key), 1, file) != 1 || std::fread(&count, 4, 1, file) != 1 || count > kMaxListed)
                    break;
                auto input = std::make_shared<VertexInput>();
                input->elements.resize(count);
                if (count != 0 && std::fread(input->elements.data(), sizeof(VertexElement), count, file) != count)
                    break;
                if (std::fread(&count, 4, 1, file) != 1 || count > kMaxListed)
                    break;
                input->strides.resize(count);
                if (count != 0 && std::fread(input->strides.data(), 4, count, file) != count)
                    break;
                input->id = key.input;
                // A key whose shaders the cache lost (another shader database) is left out.
                if (cache_.Dxil(key.vertexShader).empty() || (key.pixelShader != 0 && cache_.Dxil(key.pixelShader).empty()))
                    continue;
                if (entries_.try_emplace(key, Entry{ State::Queued, std::move(input), nullptr }).second)
                    queue_.push_back(key);
            }
            std::fclose(file);
        }

        // The list is written again with the records kept: a list of another version, a damaged
        // tail, duplicates and keys the cache lost all go.
        std::error_code error;
        std::filesystem::create_directories(listFile.parent_path(), error);
        list_ = _wfopen(listFile.c_str(), L"wb");
        if (list_ == nullptr)
        {
            Log("gpu: cannot write the pipeline list %s; pipelines are not remembered", listFile.string().c_str());
            return;
        }
        const uint32_t header[2] = { kListMagic, kListVersion };
        std::fwrite(header, sizeof(header), 1, list_);
        for (const PipelineKey& key : queue_)
            AppendToList(key, *entries_.at(key).input);
        std::fflush(list_);
    }

    void Pipelines::AppendToList(const PipelineKey& key, const VertexInput& input)
    {
        if (list_ == nullptr)
            return;
        uint32_t elements = uint32_t(input.elements.size());
        uint32_t strides = uint32_t(input.strides.size());
        if (elements > kMaxListed || strides > kMaxListed)
            return;
        std::fwrite(&key, sizeof(key), 1, list_);
        std::fwrite(&elements, 4, 1, list_);
        std::fwrite(input.elements.data(), sizeof(VertexElement), elements, list_);
        std::fwrite(&strides, 4, 1, list_);
        std::fwrite(input.strides.data(), 4, strides, list_);
        // Flushed per record: the process may end without running destructors.
        std::fflush(list_);
    }

    const plume::RenderPipeline* Pipelines::Get(const PipelineKey& key, const std::shared_ptr<const VertexInput>& input)
    {
        std::unique_lock lock(mutex_);
        auto [it, inserted] = entries_.try_emplace(key, Entry{ State::Creating, input, nullptr });
        Entry& entry = it->second;
        if (!inserted)
        {
            if (entry.state == State::Creating)
                created_.wait(lock, [&] { return entry.state == State::Ready; });
            if (entry.state == State::Ready)
                return entry.pipeline.get();
            // Listed but not taken yet: created here instead (the threads skip it).
            entry.state = State::Creating;
        }
        std::shared_ptr<const VertexInput> entryInput = entry.input;
        lock.unlock();

        std::unique_ptr<plume::RenderPipeline> pipeline = Create(key, *entryInput);
        if (inserted && pipeline)
            AppendToList(key, *entryInput);

        lock.lock();
        entry.pipeline = std::move(pipeline);
        entry.state = State::Ready;
        return entry.pipeline.get();
    }

    void Pipelines::WorkerMain()
    {
        SetThreadDescription(GetCurrentThread(), L"gpu pipelines");
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        auto start = std::chrono::steady_clock::now();
        std::unique_lock lock(mutex_);
        for (;;)
        {
            if (stopping_)
                return;
            if (queue_.empty())
            {
                // The thread that finishes the last listed pipeline reports how long the list took.
                if (busy_ == 0 && !reported_)
                {
                    reported_ = true;
                    std::chrono::duration<double> seconds = std::chrono::steady_clock::now() - start;
                    Log("gpu: the pipeline list was created in %.1f s", seconds.count());
                }
                return;
            }
            PipelineKey key = queue_.front();
            queue_.pop_front();
            Entry& entry = entries_.at(key);
            if (entry.state != State::Queued)
                continue;
            entry.state = State::Creating;
            busy_++;
            std::shared_ptr<const VertexInput> input = entry.input;
            lock.unlock();
            std::unique_ptr<plume::RenderPipeline> pipeline = Create(key, *input);
            lock.lock();
            busy_--;
            entry.pipeline = std::move(pipeline);
            entry.state = State::Ready;
            created_.notify_all();
        }
    }

    const plume::RenderShader* Pipelines::Shader(uint64_t key)
    {
        std::lock_guard lock(shaderMutex_);
        auto it = shaders_.find(key);
        if (it != shaders_.end())
            return it->second.get();
        std::span<const uint8_t> dxil = cache_.Dxil(key);
        std::unique_ptr<plume::RenderShader> shader;
        if (!dxil.empty())
            shader = device_.createShader(dxil.data(), dxil.size(), "main", plume::RenderShaderFormat::DXIL);
        else
            Log("gpu: shader %016llX is not in the shader cache; its draws are skipped", (unsigned long long)key);
        return shaders_.emplace(key, std::move(shader)).first->second.get();
    }

    std::unique_ptr<plume::RenderPipeline> Pipelines::Create(const PipelineKey& key, const VertexInput& input)
    {
        const plume::RenderShader* vertexShader = Shader(key.vertexShader);
        const plume::RenderShader* pixelShader = key.pixelShader != 0 ? Shader(key.pixelShader) : nullptr;
        if (vertexShader == nullptr || (key.pixelShader != 0 && pixelShader == nullptr))
            return nullptr;

        std::vector<plume::RenderInputSlot> slots;
        for (uint32_t i = 0; i < input.strides.size(); i++)
            slots.emplace_back(i, input.strides[i]);
        std::vector<plume::RenderInputElement> elements;
        for (uint32_t i = 0; i < input.elements.size(); i++)
        {
            const VertexElement& element = input.elements[i];
            if (element.usage >= std::size(kSemantics) || element.dwords == 0 || element.dwords > 4 ||
                element.stream >= input.strides.size())
                return nullptr;
            elements.emplace_back(kSemantics[element.usage], element.usageIndex, i, kElementFormats[element.dwords - 1],
                element.stream, element.offset);
        }

        const RenderState& state = key.state;
        plume::RenderGraphicsPipelineDesc desc;
        desc.pipelineLayout = &layout_;
        desc.vertexShader = vertexShader;
        desc.pixelShader = pixelShader;
        desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
        desc.inputSlots = slots.data();
        desc.inputSlotsCount = uint32_t(slots.size());
        desc.inputElements = elements.data();
        desc.inputElementsCount = uint32_t(elements.size());
        desc.multisampling.sampleCount = plume::RenderSampleCounts(key.samples);
        desc.alphaToCoverageEnabled = state.alphaToCoverage != 0;
        desc.depthClipEnabled = true;
        if ((state.cullControl & kCullFront) != 0)
            desc.cullMode = plume::RenderCullMode::FRONT;
        else if ((state.cullControl & kCullBack) != 0)
            desc.cullMode = plume::RenderCullMode::BACK;
        else
            desc.cullMode = plume::RenderCullMode::NONE;
        desc.frontFace = (state.cullControl & kFrontClockwise) != 0 ? plume::RenderFrontFace::CLOCKWISE
                                                                      : plume::RenderFrontFace::COUNTER_CLOCKWISE;
        desc.depthBias = state.depthBias;
        desc.slopeScaledDepthBias = state.depthBiasSlope;
        if ((key.targets & 1) != 0)
        {
            desc.renderTargetFormat[0] = Targets::kColorFormat;
            desc.renderTargetBlend[0] = Blend(state.blendControl, state.colorMask);
            desc.renderTargetCount = 1;
        }
        if ((key.targets & 2) != 0)
        {
            desc.depthTargetFormat = Targets::kDepthFormat;
            desc.depthEnabled = (state.depthControl & kDepthEnable) != 0;
            desc.depthWriteEnabled = (state.depthControl & kDepthWrite) != 0;
            desc.depthFunction = Compare((state.depthControl >> 4) & 7);
            desc.stencilEnabled = (state.depthControl & kStencilEnable) != 0;
            if (desc.stencilEnabled)
            {
                // The reference is set per draw (Draws::Draw); every pipeline keeps 0.
                desc.stencilReadMask = (state.stencilMasks >> 8) & 0xFF;
                desc.stencilWriteMask = (state.stencilMasks >> 16) & 0xFF;
                desc.stencilFrontFace = StencilFace(state.depthControl >> 8);
                desc.stencilBackFace = (state.depthControl & kBackFaceStencil) != 0
                    ? StencilFace(state.depthControl >> 20)
                    : desc.stencilFrontFace;
            }
        }
        std::unique_ptr<plume::RenderPipeline> pipeline = device_.createGraphicsPipeline(desc);
        if (static_cast<plume::D3D12GraphicsPipeline*>(pipeline.get())->d3d == nullptr)
        {
            Log("gpu: cannot create the pipeline of shaders %016llX and %016llX; their draws are skipped",
                (unsigned long long)key.vertexShader, (unsigned long long)key.pixelShader);
            return nullptr;
        }
        return pipeline;
    }
}
