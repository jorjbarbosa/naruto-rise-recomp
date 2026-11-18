// Frame recordings: what the recorder keeps while the game runs, and the file format.

#include "gpu/renderer/recording.h"

#include "host/files.h"
#include "log.h"

#include <cstdio>
#include <cstring>
#include <type_traits>

namespace gpu
{
    namespace
    {
        constexpr uint32_t kMagic = 0x52465852; // "RXFR"
        constexpr uint32_t kVersion = 1;

        // The plain structs a recording stores as bytes. A recording made by a build where any of
        // their sizes differs is refused; a change of their fields that keeps the sizes needs a new
        // kVersion.
        constexpr uint32_t kLayoutSizes[] = { sizeof(SurfaceDesc), sizeof(Rect), sizeof(ClearOp), sizeof(ResolveOp),
            sizeof(SharedConstants), sizeof(VertexElement), sizeof(RenderState), sizeof(DrawTexture), sizeof(BufferKey),
            sizeof(HostedStream), sizeof(HostedIndices), sizeof(ConstantRange), sizeof(textures::LevelPlace) };

        // Writes plain values and arrays of them to a file; the first failure sticks.
        class Writer
        {
        public:
            explicit Writer(std::FILE* file) : file_(file) {}
            bool Ok() const { return ok_; }

            void Raw(const void* data, size_t bytes)
            {
                if (ok_ && bytes != 0)
                    ok_ = std::fwrite(data, 1, bytes, file_) == bytes;
            }
            template <typename T>
            void Pod(const T& value)
            {
                static_assert(std::is_trivially_copyable_v<T>);
                Raw(&value, sizeof(value));
            }
            template <typename Vector>
            void Array(const Vector& values)
            {
                static_assert(std::is_trivially_copyable_v<typename Vector::value_type>);
                Pod(uint64_t(values.size()));
                Raw(values.data(), values.size() * sizeof(typename Vector::value_type));
            }

        private:
            std::FILE* file_;
            bool ok_ = true;
        };

        // Reads what Writer wrote from memory; the first failure (past the end) sticks.
        class Reader
        {
        public:
            explicit Reader(const std::vector<uint8_t>& data) : at_(data.data()), end_(data.data() + data.size()) {}
            bool Ok() const { return ok_; }
            bool AtEnd() const { return at_ == end_; }

            void Raw(void* data, size_t bytes)
            {
                if (!ok_ || size_t(end_ - at_) < bytes)
                {
                    ok_ = false;
                    return;
                }
                if (bytes != 0)
                    std::memcpy(data, at_, bytes);
                at_ += bytes;
            }
            template <typename T>
            void Pod(T& value)
            {
                static_assert(std::is_trivially_copyable_v<T>);
                Raw(&value, sizeof(value));
            }
            template <typename Vector>
            void Array(Vector& values)
            {
                using Value = typename Vector::value_type;
                static_assert(std::is_trivially_copyable_v<Value>);
                uint64_t count = 0;
                Pod(count);
                if (!ok_ || count > uint64_t(end_ - at_) / sizeof(Value))
                {
                    ok_ = false;
                    return;
                }
                values.resize(size_t(count));
                Raw(values.data(), size_t(count) * sizeof(Value));
            }

        private:
            const uint8_t* at_;
            const uint8_t* end_;
            bool ok_ = true;
        };

        // The fields of a DrawOp other than its vectors and its input.
        struct DrawFields
        {
            uint64_t vertexShader;
            uint64_t pixelShader;
            SurfaceDesc color;
            SurfaceDesc depth;
            RenderState state;
            uint8_t stencilReference;
            std::array<float, 4> blendColor;
            std::array<uint32_t, kMaxVertexStreams> streamOffsets;
            std::array<HostedStream, kMaxVertexStreams> hosted;
            HostedIndices hostedIndices;
            ConstantRange vertexConstants;
            ConstantRange pixelConstants;
            SharedConstants shared;
            std::array<DrawTexture, SharedConstants::kFetchSlots> textures;
            std::array<float, 6> viewport;
            Rect scissor;
        };

        DrawFields FieldsOf(const DrawOp& op)
        {
            DrawFields fields{ op.vertexShader, op.pixelShader, op.color, op.depth, op.state, op.stencilReference,
                op.blendColor, op.streamOffsets, op.hosted, op.hostedIndices, op.vertexConstants, op.pixelConstants,
                op.shared, op.textures, {}, op.scissor };
            std::copy_n(op.viewport, 6, fields.viewport.begin());
            return fields;
        }

