#include "localvrrtest.h"

#if defined(Q_OS_LINUX) && defined(HAVE_LIBPLACEBO_VULKAN)

#include "streaming/video/ffmpeg-renderers/plvk.h"
#include "streaming/video/ffmpeg-renderers/pacer/pacer.h"

#include <QDir>
#include <QFile>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QStandardPaths>
#include <QTextStream>
#include <QDateTime>
#include <QDebug>

#include <SDL.h>
#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

namespace LocalVrrTest {
namespace {
using Clock = std::chrono::steady_clock;
struct Row { std::uint64_t timeUs, value; Metric metric; };
struct Stat { std::uint64_t count = 0, last = 0, peak = 0; long double sum = 0; };
std::atomic<bool> g_Enabled {false};
std::mutex g_Lock;
std::vector<Row> g_Rows;
std::array<Stat, 6> g_Stats;
std::uint64_t g_LastProduced = 0, g_LastSubmit = 0;
std::atomic<bool> g_Stop {false};
std::atomic<bool> g_Variable {true};
constexpr int kWidth = 960, kHeight = 540;

// Fixed, repeatable 80-110 FPS intervals in microseconds, not host cadence playback.
// The producer sleeps against an absolute deadline to avoid cumulative scheduler drift.
constexpr std::array<int, 20> kPeriods = {{
    9091, 11111, 12500, 9615, 11905, 10000, 11364, 9259, 12195, 10417,
    11628, 9434, 10870, 12346, 9804, 11236, 9174, 12048, 10204, 10638
}};

const char* metricName(Metric metric)
{
    switch (metric) {
    case Metric::Produce: return "production_interval";
    case Metric::Acquire: return "swapchain_acquire_duration";
    case Metric::Render: return "render_cpu_duration";
    case Metric::Submit: return "submission_interval";
    case Metric::Drop: return "pacer_drop";
    case Metric::AcquireFailure: return "swapchain_acquire_failed";
    }
    return "unknown";
}

double lastMs(Metric metric)
{
    const auto& s = g_Stats[static_cast<int>(metric)];
    return double(s.last) / 1000.0;
}

double peakMs(Metric metric)
{
    return double(g_Stats[static_cast<int>(metric)].peak) / 1000.0;
}

void exportCsv()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!QDir().mkpath(dir)) {
        qWarning() << "VRR diagnostic: cannot create output directory" << dir;
        return;
    }
    const QString filename = dir + "/local-vrr-" +
        QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss") + ".csv";
    QFile csv(filename);
    if (!csv.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "VRR diagnostic: cannot write" << filename;
        return;
    }
    QTextStream stream(&csv);
    stream << "monotonic_us,event,value_us,actual_present_us\n";
    std::lock_guard<std::mutex> lock(g_Lock);
    for (const Row& row : g_Rows) {
        stream << row.timeUs << ',' << metricName(row.metric) << ','
               << row.value << ",\n";
    }
    qInfo() << "VRR diagnostic CSV:" << filename
            << "(actual presentation unavailable through libplacebo swapchain)";
}

AVFrame* makeFrame(std::uint64_t elapsedUs)
{
    AVFrame* frame = av_frame_alloc();
    if (!frame) return nullptr;
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = kWidth;
    frame->height = kHeight;
    frame->colorspace = AVCOL_SPC_BT709;
    frame->color_primaries = AVCOL_PRI_BT709;
    frame->color_trc = AVCOL_TRC_BT709;
    frame->color_range = AVCOL_RANGE_JPEG;
    if (av_frame_get_buffer(frame, 32) < 0) {
        av_frame_free(&frame);
        return nullptr;
    }

    // Continuous, time-based horizontal translation, with crisp vertical
    // bars, checker contrast and a narrow bright tracking line. Do not use
    // the produced-frame index to derive any visual coordinate.
    const int motion = int((elapsedUs * 240ULL / 1000000ULL) % kWidth);
    for (int y = 0; y < kHeight; ++y) {
        auto* pixels = frame->data[0] + y * frame->linesize[0];
        for (int x = 0; x < kWidth; ++x) {
            const int movingX = (x + motion) % kWidth;
            const bool checker = ((movingX / 72 + y / 72) & 1) != 0;
            const bool tracking = movingX < 12 || (movingX > 480 && movingX < 486);
            pixels[x] = tracking ? 236 : (checker ? 179 : 42);
        }
    }
    for (int y = 0; y < kHeight / 2; ++y) {
        std::memset(frame->data[1] + y * frame->linesize[1], 128, kWidth / 2);
        std::memset(frame->data[2] + y * frame->linesize[2], 128, kWidth / 2);
    }
    frame->pkt_dts = static_cast<std::int64_t>(LiGetMicroseconds());
    return frame;
}

