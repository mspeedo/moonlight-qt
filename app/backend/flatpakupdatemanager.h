#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QVariantList>

class QNetworkAccessManager;

// Flatpak portal updates to latest; known historical commits use the existing
// host-spawn permission to run a pinned Flatpak update.
class FlatpakUpdateManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool supported READ supported CONSTANT)
    Q_PROPERTY(QString runningVersion READ runningVersion CONSTANT)
    Q_PROPERTY(bool checking READ checking NOTIFY changed)
    Q_PROPERTY(bool installing READ installing NOTIFY changed)
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY changed)
    Q_PROPERTY(bool canInstall READ canInstall NOTIFY changed)
    Q_PROPERTY(bool restartRequired READ restartRequired NOTIFY changed)
    Q_PROPERTY(int progress READ progress NOTIFY changed)
    Q_PROPERTY(QString runningCommit READ runningCommit NOTIFY changed)
    Q_PROPERTY(QString latestCommit READ latestCommit NOTIFY changed)
    Q_PROPERTY(QString latestGitCommit READ latestGitCommit NOTIFY changed)
    Q_PROPERTY(QVariantList versions READ versions NOTIFY changed)
    Q_PROPERTY(bool historyLoading READ historyLoading NOTIFY changed)
    Q_PROPERTY(QString historyError READ historyError NOTIFY changed)
    Q_PROPERTY(bool installingHistorical READ installingHistorical NOTIFY changed)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed)
    Q_PROPERTY(QString errorText READ errorText NOTIFY changed)

public:
    explicit FlatpakUpdateManager(QObject* parent = nullptr);

    bool supported() const { return m_supported; }
    QString runningVersion() const { return m_runningVersion; }
    bool checking() const { return m_checking; }
    bool installing() const { return m_installing; }
    bool updateAvailable() const { return m_updateAvailable; }
    bool canInstall() const { return m_portalReady && m_updateAvailable && !m_installing && !m_checking && !m_restartRequired; }
    bool restartRequired() const { return m_restartRequired; }
    int progress() const { return m_progress; }
    QString runningCommit() const { return m_runningCommit; }
    QString latestCommit() const { return m_latestCommit; }
    QString latestGitCommit() const { return m_latestGitCommit; }
    QVariantList versions() const { return m_versions; }
    bool historyLoading() const { return m_historyLoading; }
    QString historyError() const { return m_historyError; }
    bool installingHistorical() const { return m_installingHistorical; }
    QString statusText() const { return m_statusText; }
    QString errorText() const { return m_errorText; }

    Q_INVOKABLE void checkForUpdates();
    Q_INVOKABLE void installLatest();
    Q_INVOKABLE void installVersion(const QString& commit);
    Q_INVOKABLE bool canInstallVersion(const QString& commit) const;

signals:
    void changed();

private slots:
    void onUpdateAvailable(QVariantMap info);
    void onProgress(QVariantMap info);

private:
    void setError(const QString& message);
    void setStatus(const QString& message);
    void disconnectMonitor();
    void updateVersionLabels();

    bool m_supported = false;
    bool m_portalReady = false;
    bool m_checking = false;
    bool m_installing = false;
    bool m_installingHistorical = false;
    bool m_historyLoading = false;
    bool m_updateAvailable = false;
    bool m_restartRequired = false;
    int m_progress = 0;
    quint64 m_generation = 0;
    QString m_monitorPath;
    QString m_runningVersion;
    QString m_runningCommit;
    QString m_localCommit;
    QString m_latestCommit;
    QString m_latestGitCommit;
    QString m_latestPublishedAt;
    QVariantList m_versions;
    QString m_historyError;
    QNetworkAccessManager* m_network = nullptr;
    QString m_statusText;
    QString m_errorText;
};