        void SetFields(DrawOp& op, const DrawFields& fields)
        {
            op.vertexShader = fields.vertexShader;
            op.pixelShader = fields.pixelShader;
            op.color = fields.color;
            op.depth = fields.depth;
            op.state = fields.state;
            op.stencilReference = fields.stencilReference;
            op.blendColor = fields.blendColor;
            op.streamOffsets = fields.streamOffsets;
            op.hosted = fields.hosted;
            op.hostedIndices = fields.hostedIndices;
            op.vertexConstants = fields.vertexConstants;
            op.pixelConstants = fields.pixelConstants;
            op.shared = fields.shared;
            op.textures = fields.textures;
            std::copy_n(fields.viewport.begin(), 6, op.viewport);
            op.scissor = fields.scissor;
        }

        void WriteTexture(Writer& out, const TextureUpload& upload)
        {
            const textures::Layout& layout = upload.layout;
            out.Pod(upload.address);
            out.Pod(upload.layoutKey);
            out.Pod(layout.dimension);
            out.Pod(layout.format);
            out.Pod(layout.endian);
            out.Pod(layout.tiled);
            out.Pod(layout.width);
            out.Pod(layout.height);
            out.Pod(layout.faces);
            out.Pod(layout.mipMin);
            out.Pod(layout.mipMax);
            out.Pod(layout.hasBase);
            out.Array(layout.levels);
            out.Pod(uint64_t(upload.ranges.size()));
            for (const auto& [address, bytes] : upload.ranges)
            {
                out.Pod(address);
                out.Pod(bytes);
            }
            out.Array(upload.data);
        }

        std::shared_ptr<const TextureUpload> ReadTexture(Reader& in)
        {
            auto upload = std::make_shared<TextureUpload>();
            textures::Layout& layout = upload->layout;
            in.Pod(upload->address);
            in.Pod(upload->layoutKey);
            in.Pod(layout.dimension);
            in.Pod(layout.format);
            in.Pod(layout.endian);
            in.Pod(layout.tiled);
            in.Pod(layout.width);
            in.Pod(layout.height);
            in.Pod(layout.faces);
            in.Pod(layout.mipMin);
            in.Pod(layout.mipMax);
            in.Pod(layout.hasBase);
            in.Array(layout.levels);
            uint64_t ranges = 0;
            in.Pod(ranges);
            for (uint64_t i = 0; i < ranges && in.Ok(); i++)
            {
                std::pair<uint32_t, uint32_t> range;
                in.Pod(range.first);
                in.Pod(range.second);
                upload->ranges.push_back(range);
            }
            in.Array(upload->data);
            return upload;
        }

        void WriteBuffer(Writer& out, const BufferUpload& upload)
        {
            out.Pod(upload.key);
            out.Array(upload.data);
        }

        std::shared_ptr<const BufferUpload> ReadBuffer(Reader& in)
        {
            auto upload = std::make_shared<BufferUpload>();
            in.Pod(upload->key);
            in.Array(upload->data);
            return upload;
        }

        bool WriteFrame(const std::filesystem::path& path, const RecordedFrame& frame)
        {
            std::FILE* file = host::OpenFile(path, "wb");
            if (file == nullptr)
                return false;
            Writer out(file);
            out.Pod(kMagic);
            out.Pod(kVersion);
            out.Pod(uint32_t(std::size(kLayoutSizes)));
            for (uint32_t size : kLayoutSizes)
                out.Pod(size);
            out.Pod(frame.frontAddress);

            out.Pod(uint64_t(frame.textures.size()));
            for (const auto& texture : frame.textures)
                WriteTexture(out, *texture);
            out.Pod(uint64_t(frame.buffers.size()));
            for (const auto& buffer : frame.buffers)
                WriteBuffer(out, *buffer);
            out.Pod(uint64_t(frame.inputs.size()));
            for (const auto& input : frame.inputs)
            {
                out.Pod(input->id);
                out.Array(input->elements);
                out.Array(input->strides);
            }

            out.Pod(uint64_t(frame.ops.size()));
            for (const RecordedOp& op : frame.ops)
            {
                out.Pod(op.kind);
                switch (op.kind)
                {
                case RecordedOp::Kind::Clear: out.Pod(op.clear); break;
                case RecordedOp::Kind::Resolve: out.Pod(op.resolve); break;
                case RecordedOp::Kind::Draw:
                {
                    const RecordedDraw& draw = *op.draw;
                    out.Pod(FieldsOf(draw.op));
                    out.Pod(draw.op.input->id);
                    out.Array(draw.op.vertices);
                    out.Array(draw.op.indices);
                    out.Array(draw.vertexConstants);
                    out.Array(draw.pixelConstants);
                    break;
                }
                case RecordedOp::Kind::UploadTexture: WriteTexture(out, *op.texture); break;
                case RecordedOp::Kind::UploadBuffer: WriteBuffer(out, *op.buffer); break;
                case RecordedOp::Kind::ReleaseBuffer: out.Pod(op.key); break;
                default:
                    out.Pod(op.first);
                    out.Pod(op.second);
                    break;
                }
            }
            bool ok = out.Ok();
            return std::fclose(file) == 0 && ok;
        }
    }

