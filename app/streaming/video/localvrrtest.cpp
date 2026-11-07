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

#ifdef HAS_X11
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#endif

#include <SDL.h>
#include <SDL_syswm.h>
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
struct Row { std::uint64_t timeUs, value, actualPresentUs; Metric metric; };
struct Stat { std::uint64_t count = 0, last = 0, peak = 0; long double sum = 0; };
std::atomic<bool> g_Enabled {false};
std::mutex g_Lock;
std::vector<Row> g_Rows;
std::array<Stat, 8> g_Stats;
std::uint64_t g_LastProduced = 0, g_LastSubmit = 0, g_LastPresented = 0;
bool g_PresentationSupported = false;
QString g_LastCsvPath;
std::atomic<bool> g_Stop {false};
std::atomic<bool> g_ProducerFailed {false};
std::atomic<bool> g_Variable {true};
QString g_CompositionRequest;
QString g_CompositionControl;
QString g_ScanoutPath;
QString g_ScanoutEvidence;
QString g_SdlWindowBackend;
QString g_VulkanPresentMode;
constexpr int kWidth = 960, kHeight = 540;

// Fixed, repeatable 80-110 FPS intervals in microseconds, not host cadence playback.
// The producer sleeps against an absolute deadline to avoid cumulative scheduler drift.
constexpr std::array<int, 20> kPeriods = {{
    9091, 11111, 12500, 9615, 11905, 10000, 11364, 9259, 12195, 10417,
    11628, 9434, 10870, 12346, 9804, 11236, 9174, 12048, 10204, 10638
}};


// The compositor owns the DRM plane allocation decision; it is not observable
// from Vulkan submit times or wp_presentation's ZERO_COPY bit (which Gamescope
// currently sets even for GPU-composited frames). Never present those as proof
// of physical direct scanout.
#ifdef HAS_X11
bool x11Cardinal(Display* display, Window root, Atom atom, unsigned long* value)
{
    if (!display || atom == None) return false;
    Atom type = None;
    int format = 0;
    unsigned long count = 0, remaining = 0;
    unsigned char* data = nullptr;
    const int rc = XGetWindowProperty(display, root, atom, 0, 1, False,
                                       XA_CARDINAL, &type, &format, &count,
                                       &remaining, &data);
    const bool valid = rc == Success && type == XA_CARDINAL &&
                       format == 32 && count == 1 && data;
    if (valid) *value = *reinterpret_cast<unsigned long*>(data);
    if (data) XFree(data);
    return valid;
}
#endif

// The X root property is the supported Gamescope force-composition switch.
// Its read-back confirms the property change, NOT that Gamescope processed
// the event or which DRM planes were ultimately selected.
// Stock Gamescope does not export actual scanout-path telemetry. A locally
// instrumented Gamescope may publish GAMESCOPE_MOONLIGHT_SCANOUT_PATH:
// 1=hardware scanout, 2=full GPU composition. Unknown is always explicit.
class GamescopeCompositionControl
{
public:
    explicit GamescopeCompositionControl(bool force) : m_Force(force)
    {
#ifdef HAS_X11
        m_Display = XOpenDisplay(nullptr);
        if (!m_Display) {
            m_Status = QStringLiteral("XWayland unavailable: cannot control Gamescope");
            return;
        }
        m_Root = DefaultRootWindow(m_Display);
        const Atom signature = XInternAtom(m_Display, "GAMESCOPE_VRR_CAPABLE", True);
        unsigned long capable = 0;
        if (!x11Cardinal(m_Display, m_Root, signature, &capable)) {
            m_Status = QStringLiteral("X11 root is not a verified Gamescope root");
            return;
        }
        m_ForceAtom = XInternAtom(m_Display, "GAMESCOPE_COMPOSITE_FORCE", False);
        m_HadOriginal = x11Cardinal(m_Display, m_Root, m_ForceAtom, &m_Original);
        const unsigned long requested = force ? 1 : 0;
        m_Changed = true; // Restore even if read-back verification fails.
        XChangeProperty(m_Display, m_Root, m_ForceAtom, XA_CARDINAL, 32,
                        PropModeReplace, reinterpret_cast<const unsigned char*>(&requested), 1);
        XSync(m_Display, False);
        unsigned long current = ~0UL;
        if (!x11Cardinal(m_Display, m_Root, m_ForceAtom, &current) ||
            current != requested) {
            m_Status = QStringLiteral("Gamescope control property read-back failed");
            return;
        }
        m_Ready = true;
        m_Status = QStringLiteral("Gamescope X11 control set; no compositor acknowledgement");
        // Intern the name even if the compositor has not yet published its
        // first path decision; otherwise a fresh instrumented build would be
        // incorrectly treated as unsupported for the entire benchmark.
        m_ScanoutAtom = XInternAtom(m_Display,
                                   "GAMESCOPE_MOONLIGHT_SCANOUT_PATH", False);
#else
        m_Status = QStringLiteral("Built without X11 Gamescope control support");
#endif
    }