struct ProducerContext { Pacer* pacer; };
int producerThread(void* context)
{
    auto* producer = static_cast<ProducerContext*>(context);
    const auto epoch = Clock::now();
    auto due = epoch;
    std::uint64_t frameIndex = 0;
    while (!g_Stop.load(std::memory_order_relaxed)) {
        const int interval = g_Variable.load(std::memory_order_relaxed)
            ? kPeriods[frameIndex % kPeriods.size()] : 11111;
        due += std::chrono::microseconds(interval);
        std::this_thread::sleep_until(due);
        if (g_Stop.load(std::memory_order_relaxed)) break;

        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - epoch).count();
        AVFrame* frame = makeFrame(static_cast<std::uint64_t>(elapsed));
        if (!frame) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "VRR diagnostic frame allocation failed");
            g_Stop.store(true);
            break;
        }
        record(Metric::Produce, nowUs());
        producer->pacer->submitFrame(frame);
        ++frameIndex;
    }
    return 0;
}

SDL_Surface* createHud(const Snapshot& s, bool late)
{
    // Rendering the HUD at 2 Hz keeps its upload overhead out of most frames.
    QImage image(930, 252, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.fillRect(0, 0, image.width(), image.height(), QColor(0, 0, 0, 205));
    painter.setPen(Qt::white);
    QFont font("monospace");
    font.setPixelSize(21);
    painter.setFont(font);
    QStringList lines = {
        QString("LOCAL VRR PIPELINE | %1 | %2")
            .arg(g_Variable.load() ? "repeatable 80-110 FPS" : "fixed 90 FPS")
            .arg(late ? "ACQUIRE LATE" : "ACQUIRE EARLY (baseline)"),
        QString("Frames produced %1 | submitted %2 | dropped %3")
            .arg(s.produced).arg(s.submitted).arg(s.dropped),
        QString("Producer interval: %1 ms | max %2")
            .arg(s.produceMs, 0, 'f', 2).arg(s.producePeakMs, 0, 'f', 2),
        QString("Swapchain acquire: %1 ms | max %2")
            .arg(s.acquireMs, 0, 'f', 2).arg(s.acquirePeakMs, 0, 'f', 2),
        QString("Render CPU: %1 ms | max %2")
            .arg(s.renderMs, 0, 'f', 2).arg(s.renderPeakMs, 0, 'f', 2),
        QString("Submit interval: %1 ms | max %2")
            .arg(s.submitMs, 0, 'f', 2).arg(s.submitPeakMs, 0, 'f', 2),
        QString("Display presentation feedback: N/A (libplacebo does not expose it)"),
        QString("X: 90/variable | Y: early/late | B: exit | H: HUD")
    };
    for (int i = 0; i < lines.size(); ++i)
        painter.drawText(12, 29 + i * 29, lines[i]);
    painter.end();

    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(
        0, image.width(), image.height(), 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) return nullptr;
    for (int y = 0; y < image.height(); ++y) {
        std::memcpy(static_cast<std::uint8_t*>(surface->pixels) + y * surface->pitch,
                    image.constScanLine(y), image.width() * 4);
    }
    return surface;
}
} // namespace

bool active() { return g_Enabled.load(std::memory_order_relaxed); }
std::uint64_t nowUs() { return static_cast<std::uint64_t>(LiGetMicroseconds()); }
void record(Metric metric, std::uint64_t value)
{
    if (!active()) return;
    const auto now = nowUs();
    std::lock_guard<std::mutex> lock(g_Lock);
    if (metric == Metric::Produce) {
        value = g_LastProduced ? now - g_LastProduced : 0;
        g_LastProduced = now;
    } else if (metric == Metric::Submit) {
        value = g_LastSubmit ? now - g_LastSubmit : 0;
        g_LastSubmit = now;
    } else if (metric == Metric::Drop) {
        value = 1;
    }
    Stat& stat = g_Stats[static_cast<int>(metric)];
    ++stat.count;
    if (value > 0) {
        stat.last = value;
        stat.peak = std::max(stat.peak, value);
        stat.sum += value;
    }
    if (g_Rows.size() < 500000)
        g_Rows.push_back({now, value, metric});
}
Snapshot snapshot()
{
    std::lock_guard<std::mutex> lock(g_Lock);
    Snapshot s;
    s.produced = g_Stats[static_cast<int>(Metric::Produce)].count;
    s.submitted = g_Stats[static_cast<int>(Metric::Submit)].count;
    s.dropped = g_Stats[static_cast<int>(Metric::Drop)].count;
    s.produceMs = lastMs(Metric::Produce);
    s.acquireMs = lastMs(Metric::Acquire);
    s.renderMs = lastMs(Metric::Render);
    s.submitMs = lastMs(Metric::Submit);
    s.producePeakMs = peakMs(Metric::Produce);
    s.acquirePeakMs = peakMs(Metric::Acquire);
    s.renderPeakMs = peakMs(Metric::Render);
    s.submitPeakMs = peakMs(Metric::Submit);
    return s;
}