    void FrameRecorder::RecordNextFrame(std::filesystem::path file)
    {
        std::lock_guard lock(mutex_);
        frame_ = std::make_unique<RecordedFrame>();
        inputIndices_.clear();
        file_ = std::move(file);
        for (const auto& [key, upload] : textures_)
            frame_->textures.push_back(upload);
        for (const auto& [key, upload] : buffers_)
            frame_->buffers.push_back(upload);
    }

    void FrameRecorder::Record(RecordedOp op)
    {
        if (frame_)
            frame_->ops.push_back(std::move(op));
    }

    void FrameRecorder::UploadTexture(std::shared_ptr<const TextureUpload> upload)
    {
        std::lock_guard lock(mutex_);
        textures_[{ upload->address, upload->layoutKey }] = upload;
        RecordedOp op;
        op.kind = RecordedOp::Kind::UploadTexture;
        op.texture = std::move(upload);
        Record(std::move(op));
    }

    void FrameRecorder::UploadBuffer(std::shared_ptr<const BufferUpload> upload)
    {
        std::lock_guard lock(mutex_);
        buffers_[upload->key] = upload;
        RecordedOp op;
        op.kind = RecordedOp::Kind::UploadBuffer;
        op.buffer = std::move(upload);
        Record(std::move(op));
    }

    void FrameRecorder::Clear(const ClearOp& clear)
    {
        std::lock_guard lock(mutex_);
        RecordedOp op;
        op.kind = RecordedOp::Kind::Clear;
        op.clear = clear;
        Record(std::move(op));
    }

    void FrameRecorder::Resolve(const ResolveOp& resolve)
    {
        std::lock_guard lock(mutex_);
        RecordedOp op;
        op.kind = RecordedOp::Kind::Resolve;
        op.resolve = resolve;
        Record(std::move(op));
    }

    void FrameRecorder::Draw(const DrawOp& drawn, const uint8_t* vertexConstants, const uint8_t* pixelConstants)
    {
        std::lock_guard lock(mutex_);
        if (!frame_)
            return;
        auto draw = std::make_shared<RecordedDraw>();
        draw->op = drawn;
        draw->vertexConstants.assign(vertexConstants, vertexConstants + drawn.vertexConstants.bytes);
        draw->pixelConstants.assign(pixelConstants, pixelConstants + drawn.pixelConstants.bytes);
        // The input as the frame keeps it: one copy per distinct input.
        auto [it, added] = inputIndices_.try_emplace(drawn.input->id, frame_->inputs.size());
        if (added)
            frame_->inputs.push_back(std::make_unique<VertexInput>(*drawn.input));
        draw->op.input = frame_->inputs[it->second].get();
        RecordedOp op;
        op.kind = RecordedOp::Kind::Draw;
        op.draw = std::move(draw);
        Record(std::move(op));
    }

    void FrameRecorder::ReleaseSurface(uint32_t key)
    {
        std::lock_guard lock(mutex_);
        RecordedOp op;
        op.kind = RecordedOp::Kind::ReleaseSurface;
        op.first = key;
        Record(std::move(op));
    }

    void FrameRecorder::ReleaseTexture(uint32_t address, uint32_t header)
    {
        std::lock_guard lock(mutex_);
        // As Textures::Release(address, address + 1): every layout uploaded at the address.
        textures_.erase(textures_.lower_bound({ address, 0 }), textures_.lower_bound({ address + 1, 0 }));
        RecordedOp op;
        op.kind = RecordedOp::Kind::ReleaseTexture;
        op.first = address;
        op.second = header;
        Record(std::move(op));
    }

    void FrameRecorder::ReleaseMemory(uint32_t begin, uint32_t end)
    {
        std::lock_guard lock(mutex_);
        textures_.erase(textures_.lower_bound({ begin, 0 }), textures_.lower_bound({ end, 0 }));
        RecordedOp op;
        op.kind = RecordedOp::Kind::ReleaseMemory;
        op.first = begin;
        op.second = end;
        Record(std::move(op));
    }

    void FrameRecorder::ReleaseBuffers(uint32_t begin, uint32_t end)
    {
        std::lock_guard lock(mutex_);
        // As Buffers::Release: every buffer that overlaps the range.
        std::erase_if(buffers_, [&](const auto& entry) {
            return entry.first.address < end && uint64_t(entry.first.address) + entry.first.bytes > begin;
        });
        RecordedOp op;
        op.kind = RecordedOp::Kind::ReleaseBuffers;
        op.first = begin;
        op.second = end;
        Record(std::move(op));
    }