    ~GamescopeCompositionControl()
    {
#ifdef HAS_X11
        if (m_Display) {
            if (m_Changed) {
                if (m_HadOriginal) {
                    XChangeProperty(m_Display, m_Root, m_ForceAtom, XA_CARDINAL,
                                    32, PropModeReplace,
                                    reinterpret_cast<const unsigned char*>(&m_Original), 1);
                } else {
                    // Deletion triggers a Gamescope property change back to its
                    // normal default (0). A compositor launched with -c is a
                    // special case: the root property doesn't encode that flag.
                    XDeleteProperty(m_Display, m_Root, m_ForceAtom);
                }
                XSync(m_Display, False);
            }
            XCloseDisplay(m_Display);
        }
#endif
    }

    bool ready() const { return m_Ready; }
    QString status() const { return m_Status; }
    QString path() const { return m_Path; }
    QString evidence() const { return m_Evidence; }

    void sampleScanoutPath()
    {
#ifdef HAS_X11
        if (m_Ready && m_ScanoutAtom != None) {
            unsigned long raw = 0;
            if (x11Cardinal(m_Display, m_Root, m_ScanoutAtom, &raw)) {
                if (raw == 1) {
                    m_Path = QStringLiteral("Hardware scanout (Gamescope telemetry)");
                    m_Evidence = QStringLiteral("Instrumented Gamescope DRM path property");
                    return;
                }
                if (raw == 2) {
                    m_Path = QStringLiteral("GPU composition (Gamescope telemetry)");
                    m_Evidence = QStringLiteral("Instrumented Gamescope DRM path property");
                    return;
                }
            }
        }
#endif
        // A requested mode is not a measurement of the active DRM plane path.
        m_Path = QStringLiteral("Not observable with stock Gamescope");
        m_Evidence = m_Force
            ? QStringLiteral("GPU composition requested; hardware path unverified")
            : QStringLiteral("Automatic: Gamescope exposes no hardware path status");
    }

private:
    bool m_Force;
    bool m_Ready = false;
    QString m_Status;
    QString m_Path = QStringLiteral("Not observable with stock Gamescope");
    QString m_Evidence = QStringLiteral("No compositor-side scanout evidence");
#ifdef HAS_X11
    Display* m_Display = nullptr;
    Window m_Root = 0;
    Atom m_ForceAtom = None;
    Atom m_ScanoutAtom = None;
    bool m_HadOriginal = false, m_Changed = false;
    unsigned long m_Original = 0;
#endif
};

const char* metricName(Metric metric)
{
    switch (metric) {
    case Metric::Produce: return "production_interval";
    case Metric::Acquire: return "swapchain_acquire_duration";
    case Metric::Render: return "render_cpu_duration";
    case Metric::Submit: return "submission_interval";
    case Metric::Drop: return "pacer_drop";
    case Metric::AcquireFailure: return "swapchain_acquire_failed";
    case Metric::Present: return "compositor_presentation_interval";
    case Metric::CompositorDiscard: return "compositor_discard";
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
double averageMs(Metric metric)
{
    const auto& stat = g_Stats[static_cast<int>(metric)];
    const bool initialEmpty = metric == Metric::Produce || metric == Metric::Submit ||
                              metric == Metric::Present;
    const std::uint64_t samples = stat.count > (initialEmpty ? 1u : 0u)
                                  ? stat.count - (initialEmpty ? 1u : 0u) : 0;
    return samples ? double(stat.sum / samples) / 1000.0 : 0.0;
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
               << row.value << ',';
        if (row.actualPresentUs != 0) stream << row.actualPresentUs;
        stream << '\n';
    }
    g_LastCsvPath = filename;
    qInfo() << "VRR diagnostic CSV:" << filename
            << "Wayland presentation timestamps provided where available";
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
            g_ProducerFailed.store(true);
            g_Stop.store(true);
            break;
        }
        record(Metric::Produce, nowUs());
        producer->pacer->submitFrame(frame);
        ++frameIndex;
    }
    return 0;
}


