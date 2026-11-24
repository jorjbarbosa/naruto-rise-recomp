// The replay of a recorded frame on either graphics API, written out for comparison.

#include "gpu/replay.h"

#include "gpu/renderer/backend.h"
#include "gpu/renderer/recording.h"
#include "gpu/renderer/renderer.h"
#include "gpu/settings.h"
#include "host/files.h"
#include "host/paths.h"
#include "host/window.h"
#include "log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <vector>

namespace gpu
{
    namespace
    {
        // A binary PPM of the image's RGB.
        bool WritePpm(const std::filesystem::path& path, const ResolvedImage& image)
        {
            std::FILE* file = host::OpenFile(path, "wb");
            if (file == nullptr)
                return false;
            std::fprintf(file, "P6\n%u %u\n255\n", image.width, image.height);
            std::vector<uint8_t> rgb(size_t(image.width) * image.height * 3);
            for (size_t i = 0, n = size_t(image.width) * image.height; i < n; i++)
                std::copy_n(image.rgba.data() + i * 4, 3, rgb.data() + i * 3);
            bool written = std::fwrite(rgb.data(), 1, rgb.size(), file) == rgb.size();
            return std::fclose(file) == 0 && written;
        }

        // The range the replayed draw names for one stage: the last one again when the recorded draw
        // shared its range with the draw before it, otherwise a new one with the recorded constants.
        struct StageConstants
        {
            uint64_t recorded = UINT64_MAX; // position in the recording run
            ConstantRange range;
        };

        bool Constants(Renderer& renderer, ShaderStage stage, StageConstants& last, const ConstantRange& recorded,
            const std::vector<uint8_t>& bytes, ConstantRange& range)
        {
            if (recorded.position != last.recorded)
            {
                std::optional<ConstantSpace> space = renderer.ReserveConstants(stage, uint32_t(bytes.size()));
                if (!space)
                    return false;
                std::memcpy(space->data, bytes.data(), bytes.size());
                last = { recorded.position, space->range };
            }
            range = last.range;
            return true;
        }

        // Every surface the frame draws into, clears or resolves from, once.
        std::vector<SurfaceDesc> FrameSurfaces(const RecordedFrame& frame)
        {
            std::vector<SurfaceDesc> surfaces;
            auto add = [&](const SurfaceDesc& surface) {
                if (surface.key != 0 && std::find(surfaces.begin(), surfaces.end(), surface) == surfaces.end())
                    surfaces.push_back(surface);
            };
            for (const RecordedOp& op : frame.ops)
            {
                if (op.kind == RecordedOp::Kind::Draw)
                {
                    add(op.draw->op.color);
                    add(op.draw->op.depth);
                }
                else if (op.kind == RecordedOp::Kind::Clear)
                {
                    for (const SurfaceDesc& color : op.clear.colors)
                        add(color);
                    add(op.clear.depth);
                }
                else if (op.kind == RecordedOp::Kind::Resolve)
                {
                    add(op.resolve.source);
                }
            }
            return surfaces;
        }