    void FrameRecorder::ReleaseBuffer(const BufferKey& key)
    {
        std::lock_guard lock(mutex_);
        buffers_.erase(key);
        RecordedOp op;
        op.kind = RecordedOp::Kind::ReleaseBuffer;
        op.key = key;
        Record(std::move(op));
    }

    void FrameRecorder::Present(uint32_t frontAddress)
    {
        std::unique_ptr<RecordedFrame> frame;
        std::filesystem::path file;
        {
            std::lock_guard lock(mutex_);
            if (!frame_)
                return;
            frame = std::move(frame_);
            file = std::move(file_);
        }
        frame->frontAddress = frontAddress;
        std::error_code error;
        std::filesystem::create_directories(file.parent_path(), error);
        if (WriteFrame(file, *frame))
            Log("gpu: frame recorded to %s: %zu textures, %zu buffers, %zu operations", file.string().c_str(),
                frame->textures.size(), frame->buffers.size(), frame->ops.size());
        else
            Log("gpu: cannot write the frame recording %s", file.string().c_str());
    }

    bool ReadRecordedFrame(const std::filesystem::path& path, RecordedFrame& frame)
    {
        std::vector<uint8_t> data = host::ReadWholeFile(path);
        Reader in(data);
        uint32_t magic = 0, version = 0, sizeCount = 0;
        in.Pod(magic);
        in.Pod(version);
        in.Pod(sizeCount);
        bool sameLayout = in.Ok() && magic == kMagic && version == kVersion && sizeCount == std::size(kLayoutSizes);
        for (uint32_t i = 0; sameLayout && i < sizeCount; i++)
        {
            uint32_t size = 0;
            in.Pod(size);
            sameLayout = in.Ok() && size == kLayoutSizes[i];
        }
        if (!sameLayout)
        {
            Log("gpu: %s is not a frame recording of this build", path.string().c_str());
            return false;
        }
        in.Pod(frame.frontAddress);

        uint64_t count = 0;
        in.Pod(count);
        for (uint64_t i = 0; i < count && in.Ok(); i++)
            frame.textures.push_back(ReadTexture(in));
        in.Pod(count);
        for (uint64_t i = 0; i < count && in.Ok(); i++)
            frame.buffers.push_back(ReadBuffer(in));
        std::map<uint64_t, const VertexInput*> inputs;
        in.Pod(count);
        for (uint64_t i = 0; i < count && in.Ok(); i++)
        {
            auto input = std::make_unique<VertexInput>();
            in.Pod(input->id);
            in.Array(input->elements);
            in.Array(input->strides);
            inputs[input->id] = input.get();
            frame.inputs.push_back(std::move(input));
        }

        in.Pod(count);
        for (uint64_t i = 0; i < count && in.Ok(); i++)
        {
            RecordedOp op;
            in.Pod(op.kind);
            switch (op.kind)
            {
            case RecordedOp::Kind::Clear: in.Pod(op.clear); break;
            case RecordedOp::Kind::Resolve: in.Pod(op.resolve); break;
            case RecordedOp::Kind::Draw:
            {
                auto draw = std::make_shared<RecordedDraw>();
                DrawFields fields;
                in.Pod(fields);
                SetFields(draw->op, fields);
                uint64_t inputId = 0;
                in.Pod(inputId);
                auto input = inputs.find(inputId);
                if (input == inputs.end())
                {
                    Log("gpu: %s: a draw names an input the recording lacks", path.string().c_str());
                    return false;
                }
                draw->op.input = input->second;
                in.Array(draw->op.vertices);
                in.Array(draw->op.indices);
                in.Array(draw->vertexConstants);
                in.Array(draw->pixelConstants);
                op.draw = std::move(draw);
                break;
            }
            case RecordedOp::Kind::UploadTexture: op.texture = ReadTexture(in); break;
            case RecordedOp::Kind::UploadBuffer: op.buffer = ReadBuffer(in); break;
            case RecordedOp::Kind::ReleaseBuffer: in.Pod(op.key); break;
            case RecordedOp::Kind::ReleaseSurface:
            case RecordedOp::Kind::ReleaseTexture:
            case RecordedOp::Kind::ReleaseMemory:
            case RecordedOp::Kind::ReleaseBuffers:
                in.Pod(op.first);
                in.Pod(op.second);
                break;
            default:
                Log("gpu: %s: unknown operation %u", path.string().c_str(), unsigned(op.kind));
                return false;
            }
            frame.ops.push_back(std::move(op));
        }
        if (!in.Ok() || !in.AtEnd())
        {
            Log("gpu: %s is damaged", path.string().c_str());
            return false;
        }
        return true;
    }
}