QString actualWindowBackend(SDL_Window* window)
{
    SDL_SysWMinfo wm = {};
    SDL_VERSION(&wm.version);
    if (SDL_GetWindowWMInfo(window, &wm)) {
        switch (wm.subsystem) {
            case SDL_SYSWM_WAYLAND:
                return QStringLiteral("Native Wayland (SDL window)");
            case SDL_SYSWM_X11:
                return QStringLiteral("X11 (typically XWayland in SteamOS Game Mode)");
            case SDL_SYSWM_KMSDRM:
                return QStringLiteral("KMS/DRM (SDL window)");
            default:
                break;
        }
    }

    // A global SDL driver name is not proof of a given window's WSI.
    const char* videoDriver = SDL_GetCurrentVideoDriver();
    return QStringLiteral("Unverified SDL window backend (driver: %1)")
        .arg(videoDriver ? QString::fromUtf8(videoDriver) : QStringLiteral("unknown"));
}

QString swapchainPresentMode(VkPresentModeKHR mode)
{
    switch (mode) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR:
            return QStringLiteral("IMMEDIATE_KHR");
        case VK_PRESENT_MODE_MAILBOX_KHR:
            return QStringLiteral("MAILBOX_KHR");
        case VK_PRESENT_MODE_FIFO_KHR:
            return QStringLiteral("FIFO_KHR");
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
            return QStringLiteral("FIFO_RELAXED_KHR");
        case VK_PRESENT_MODE_MAX_ENUM_KHR:
            return QStringLiteral("No Vulkan swapchain");
        default:
            return QStringLiteral("Unknown Vulkan present mode (%1)")
                .arg(static_cast<int>(mode));
    }
}