int run(const QStringList& args)
{
    g_Stop.store(false);
    g_Variable.store(!args.contains("--vrr-constant"));
    bool late = args.contains("--vrr-acquire-late");
    bool showHud = true;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        qWarning() << "VRR diagnostic SDL initialization failed:" << SDL_GetError();
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("Moonlight local VRR diagnostic",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 800, SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN);
    if (!window) {
        qWarning() << "VRR diagnostic window failed:" << SDL_GetError();
        return 1;
    }
    SDL_ShowCursor(SDL_DISABLE);
    SDL_GameController* controller = nullptr;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            controller = SDL_GameControllerOpen(i);
            break;
        }
    }

    int result = 0;
    {
        PlVkRenderer renderer;
        DECODER_PARAMETERS params = {};
        params.window = window;
        params.videoFormat = VIDEO_FORMAT_H264;
        params.width = kWidth;
        params.height = kHeight;
        params.frameRate = 110;
        params.enableVsync = false; // preserve IMMEDIATE and unpaced VRR behavior
        params.enableFramePacing = false;
        if (!renderer.initialize(&params)) {
            qWarning() << "VRR diagnostic Vulkan renderer initialization failed";
            result = 2;
        } else {
            VIDEO_STATS stats = {};
            Pacer pacer(&renderer, &stats, true);
            pacer.setDiagnosticAcquireLate(late);
            if (!pacer.initialize(window, 110, false, false)) {
                qWarning() << "VRR diagnostic Pacer initialization failed";
                result = 3;
            } else {
                ProducerContext producer { &pacer };
                g_Enabled.store(true, std::memory_order_release);
                SDL_Thread* thread = SDL_CreateThread(producerThread, "LocalVRRProducer", &producer);
                if (!thread) {
                    g_Enabled.store(false, std::memory_order_release);
                    result = 4;
                } else {
                    Uint32 nextHudUpdate = 0;
                    bool running = true;
                    while (running && !g_Stop.load()) {
                        SDL_Event event;
                        while (SDL_PollEvent(&event)) {
                            if (event.type == SDL_QUIT) running = false;
                            if (event.type == SDL_RENDER_DEVICE_RESET) {
                                qWarning() << "VRR diagnostic: Vulkan device reset requested";
                                running = false; result = 5;
                            }
                            if (event.type == SDL_KEYDOWN) {
                                if (event.key.keysym.sym == SDLK_ESCAPE) running = false;
                                if (event.key.keysym.sym == SDLK_x || event.key.keysym.sym == SDLK_c)
                                    g_Variable.store(!g_Variable.load());
                                if (event.key.keysym.sym == SDLK_y || event.key.keysym.sym == SDLK_l) {
                                    late = !late; pacer.setDiagnosticAcquireLate(late);
                                }
                                if (event.key.keysym.sym == SDLK_h) showHud = !showHud;
                            }
                            if (event.type == SDL_CONTROLLERBUTTONDOWN) {
                                if (event.cbutton.button == SDL_CONTROLLER_BUTTON_B) running = false;
                                if (event.cbutton.button == SDL_CONTROLLER_BUTTON_X)
                                    g_Variable.store(!g_Variable.load());
                                if (event.cbutton.button == SDL_CONTROLLER_BUTTON_Y) {
                                    late = !late; pacer.setDiagnosticAcquireLate(late);
                                }
                                if (event.cbutton.button == SDL_CONTROLLER_BUTTON_A)
                                    showHud = !showHud;
                            }
                        }
                        if (SDL_GetTicks() >= nextHudUpdate) {
                            nextHudUpdate = SDL_GetTicks() + 500;
                            if (showHud) {
                                SDL_Surface* hud = createHud(snapshot(), late);
                                if (hud) renderer.updateDebugOverlay(hud, nullptr);
                            } else {
                                renderer.updateDebugOverlay(nullptr, nullptr);
                            }
                        }
                        SDL_Delay(8);
                    }
                    g_Stop.store(true);
                    SDL_WaitThread(thread, nullptr);
                }
                // Pacer is destroyed while the renderer is still alive;
                // it drains queued frames and releases pending swapchain images.
                g_Enabled.store(false, std::memory_order_release);
            }
        }
    }
    if (controller) SDL_GameControllerClose(controller);
    SDL_ShowCursor(SDL_ENABLE);
    SDL_DestroyWindow(window);
    exportCsv();
    return result;
}
} // namespace LocalVrrTest

#endif
