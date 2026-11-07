#pragma once
#include <QtGlobal>

// Isolated local Vulkan / Gamescope diagnostic. Never enabled for normal sessions.
#if defined(Q_OS_LINUX) && defined(HAVE_LIBPLACEBO_VULKAN)
#include <QString>
#include <QStringList>
#include <cstdint>
#include <SDL.h>

namespace LocalVrrTest {
enum class Metric { Produce, Acquire, Render, Submit, Drop, AcquireFailure, Present, CompositorDiscard };
struct Snapshot {
    unsigned long long produced = 0, submitted = 0, dropped = 0, presented = 0, discarded = 0;
    double produceMs = 0, acquireMs = 0, renderMs = 0, submitMs = 0, presentMs = 0;
    double producePeakMs = 0, acquirePeakMs = 0, renderPeakMs = 0, submitPeakMs = 0, presentPeakMs = 0;
};
bool active();
std::uint64_t nowUs();
void record(Metric metric, std::uint64_t value = 0);
Snapshot snapshot();
bool initPresentationFeedback(SDL_Window* window);
void requestPresentationFeedback();
void shutdownPresentationFeedback();
int run(const QStringList& args);
}
#endif