SDL_Surface* createHud(const Snapshot& s, bool late)
{
    // Rendering the HUD at 2 Hz keeps its upload overhead out of most frames.
    QImage image(1160, 455, QImage::Format_ARGB32);
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
        QString("Gamescope actual presentation: %1")
            .arg(g_PresentationSupported
                ? QString("%1 ms | max %2 | count %3")
                    .arg(s.presentMs, 0, 'f', 2).arg(s.presentPeakMs, 0, 'f', 2)
                    .arg(s.presented)
                : QString("unavailable on this window/backend")),
        QString("Compositor discarded %1 | CPU submit != display time").arg(s.discarded),
        QString("SDL window: %1").arg(s.sdlWindowBackend),
        QString("Vulkan swapchain: %1").arg(s.vulkanPresentMode),
        QString("Composition: %1").arg(s.compositionRequest),
        QString("Scanout path: %1").arg(s.scanoutPath),
        QString("X: 90/variable | Y: early/late | B: exit | A/H: HUD")
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
    const auto actualPresentUs = metric == Metric::Present ? value : 0;
    if (metric == Metric::Produce) {
        value = g_LastProduced ? now - g_LastProduced : 0;
        g_LastProduced = now;
    } else if (metric == Metric::Submit) {
        value = g_LastSubmit ? now - g_LastSubmit : 0;
        g_LastSubmit = now;
    } else if (metric == Metric::Present) {
        const std::uint64_t presentedUs = value;
        value = g_LastPresented ? presentedUs - g_LastPresented : 0;
        g_LastPresented = presentedUs;
    } else if (metric == Metric::Drop || metric == Metric::CompositorDiscard) {
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
        g_Rows.push_back({now, value, actualPresentUs, metric});
}
QString lastCsvPath()
{
    std::lock_guard<std::mutex> lock(g_Lock);
    return g_LastCsvPath;
}
Snapshot snapshot()
{
    std::lock_guard<std::mutex> lock(g_Lock);
    Snapshot s;
    s.produced = g_Stats[static_cast<int>(Metric::Produce)].count;
    s.submitted = g_Stats[static_cast<int>(Metric::Submit)].count;
    s.dropped = g_Stats[static_cast<int>(Metric::Drop)].count;
    s.presented = g_Stats[static_cast<int>(Metric::Present)].count;
    s.discarded = g_Stats[static_cast<int>(Metric::CompositorDiscard)].count;
    s.produceMs = lastMs(Metric::Produce);
    s.acquireMs = lastMs(Metric::Acquire);
    s.renderMs = lastMs(Metric::Render);
    s.submitMs = lastMs(Metric::Submit);
    s.presentMs = lastMs(Metric::Present);
    s.produceAvgMs = averageMs(Metric::Produce);
    s.acquireAvgMs = averageMs(Metric::Acquire);
    s.renderAvgMs = averageMs(Metric::Render);
    s.submitAvgMs = averageMs(Metric::Submit);
    s.presentAvgMs = averageMs(Metric::Present);
    s.producePeakMs = peakMs(Metric::Produce);
    s.acquirePeakMs = peakMs(Metric::Acquire);
    s.renderPeakMs = peakMs(Metric::Render);
    s.submitPeakMs = peakMs(Metric::Submit);
    s.presentPeakMs = peakMs(Metric::Present);
    s.compositionRequest = g_CompositionRequest;
    s.compositionControl = g_CompositionControl;
    s.scanoutPath = g_ScanoutPath;
    s.scanoutEvidence = g_ScanoutEvidence;
    s.sdlWindowBackend = g_SdlWindowBackend;
    s.vulkanPresentMode = g_VulkanPresentMode;
    return s;
}

int run(const QStringList& args)
{
    g_Enabled.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_Lock);
        g_Rows.clear();
        g_Stats.fill({});
        g_LastProduced = g_LastSubmit = g_LastPresented = 0;
        g_LastCsvPath.clear();
    }
    g_Stop.store(false);
    g_ProducerFailed.store(false);
    g_Variable.store(!args.contains("--vrr-constant"));
    const bool forceComposition = args.contains("--vrr-force-composition");
    g_CompositionRequest = forceComposition
        ? QStringLiteral("Forced GPU composition")
        : QStringLiteral("Automatic (Gamescope decision)");
    g_CompositionControl.clear();
    g_ScanoutPath = QStringLiteral("Not measured");
    g_ScanoutEvidence.clear();
    g_SdlWindowBackend = QStringLiteral("Not initialized");
    g_VulkanPresentMode = QStringLiteral("Not initialized");
    GamescopeCompositionControl compositor(forceComposition);
    g_CompositionControl = compositor.status();
    compositor.sampleScanoutPath();
    g_ScanoutPath = compositor.path();
    g_ScanoutEvidence = compositor.evidence();
    if (forceComposition && !compositor.ready()) {
        qWarning() << "Cannot run forced-composition comparison:" << g_CompositionControl;
        return 8;
    }
    bool late = args.contains("--vrr-acquire-late");
    bool showHud = !args.contains("--vrr-hud-off");
    int durationSeconds = 0;
    for (const QString& arg : args) {
        if (arg.startsWith("--vrr-duration=")) {
            bool ok = false;
            const int seconds = arg.mid(QString("--vrr-duration=").length()).toInt(&ok);
            if (!ok || seconds < 0 || seconds > 3600) {
                qWarning() << "Invalid VRR duration:" << arg;
                return 6;
            }
            durationSeconds = seconds;
        }
    }
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        qWarning() << "VRR diagnostic SDL initialization failed:" << SDL_GetError();
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("Moonlight local VRR diagnostic",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 800, SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN);
    if (!window) {
        qWarning() << "VRR diagnostic window failed:" << SDL_GetError();
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
        return 1;
    }
    g_SdlWindowBackend = actualWindowBackend(window);
    qInfo() << "Local VRR actual SDL window backend:" << g_SdlWindowBackend
            << "SDL driver:" << SDL_GetCurrentVideoDriver();
    const int previousCursor = SDL_ShowCursor(SDL_QUERY);
    SDL_ShowCursor(SDL_DISABLE);
    g_PresentationSupported = initPresentationFeedback(window);
    qInfo() << "Wayland compositor presentation feedback:" << g_PresentationSupported;
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
            g_VulkanPresentMode = swapchainPresentMode(renderer.diagnosticSwapchainPresentMode());
            // Mesa may override the VkSwapchainCreateInfoKHR present mode at
            // WSI level, in which case the app cannot query the effective mode
            // back from Vulkan. Explicitly surface the override instead.
            const QByteArray mesaPresentOverride = qgetenv("MESA_VK_WSI_PRESENT_MODE");
            if (!mesaPresentOverride.isEmpty()) {
                g_VulkanPresentMode += QStringLiteral(" [Mesa WSI override=%1; effective mode unknown]")
                    .arg(QString::fromUtf8(mesaPresentOverride));
            }
            qInfo() << "Local VRR Vulkan swapchain requested present mode:" << g_VulkanPresentMode;
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
                    const auto benchmarkStart = Clock::now();
                    while (running && !g_Stop.load()) {
                        if (durationSeconds > 0 &&
                            Clock::now() - benchmarkStart >=
                                std::chrono::seconds(durationSeconds)) {
                            running = false;
                            break;
                        }
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
                            compositor.sampleScanoutPath();
                            g_ScanoutPath = compositor.path();
                            g_ScanoutEvidence = compositor.evidence();
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
                    if (g_ProducerFailed.load()) result = 7;
                }
                // Pacer is destroyed while the renderer is still alive;
                // it drains queued frames and releases pending swapchain images.
                g_Enabled.store(false, std::memory_order_release);
            }
        }
    }
    shutdownPresentationFeedback();
    if (controller) SDL_GameControllerClose(controller);
    SDL_ShowCursor(previousCursor);
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
    compositor.sampleScanoutPath();
    g_ScanoutPath = compositor.path();
    g_ScanoutEvidence = compositor.evidence();
    exportCsv();
    return result;
}
} // namespace LocalVrrTest

#endif
