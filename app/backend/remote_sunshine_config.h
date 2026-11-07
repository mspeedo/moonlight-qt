#pragma once

#include <QObject>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QSslCertificate>
#include <QVariantList>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>

// Sunshine's existing web administration API, using the pinned certificate
// obtained during Moonlight pairing. A short-lived API grant comes from the
// paired-client mTLS GameStream endpoint. No web-admin password is stored.
class RemoteSunshineConfig : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY stateChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(QString platform READ platform NOTIFY stateChanged)
    Q_PROPERTY(QVariantList settings READ settings NOTIFY stateChanged)
    Q_PROPERTY(QStringList presets READ presets NOTIFY stateChanged)

public:
    explicit RemoteSunshineConfig(QObject* parent = nullptr);
    bool busy() const { return m_Busy; }
    bool connected() const { return m_Connected; }
    bool dirty() const { return m_Dirty; }
    QString status() const { return m_Status; }
    QString platform() const { return m_Platform; }
    QVariantList settings() const { return m_Settings; }
    QStringList presets() const { return m_Presets; }

    Q_INVOKABLE void connectPaired(const QVariantMap& host);
    Q_INVOKABLE void close();
    Q_INVOKABLE void reload();
    Q_INVOKABLE void setOption(const QString& key, const QString& value);
    Q_INVOKABLE void removeOption(const QString& key);
    Q_INVOKABLE void save(bool restart = false);
    Q_INVOKABLE void restart();
    Q_INVOKABLE void createPreset(const QString& name, bool overwrite = false);
    Q_INVOKABLE void switchPreset(const QString& name);
    Q_INVOKABLE void deletePreset(const QString& name);

signals:
    void stateChanged();

private:
    void request(const QString& action, const QString& path, const QByteArray& method = "GET",
                 const QJsonObject& body = {});
    void onResult(const QString& action, const QByteArray& data);
    void rebuildSettings();
    void fail(const QString& message);

    QNetworkAccessManager m_Network;
    QPointer<QNetworkReply> m_ActiveReply;
    QUrl m_BaseUrl;
    QUrl m_GameStreamUrl;
    QSslCertificate m_Cert;
    QByteArray m_Authorization; // ephemeral bearer token, RAM only
    QString m_Csrf;
    QJsonObject m_Values;
    QVariantList m_Settings;
    QStringList m_Presets;
    QString m_Status;
    QString m_Platform;
    bool m_Busy = false;
    bool m_Closing = false;
    bool m_Connected = false;
    bool m_Dirty = false;
    bool m_RestartAfterSave = false;
    bool m_RefreshAttempted = false;
    QString m_RetryAction;
    QString m_RetryPath;
    QByteArray m_RetryMethod;
    QJsonObject m_RetryBody;
};
