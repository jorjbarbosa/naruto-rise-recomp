// The queue between the host UI and the renderer's owner: textures to create and release, and the
// newest frame.

#include "gpu/ui.h"
#include "gpu/ui_updates.h"

#include <algorithm>
#include <atomic>
#include <mutex>

namespace gpu::ui
{
    namespace
    {
        std::mutex g_mutex; // the state below
        Texture g_lastTexture = kNoTexture;
        Updates g_pending;
        std::atomic<PresentListener> g_listener{ nullptr };
    }

    Texture CreateTexture(TextureDesc desc)
    {
        std::lock_guard lock(g_mutex);
        Texture texture = ++g_lastTexture;
        g_pending.creates.emplace_back(texture, std::move(desc));
        return texture;
    }

    void ReleaseTexture(Texture texture)
    {
        if (texture == kNoTexture)
            return;
        std::lock_guard lock(g_mutex);
        // Not yet created: the create goes instead. No frame the owner draws can name it, since
        // frames submitted before the release were submitted before the create too, or replaced.
        auto create = std::find_if(g_pending.creates.begin(), g_pending.creates.end(),
            [&](const auto& entry) { return entry.first == texture; });
        if (create != g_pending.creates.end() &&
            (g_pending.frame == nullptr ||
                std::none_of(g_pending.frame->draws.begin(), g_pending.frame->draws.end(),
                    [&](const Draw& draw) { return draw.texture == texture; })))
        {
            g_pending.creates.erase(create);
            return;
        }
        g_pending.releases.push_back(texture);
    }

    void Submit(Frame frame)
    {
        auto shared = std::make_shared<const Frame>(std::move(frame));
        std::lock_guard lock(g_mutex);
        g_pending.frame = std::move(shared);
    }

    Updates TakeUpdates()
    {
        std::lock_guard lock(g_mutex);
        return std::exchange(g_pending, {});
    }

    void SetPresentListener(PresentListener listener)
    {
        g_listener.store(listener, std::memory_order_release);
    }

    void NotifyPresented()
    {
        if (PresentListener listener = g_listener.load(std::memory_order_acquire))
            listener();
    }
}