        // Posts the frame's operations; the destinations of its resolves go to resolves. False when
        // the renderer stopped taking them.
        bool PostOperations(Renderer& renderer, const RecordedFrame& frame, std::vector<uint32_t>& resolves)
        {
            StageConstants vertexConstants;
            StageConstants pixelConstants;
            for (const RecordedOp& op : frame.ops)
            {
                switch (op.kind)
                {
                case RecordedOp::Kind::Clear: renderer.Clear(op.clear); break;
                case RecordedOp::Kind::Resolve:
                    renderer.Resolve(op.resolve);
                    resolves.push_back(op.resolve.destAddress);
                    break;
                case RecordedOp::Kind::Draw:
                {
                    const RecordedDraw& recorded = *op.draw;
                    std::unique_ptr<DrawOp> draw = renderer.NewDraw();
                    *draw = recorded.op;
                    if (!Constants(renderer, ShaderStage::Vertex, vertexConstants, recorded.op.vertexConstants,
                            recorded.vertexConstants, draw->vertexConstants) ||
                        !Constants(renderer, ShaderStage::Pixel, pixelConstants, recorded.op.pixelConstants,
                            recorded.pixelConstants, draw->pixelConstants))
                        return false;
                    renderer.Draw(std::move(draw));
                    break;
                }
                case RecordedOp::Kind::UploadTexture:
                    renderer.UploadTexture(std::make_unique<TextureUpload>(*op.texture));
                    break;
                case RecordedOp::Kind::UploadBuffer: renderer.UploadBuffer(std::make_unique<BufferUpload>(*op.buffer)); break;
                case RecordedOp::Kind::ReleaseSurface: renderer.ReleaseSurface(op.first); break;
                case RecordedOp::Kind::ReleaseTexture: renderer.ReleaseTexture(op.first, op.second); break;
                case RecordedOp::Kind::ReleaseMemory: renderer.ReleaseMemory(op.first, op.second); break;
                case RecordedOp::Kind::ReleaseBuffers: renderer.ReleaseBuffers(op.first, op.second); break;
                case RecordedOp::Kind::ReleaseBuffer: renderer.ReleaseBuffer(op.key); break;
                }
            }
            return true;
        }
    }

    int RunReplay(const std::filesystem::path& file, const std::filesystem::path& output)
    {
        RecordedFrame frame;
        if (!ReadRecordedFrame(file, frame))
            return 1;
        Settings settings = Settings::FromStore();
        if (!settings.renderer)
        {
            Log("replay: error: ROAN_GPU_API=none, and a replay needs a renderer");
            return 1;
        }
        settings.recordFrames = false;
        if (std::filesystem::path executable = host::ExecutableDirectory(); !executable.empty())
            settings.SetCacheDirectory(executable / "cache");
        std::unique_ptr<Renderer> renderer =
            Renderer::Create(host::MainWindowHandle(), settings, [](uint32_t) {}, [] {});
        if (!renderer)
            return 1;
        Log("replay: %s on %s: %zu textures, %zu buffers, %zu operations", file.string().c_str(), ApiName(settings.api),
            frame.textures.size(), frame.buffers.size(), frame.ops.size());

        for (const auto& texture : frame.textures)
            renderer->UploadTexture(std::make_unique<TextureUpload>(*texture));
        for (const auto& buffer : frame.buffers)
            renderer->UploadBuffer(std::make_unique<BufferUpload>(*buffer));
        for (const SurfaceDesc& surface : FrameSurfaces(frame))
        {
            ClearOp clear;
            if (surface.depth)
            {
                clear.depth = surface;
                clear.clearDepth = true;
                clear.clearStencil = true;
                clear.depthValue = 1.0f;
            }
            else
            {
                clear.colors[0] = surface;
            }
            renderer->Clear(clear);
        }
        std::vector<uint32_t> resolves;
        if (!PostOperations(*renderer, frame, resolves))
            return 1;

        std::error_code error;
        std::filesystem::create_directories(output, error);
        ResolvedImage image;
        if (!renderer->ReadResolved(frame.frontAddress, image) || !WritePpm(output / "front.ppm", image))
        {
            Log("replay: error: the front buffer at %08X cannot be read back or written", frame.frontAddress);
            return 1;
        }
        std::sort(resolves.begin(), resolves.end());
        resolves.erase(std::unique(resolves.begin(), resolves.end()), resolves.end());
        size_t written = 0;
        for (uint32_t address : resolves)
        {
            char name[32];
            std::snprintf(name, sizeof(name), "resolve-%08X.ppm", address);
            // Depth resolves read back nothing.
            if (address != frame.frontAddress && renderer->ReadResolved(address, image) && WritePpm(output / name, image))
                written++;
        }
        Log("replay: front buffer and %zu color resolves written to %s", written, output.string().c_str());
        return 0;
    }
}
