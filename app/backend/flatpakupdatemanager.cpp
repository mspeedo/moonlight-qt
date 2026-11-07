#include "flatpakupdatemanager.h"

#include <QFile>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSettings>

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
// Public metadata only. The signed Flatpak repository remains the authority
// for actual installation, not this JSON document.
constexpr auto kLatestIndex = "https://pub-3a75f66f021b46f3a095b41d1a3c553b.r2.dev/flatpak/moonlight-latest.json";
#endif
}

FlatpakUpdateManager::FlatpakUpdateManager(QObject* parent)
    : QObject(parent),
      m_runningVersion(QString::fromLatin1(VERSION_STR)),
      m_statusText(tr("Check R2 for the latest published Flatpak build."))
{
#if defined(Q_OS_LINUX) && !defined(STEAM_LINK)
    // UpdateMonitor only works for Flatpak-installed applications.
    m_supported = QFile::exists(QStringLiteral("/.flatpak-info"));
    if (m_supported) {
        const QSettings flatpakInfo(QStringLiteral("/.flatpak-info"), QSettings::IniFormat);
        m_runningCommit = flatpakInfo.value(QStringLiteral("Instance/app-commit")).toString();
        m_localCommit = m_runningCommit;
    }
#endif
    m_network = new QNetworkAccessManager(this);
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
    m_portalReady = false;
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
    m_latestGitCommit.clear();
    m_portalReady = false;
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
        m_portalReady = true;
        emit changed();
    });

    // Unlike the portal's periodic polling, this HTTPS request is an actual
    // on-demand check for the latest successfully published R2 revision.
    QNetworkRequest request(QUrl(QString::fromLatin1(kLatestIndex)));
    request.setRawHeader("Cache-Control", "no-cache");
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    auto* reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
        const QByteArray bytes = reply->readAll();
        const QNetworkReply::NetworkError networkError = reply->error();
        const QString networkMessage = reply->errorString();
        reply->deleteLater();
        if (generation != m_generation || m_installing || !m_errorText.isEmpty()) {
            return;
        }
        if (networkError != QNetworkReply::NoError) {
            setError(tr("Could not read R2 update index: %1").arg(networkMessage));
            return;
        }
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
        const QJsonObject json = doc.object();
        const QString commit = json.value(QStringLiteral("ostree_commit")).toString();
        if (doc.isNull() || !doc.isObject() ||
            json.value(QStringLiteral("schema")).toInt() != 1 ||
            json.value(QStringLiteral("app_id")).toString() != QStringLiteral("com.moonlight_stream.Moonlight") ||
            json.value(QStringLiteral("arch")).toString() != QStringLiteral("x86_64") ||
            json.value(QStringLiteral("branch")).toString() != QStringLiteral("master") ||
            !QRegularExpression(QStringLiteral("^[a-f0-9]{64}$")).match(commit).hasMatch()) {
            setError(tr("R2 returned an invalid Moonlight update index."));
            return;
        }
        m_latestCommit = commit;
        m_latestGitCommit = json.value(QStringLiteral("git_commit")).toString();
        m_checking = false;
        m_updateAvailable = !m_runningCommit.isEmpty() && m_latestCommit != m_runningCommit;
        if (m_runningCommit.isEmpty()) {
            m_statusText = tr("Cannot read the running Flatpak commit. Install from the Moonlight R2 remote.");
        } else if (m_restartRequired) {
            m_statusText = tr("A different build is already installed. Restart Moonlight to activate it.");
        } else if (m_updateAvailable) {
            m_statusText = tr("A newer published build is available from R2.");
        } else {
            m_statusText = tr("You're running the latest published R2 build.");
        }
        emit changed();
    });

    // A stalled metadata request must not lock the page permanently.
    QTimer::singleShot(12000, this, [this, generation] {
        if (generation != m_generation || !m_checking) {
            return;
        }
        m_checking = false;
        m_statusText = tr("The R2 update index hasn't responded yet. Check your connection and try again.");
        emit changed();
    });
#endif
}

void FlatpakUpdateManager::onUpdateAvailable(QVariantMap info)
{
    const QString running = info.value(QStringLiteral("running-commit")).toString();
    const QString local = info.value(QStringLiteral("local-commit")).toString();
    const QString remote = info.value(QStringLiteral("remote-commit")).toString();
    if (!running.isEmpty()) {
        m_runningCommit = running;
    }
    if (!local.isEmpty()) {
        m_localCommit = local;
    }

    m_restartRequired = !m_runningCommit.isEmpty() &&
                        !m_localCommit.isEmpty() && m_localCommit != m_runningCommit;

    // A notification can arrive while the HTTPS request is still running.
    // Do not replace R2's newest commit with a potentially stale poll result.
    if (m_latestCommit.isEmpty() && !remote.isEmpty()) {
        m_latestCommit = remote;
        m_updateAvailable = !m_localCommit.isEmpty() && remote != m_localCommit;
        if (m_updateAvailable) {
            m_statusText = tr("Flatpak reported an available update.");
        }
    }
    if (m_restartRequired) {
        m_updateAvailable = false;
        m_statusText = tr("An update is already installed; restart Moonlight when convenient.");
    }
    emit changed();
}

void FlatpakUpdateManager::installLatest()
{
#if !defined(Q_OS_LINUX) || defined(STEAM_LINK)
    setError(tr("This feature requires a Linux Flatpak installation."));
#else
    if (!canInstall() || m_monitorPath.isEmpty()) {
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
        QDBusConnection::sessionBus().asyncCall(update, 300000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher* call) {
        QDBusPendingReply<> result = *call;
        call->deleteLater();
        if (result.isError()) {
            const QString message = result.error().message();
            // SteamOS may expose Flatpak's UpdateMonitor but have no Access
            // portal backend to show its one-time confirmation dialog.
            // Do not bypass consent: guide users to explicitly authorize
            // self-updates from the host permission store instead.
            if (message.contains(QStringLiteral("No portal support found"), Qt::CaseInsensitive)) {
                setError(tr("SteamOS cannot display Flatpak's update permission dialog. "
                            "In Desktop Mode, open Konsole and run: "
                            "flatpak permission-set flatpak updates com.moonlight_stream.Moonlight yes. "
                            "Then check for updates again."));
            } else if (message.contains(QStringLiteral("new permissions"), Qt::CaseInsensitive)) {
                // The Flatpak self-update portal refuses permission-increasing
                // upgrades even when the user has already granted updates.
                setError(tr("This release requests new Flatpak permissions, which the "
                            "self-update portal cannot approve. In Desktop Mode, "
                            "run: flatpak update --user com.moonlight_stream.Moonlight "
                            "(or --system for a system installation). "
                            "Future updates with unchanged permissions can use this page."));
            } else {
                setError(tr("Cannot install the Flatpak update: %1").arg(message));
            }
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
        m_restartRequired = !m_runningCommit.isEmpty() &&
                            !m_localCommit.isEmpty() &&
                            m_localCommit != m_runningCommit;
        m_statusText = m_restartRequired
            ? tr("The update was already installed. Restart Moonlight to activate it.")
            : tr("Flatpak did not find an installable update. Try checking again later.");
    } else if (status == 3) {
        setError(info.value(QStringLiteral("error_message")).toString());
        return;
    }
    emit changed();
}
