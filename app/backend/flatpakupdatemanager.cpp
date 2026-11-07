#include "flatpakupdatemanager.h"

#include <QFile>
#include <QTimer>

#if defined(Q_OS_LINUX) && !defined(STEAM_LINK)
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#endif

namespace {
#if defined(Q_OS_LINUX) && !defined(STEAM_LINK)
constexpr auto kPortalService = "org.freedesktop.portal.Flatpak";
constexpr auto kPortalPath = "/org/freedesktop/portal/Flatpak";
constexpr auto kPortalInterface = "org.freedesktop.portal.Flatpak";
constexpr auto kMonitorInterface = "org.freedesktop.portal.Flatpak.UpdateMonitor";
#endif
}

FlatpakUpdateManager::FlatpakUpdateManager(QObject* parent)
    : QObject(parent),
      m_runningVersion(QString::fromLatin1(VERSION_STR)),
      m_statusText(tr("Select Check for updates to ask Flatpak for a newer build."))
{
#if defined(Q_OS_LINUX) && !defined(STEAM_LINK)
    // UpdateMonitor only works for Flatpak-installed applications.
    m_supported = QFile::exists(QStringLiteral("/.flatpak-info"));
#endif
}

void FlatpakUpdateManager::setStatus(const QString& message)
{
    m_statusText = message;
    emit changed();
}

void FlatpakUpdateManager::setError(const QString& message)
{
    m_checking = false;
    m_installing = false;
    m_errorText = message;
    m_statusText = tr("Update operation failed.");
    emit changed();
}

void FlatpakUpdateManager::disconnectMonitor()
{
#if defined(Q_OS_LINUX) && !defined(STEAM_LINK)
    if (m_monitorPath.isEmpty()) {
        return;
    }

    auto bus = QDBusConnection::sessionBus();
    bus.disconnect(QString::fromLatin1(kPortalService), m_monitorPath,
                   QString::fromLatin1(kMonitorInterface), QStringLiteral("UpdateAvailable"),
                   this, SLOT(onUpdateAvailable(QVariantMap)));
    bus.disconnect(QString::fromLatin1(kPortalService), m_monitorPath,
                   QString::fromLatin1(kMonitorInterface), QStringLiteral("Progress"),
                   this, SLOT(onProgress(QVariantMap)));

    QDBusMessage close = QDBusMessage::createMethodCall(
        QString::fromLatin1(kPortalService), m_monitorPath,
        QString::fromLatin1(kMonitorInterface), QStringLiteral("Close"));
    bus.asyncCall(close);
    m_monitorPath.clear();
#endif
}

void FlatpakUpdateManager::checkForUpdates()
{
#if !defined(Q_OS_LINUX) || defined(STEAM_LINK)
    setError(tr("In-app updates are available only in the Linux Flatpak."));
#else
    if (!m_supported) {
        setError(tr("Install Moonlight from its Flatpak repository to enable in-app updates."));
        return;
    }
    if (m_installing || m_checking) {
        return;
    }

    ++m_generation;
    const quint64 generation = m_generation;
    disconnectMonitor();
    m_errorText.clear();
    m_updateAvailable = false;
    m_latestCommit.clear();
    m_checking = true;
    m_statusText = tr("Checking for available Flatpak updates...");
    emit changed();

    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        setError(tr("Cannot connect to the session D-Bus. Update portal unavailable."));
        return;
    }

    // Subscribe BEFORE CreateUpdateMonitor returns, avoiding a race with the
    // first UpdateAvailable signal. Flatpak defines this deterministic path.
    QString sender = bus.baseService();
    sender.remove(QLatin1Char(':'));
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    const QString token = QStringLiteral("moonlight_%1").arg(generation);
    const QString path = QStringLiteral("%1/update_monitor/%2/%3")
                             .arg(QString::fromLatin1(kPortalPath), sender, token);
    m_monitorPath = path;
    if (!bus.connect(QString::fromLatin1(kPortalService), path,
                     QString::fromLatin1(kMonitorInterface), QStringLiteral("UpdateAvailable"),
                     this, SLOT(onUpdateAvailable(QVariantMap))) ||
        !bus.connect(QString::fromLatin1(kPortalService), path,
                     QString::fromLatin1(kMonitorInterface), QStringLiteral("Progress"),
                     this, SLOT(onProgress(QVariantMap)))) {
        disconnectMonitor();
        setError(tr("Unable to subscribe to the Flatpak update monitor."));
        return;
    }

    QVariantMap options;
    options.insert(QStringLiteral("handle_token"), token);
    QDBusMessage create = QDBusMessage::createMethodCall(
        QString::fromLatin1(kPortalService), QString::fromLatin1(kPortalPath),
        QString::fromLatin1(kPortalInterface), QStringLiteral("CreateUpdateMonitor"));
    create << options;

    auto* watcher = new QDBusPendingCallWatcher(bus.asyncCall(create, 15000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, generation](QDBusPendingCallWatcher* call) {
        QDBusPendingReply<QDBusObjectPath> result = *call;
        call->deleteLater();
        if (generation != m_generation) {
            return;
        }
        if (result.isError()) {
            disconnectMonitor();
            setError(tr("Flatpak update portal: %1").arg(result.error().message()));
            return;
        }
        if (result.value().path() != m_monitorPath) {
            disconnectMonitor();
            setError(tr("Flatpak returned an unexpected update monitor path."));
            return;
        }
    });

    // This portal is notification-based; Flatpak may refresh its remote
    // on a timer. Do not misrepresent a timeout as proof of no update.
    QTimer::singleShot(12000, this, [this, generation] {
        if (generation != m_generation || !m_checking) {
            return;
        }
        m_checking = false;
        m_statusText = tr("No update announced yet. Flatpak may still be refreshing the repository.");
        emit changed();
    });
