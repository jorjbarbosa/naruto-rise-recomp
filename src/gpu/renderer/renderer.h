#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace gpu
{
    struct Settings;

    // The host GPU: a D3D12 device, queue and swap chain through Plume. Operations are posted from
    // the guest threads and run in posting order on the owner thread, which alone touches the
    // D3D12 objects. A completion thread waits for the GPU and reports the end of each batch.
    class Renderer
    {
    public:
        // Called on the completion thread with the token of each Kick, in posting order.
        using Completion = std::function<void(uint32_t token)>;

        // Null when the device or the swap chain cannot be created (logged).
        static std::unique_ptr<Renderer> Create(void* window, const Settings& settings, Completion completion);
        ~Renderer();

        // Ends a batch: completion(token) runs once the GPU has finished everything posted before.
        void Kick(uint32_t token);
        // Presents a back buffer cleared to a fixed color (there is no guest image yet).
        void Present();

        struct State;

    private:
        explicit Renderer(std::unique_ptr<State> state);
        std::unique_ptr<State> state_;
    };
}
