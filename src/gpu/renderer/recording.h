#pragma once

// Recordings of one frame of renderer operations, so that the same frame can be replayed on either
// graphics API and the results compared (gpu/replay.h, scripts/compare-gpu.ps1). A recording holds
// every texture and buffer upload still live when the frame starts, and the operations the frame
// posted, in order, with the constants its draws read. What earlier frames left in the surfaces and
// in the resolved textures is not in it: a replay starts those from nothing.

#include "gpu/renderer/renderer.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace gpu
{
    // A draw of a recording: the DrawOp as posted (input points into RecordedFrame::inputs; the
    // constant ranges keep their positions in the recording run, so that draws sharing a range can be
    // told) and the constants it read.
    struct RecordedDraw
    {
        DrawOp op;
        std::vector<uint8_t> vertexConstants;
        std::vector<uint8_t> pixelConstants;
    };

    // An operation of a recorded frame, as posted to the Renderer.
    struct RecordedOp
    {
        enum class Kind : uint8_t
        {
            Clear,
            Resolve,
            Draw,
            UploadTexture,
            UploadBuffer,
            ReleaseSurface, // first: the key
            ReleaseTexture, // first, second: address, header
            ReleaseMemory,  // first, second: GPU addresses [begin, end)
            ReleaseBuffers, // first, second: GPU addresses [begin, end)
            ReleaseBuffer,  // key
        };

        Kind kind = Kind::Clear;
        ClearOp clear;
        ResolveOp resolve;
        std::shared_ptr<const RecordedDraw> draw;
        std::shared_ptr<const TextureUpload> texture;
        std::shared_ptr<const BufferUpload> buffer;
        uint32_t first = 0;
        uint32_t second = 0;
        BufferKey key;
    };

    struct RecordedFrame
    {
        std::vector<std::shared_ptr<const TextureUpload>> textures; // live when the frame started
        std::vector<std::shared_ptr<const BufferUpload>> buffers;
        std::vector<std::unique_ptr<VertexInput>> inputs; // the draws' inputs
        std::vector<RecordedOp> ops;
        uint32_t frontAddress = 0; // what the frame's Present showed
    };

    // Keeps what a recording needs while the game runs: every texture and buffer upload not yet
    // released (their data is shared with the owner's copy, not duplicated), and, once asked, the
    // operations up to the next Present, then written to a file. Fed by the Renderer's public calls in
    // posting order; any thread.
    class FrameRecorder
    {
    public:
        // The operations posted from now to the next Present are recorded into file.
        void RecordNextFrame(std::filesystem::path file);

        void UploadTexture(std::shared_ptr<const TextureUpload> upload);
        void UploadBuffer(std::shared_ptr<const BufferUpload> upload);
        void Clear(const ClearOp& op);
        void Resolve(const ResolveOp& op);
        // vertexConstants, pixelConstants: the bytes of the draw's constant ranges.
        void Draw(const DrawOp& op, const uint8_t* vertexConstants, const uint8_t* pixelConstants);
        void ReleaseSurface(uint32_t key);
        void ReleaseTexture(uint32_t address, uint32_t header);
        void ReleaseMemory(uint32_t begin, uint32_t end);
        void ReleaseBuffers(uint32_t begin, uint32_t end);
        void ReleaseBuffer(const BufferKey& key);
        // Ends the frame being recorded, if any, and writes its file (logged).
        void Present(uint32_t frontAddress);

    private:
        // Under mutex_.
        void Record(RecordedOp op);

        std::mutex mutex_; // everything below
        std::map<std::pair<uint32_t, uint64_t>, std::shared_ptr<const TextureUpload>> textures_; // by address, layout key
        std::map<BufferKey, std::shared_ptr<const BufferUpload>> buffers_;
        std::unique_ptr<RecordedFrame> frame_; // the frame being recorded
        std::map<uint64_t, size_t> inputIndices_; // VertexInput::id to frame_->inputs
        std::filesystem::path file_;
    };

    // Reads a recording written by FrameRecorder. False when the file is missing, damaged or of
    // another build's layout (logged).
    bool ReadRecordedFrame(const std::filesystem::path& file, RecordedFrame& frame);
}