#endif
}

void FlatpakUpdateManager::onUpdateAvailable(QVariantMap info)
{
    m_checking = false;
    m_errorText.clear();
    m_runningCommit = info.value(QStringLiteral("running-commit")).toString();
    const QString local = info.value(QStringLiteral("local-commit")).toString();
    m_latestCommit = info.value(QStringLiteral("remote-commit")).toString();

    // The instance may already be running an older deployment than the one
    // currently installed. In that case it only needs a restart.
    m_restartRequired = !m_runningCommit.isEmpty() &&
                        !local.isEmpty() && local != m_runningCommit;

    m_updateAvailable = !m_latestCommit.isEmpty() &&
                        !local.isEmpty() && m_latestCommit != local;
    if (m_updateAvailable) {
        m_statusText = tr("New Flatpak build available for installation.");
    } else if (m_restartRequired) {
        m_statusText = tr("A new build is already installed. Restart Moonlight when convenient.");
    } else {
        m_statusText = tr("Moonlight is up to date according to Flatpak.");
    }
    emit changed();
}

void FlatpakUpdateManager::installLatest()
{
#if !defined(Q_OS_LINUX) || defined(STEAM_LINK)
    setError(tr("This feature requires a Linux Flatpak installation."));
#else
    if (!m_supported || !m_updateAvailable || m_monitorPath.isEmpty() ||
        m_installing || m_checking) {
        return;
    }
    m_errorText.clear();
    m_installing = true;
    m_progress = 0;
    m_statusText = tr("Installing update using the Flatpak portal...");
    emit changed();

    QDBusMessage update = QDBusMessage::createMethodCall(
        QString::fromLatin1(kPortalService), m_monitorPath,
        QString::fromLatin1(kMonitorInterface), QStringLiteral("Update"));
    update << QString() << QVariantMap();
    auto* watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(update, 30000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher* call) {
        QDBusPendingReply<> result = *call;
        call->deleteLater();
        if (result.isError()) {
            setError(tr("Cannot install the Flatpak update: %1").arg(result.error().message()));
        }
    });
#endif
}

void FlatpakUpdateManager::onProgress(QVariantMap info)
{
    if (!m_installing) {
        return;
    }
    const uint status = info.value(QStringLiteral("status")).toUInt();
    const uint count = qMax(1u, info.value(QStringLiteral("n_ops")).toUInt());
    const uint step = qMin(info.value(QStringLiteral("op")).toUInt(), count - 1);
    const uint part = qMin(info.value(QStringLiteral("progress")).toUInt(), 100u);

    if (status == 0) {
        m_progress = qMin(100, int((step * 100 + part) / count));
        m_statusText = tr("Downloading and installing update...");
    } else if (status == 2) {
        m_installing = false;
        m_checking = false;
        m_updateAvailable = false;
        m_restartRequired = true;
        m_progress = 100;
        m_statusText = tr("Update installed. Close Moonlight and launch it from Steam when ready.");
    } else if (status == 1) {
        m_installing = false;
        m_updateAvailable = false;
        m_statusText = tr("No additional update was available for installation.");
    } else if (status == 3) {
        setError(info.value(QStringLiteral("error_message")).toString());
        return;
    }
    emit changed();
}
