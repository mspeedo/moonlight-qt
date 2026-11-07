#include "localvrrtest.h"

#if defined(Q_OS_LINUX) && defined(HAVE_LIBPLACEBO_VULKAN)

#include <SDL.h>
#include <SDL_syswm.h>
#include <cstring>
#include <mutex>
#include <vector>
#include <algorithm>

#ifdef HAS_WAYLAND
#include <wayland-client.h>
#endif

namespace LocalVrrTest {
#ifdef HAS_WAYLAND
namespace {
// This is the version 1 stable Wayland presentation-time wire protocol.
// Keep the tiny local interface definitions instead of requiring generated
// SDK headers (which are not installed by all Flatpak runtimes).
extern const wl_interface kFeedbackInterface;
const wl_interface* kFeedbackRequestTypes[] = { &wl_surface_interface, &kFeedbackInterface };
const wl_message kPresentationRequests[] = {
    { "destroy", "", nullptr },
    { "feedback", "on", kFeedbackRequestTypes }
};
const wl_message kPresentationEvents[] = {
    { "clock_id", "u", nullptr }
};
const wl_interface kPresentationInterface = {
    "wp_presentation", 1, 2, kPresentationRequests, 1, kPresentationEvents
};
const wl_interface* kSyncOutputTypes[] = { &wl_output_interface };
const wl_message kFeedbackEvents[] = {
    { "sync_output", "o", kSyncOutputTypes },
    { "presented", "uuuuuuu", nullptr },
    { "discarded", "", nullptr }
};
const wl_interface kFeedbackInterface = {
    "wp_presentation_feedback", 1, 0, nullptr, 3, kFeedbackEvents
};

std::mutex g_Mutex;
wl_display* g_Display = nullptr;
wl_surface* g_Surface = nullptr;
wl_registry* g_Registry = nullptr;
wl_proxy* g_Presentation = nullptr;
std::vector<wl_proxy*> g_Pending;
bool g_ClockKnown = false;

void finished(wl_proxy* feedback)
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    const auto it = std::find(g_Pending.begin(), g_Pending.end(), feedback);
    if (it != g_Pending.end()) g_Pending.erase(it);
    wl_proxy_destroy(feedback);
}
void feedbackSyncOutput(void*, wl_proxy*, wl_output*) {}
void feedbackPresented(void*, wl_proxy* feedback,
                       std::uint32_t secHi, std::uint32_t secLo,
                       std::uint32_t nanos, std::uint32_t,
                       std::uint32_t, std::uint32_t, std::uint32_t)
{
    const std::uint64_t seconds = (std::uint64_t(secHi) << 32) | secLo;
    record(Metric::Present, seconds * 1000000ULL + nanos / 1000ULL);
    finished(feedback);
}
void feedbackDiscarded(void*, wl_proxy* feedback)
{
    record(Metric::CompositorDiscard);
    finished(feedback);
}
struct FeedbackListener {
    void (*sync_output)(void*, wl_proxy*, wl_output*);
    void (*presented)(void*, wl_proxy*, std::uint32_t, std::uint32_t,
                      std::uint32_t, std::uint32_t, std::uint32_t,
                      std::uint32_t, std::uint32_t);
    void (*discarded)(void*, wl_proxy*);
};
const FeedbackListener kFeedbackListener = {
    feedbackSyncOutput, feedbackPresented, feedbackDiscarded
};

void presentationClock(void*, wl_proxy*, std::uint32_t)
{
    g_ClockKnown = true;
}
struct PresentationListener {
    void (*clock_id)(void*, wl_proxy*, std::uint32_t);
};
const PresentationListener kPresentationListener = { presentationClock };

void registryGlobal(void*, wl_registry* registry,
                    std::uint32_t name, const char* interface, std::uint32_t)
{
    if (std::strcmp(interface, "wp_presentation") == 0 && !g_Presentation) {
        g_Presentation = reinterpret_cast<wl_proxy*>(
            wl_registry_bind(registry, name, &kPresentationInterface, 1));
        if (g_Presentation) {
            wl_proxy_add_listener(g_Presentation,
                reinterpret_cast<void (**)(void)>(
                    const_cast<PresentationListener*>(&kPresentationListener)), nullptr);
        }
    }
}
void registryRemove(void*, wl_registry*, std::uint32_t) {}
const wl_registry_listener kRegistryListener = { registryGlobal, registryRemove };
} // namespace
#endif

bool initPresentationFeedback(SDL_Window* window)
{
#ifdef HAS_WAYLAND
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_WAYLAND)
        return false;
    g_Display = info.info.wl.display;
    g_Surface = info.info.wl.surface;
    if (!g_Display || !g_Surface) return false;

    g_Registry = wl_display_get_registry(g_Display);
    if (!g_Registry) return false;
    wl_registry_add_listener(g_Registry, &kRegistryListener, nullptr);
    // All Wayland setup runs on the SDL event thread before the renderer starts.
    if (wl_display_roundtrip(g_Display) < 0 ||
        wl_display_roundtrip(g_Display) < 0)
        return false;
    return g_Presentation && g_ClockKnown;
#else
    (void)window;
    return false;
#endif
}

void requestPresentationFeedback()
{
#ifdef HAS_WAYLAND
    if (!g_Presentation || !g_ClockKnown || !g_Surface) return;
    // Called immediately before libplacebo's present. The compositor assigns
    // this request to the next wl_surface.commit, never to a guessed vsync.
    auto* feedback = wl_proxy_marshal_constructor(
        g_Presentation, 1, &kFeedbackInterface, g_Surface, nullptr);
    if (!feedback) return;
    wl_proxy_add_listener(feedback,
        reinterpret_cast<void (**)(void)>(
            const_cast<FeedbackListener*>(&kFeedbackListener)), nullptr);
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_Pending.push_back(feedback);
    }
#endif
}

void shutdownPresentationFeedback()
{
#ifdef HAS_WAYLAND
    if (g_Display) wl_display_roundtrip(g_Display);
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (auto* feedback : g_Pending) wl_proxy_destroy(feedback);
        g_Pending.clear();
    }
    if (g_Presentation) {
        wl_proxy_marshal(g_Presentation, 0);
        wl_proxy_destroy(g_Presentation);
        g_Presentation = nullptr;
    }
    if (g_Registry) {
        wl_registry_destroy(g_Registry);
        g_Registry = nullptr;
    }
    g_Display = nullptr;
    g_Surface = nullptr;
    g_ClockKnown = false;
#endif
}
} // namespace LocalVrrTest
#endif
