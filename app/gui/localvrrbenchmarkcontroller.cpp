#include "localvrrbenchmarkcontroller.h"
#include "streaming/video/localvrrtest.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QDebug>
#include <QWindow>
#include <QStringList>

LocalVrrBenchmarkController::LocalVrrBenchmarkController(QObject* parent)
    : QObject(parent)
{
}

void LocalVrrBenchmarkController::setStatus(const QString& status)
{
    if (m_Status == status) return;
    m_Status = status;
    emit statusChanged();
}

void LocalVrrBenchmarkController::clearHistory()
{
    if (m_Running) return;
    m_History.clear();
    m_Results.clear();
    emit historyChanged();
    emit resultsChanged();
}

void LocalVrrBenchmarkController::start(QObject* guiWindow, bool variableFps,
                                         bool acquireLate, int durationSeconds,
                                         bool showHud)
{
#if defined(Q_OS_LINUX) && defined(HAVE_LIBPLACEBO_VULKAN)
    if (m_Running) return;
    auto* window = qobject_cast<QQuickWindow*>(guiWindow);
    if (!window) {
        setStatus(QStringLiteral("Moonlight window not available"));
        return;
    }
    if (durationSeconds != 0 && durationSeconds != 30 &&
        durationSeconds != 60 && durationSeconds != 120) {
        setStatus(QStringLiteral("Invalid benchmark duration"));
        return;
    }

    m_GuiWindow = window;
    m_Running = true;
    emit runningChanged();
    setStatus(QStringLiteral("Preparing Vulkan benchmark..."));
    m_Results.clear();
    emit resultsChanged();

    QStringList options;
    options << QStringLiteral("--local-vrr-test");
    if (!variableFps) options << QStringLiteral("--vrr-constant");
    if (acquireLate) options << QStringLiteral("--vrr-acquire-late");
    if (!showHud) options << QStringLiteral("--vrr-hud-off");
    if (durationSeconds > 0)
        options << QStringLiteral("--vrr-duration=%1").arg(durationSeconds);

    // Run SDL in the current Moonlight/Steam Game Mode process: unlike an
    // external launch, this retains Steam's app identity, gamepad mapping and
    // Gamescope environment. The Qt window is hidden (not destroyed), so SDL's
    // fullscreen Vulkan window receives focus. Restore it on return.
    QTimer::singleShot(0, this, [this, options, variableFps, acquireLate, durationSeconds]() {
        QWindow::Visibility visibility = QWindow::Windowed;
        if (m_GuiWindow) {
            visibility = m_GuiWindow->visibility();
            m_GuiWindow->hide();
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        }

        const int exitCode = LocalVrrTest::run(options);
        const auto stats = LocalVrrTest::snapshot();
        QVariantMap report;
        report.insert(QStringLiteral("produced"), qulonglong(stats.produced));
        report.insert(QStringLiteral("submitted"), qulonglong(stats.submitted));
        report.insert(QStringLiteral("dropped"), qulonglong(stats.dropped));
        report.insert(QStringLiteral("presented"), qulonglong(stats.presented));
        report.insert(QStringLiteral("discarded"), qulonglong(stats.discarded));
        report.insert(QStringLiteral("produceAvg"), stats.produceAvgMs);
        report.insert(QStringLiteral("acquireAvg"), stats.acquireAvgMs);
        report.insert(QStringLiteral("renderAvg"), stats.renderAvgMs);
        report.insert(QStringLiteral("submitAvg"), stats.submitAvgMs);
        report.insert(QStringLiteral("presentAvg"), stats.presentAvgMs);
        report.insert(QStringLiteral("produceMax"), stats.producePeakMs);
        report.insert(QStringLiteral("acquireMax"), stats.acquirePeakMs);
        report.insert(QStringLiteral("renderMax"), stats.renderPeakMs);
        report.insert(QStringLiteral("submitMax"), stats.submitPeakMs);
        report.insert(QStringLiteral("presentMax"), stats.presentPeakMs);
        report.insert(QStringLiteral("csv"), LocalVrrTest::lastCsvPath());
        report.insert(QStringLiteral("exitCode"), exitCode);
        report.insert(QStringLiteral("mode"), variableFps
                      ? QStringLiteral("Variable 80-110 FPS")
                      : QStringLiteral("Constant 90 FPS"));
        report.insert(QStringLiteral("ordering"), acquireLate
                      ? QStringLiteral("Late acquire")
                      : QStringLiteral("Early acquire"));
        report.insert(QStringLiteral("duration"), durationSeconds);
        m_Results = report;
        emit resultsChanged();
        m_History.append(report);
        while (m_History.size() > 12) m_History.removeFirst();
        emit historyChanged();

        if (m_GuiWindow) {
            if (visibility == QWindow::FullScreen)
                m_GuiWindow->showFullScreen();
            else if (visibility == QWindow::Maximized)
                m_GuiWindow->showMaximized();
            else
                m_GuiWindow->show();
            m_GuiWindow->raise();
            m_GuiWindow->requestActivate();
        }
        m_Running = false;
        emit runningChanged();
        setStatus(exitCode == 0 ? QStringLiteral("Benchmark finished")
                                : QStringLiteral("Benchmark failed (code %1)").arg(exitCode));
        emit finished();
    });
#else
    Q_UNUSED(guiWindow)
    Q_UNUSED(variableFps)
    Q_UNUSED(acquireLate)
    Q_UNUSED(durationSeconds)
    Q_UNUSED(showHud)
    setStatus(QStringLiteral("Vulkan local VRR diagnostic is only available on Linux Flatpak"));
#endif
}
