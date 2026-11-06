#include "remote_sunshine_config.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSslError>
#include <QTimer>
#include <QUrl>

RemoteSunshineConfig::RemoteSunshineConfig(QObject* parent) : QObject(parent)
{
    // Admin credentials must never be sent through a proxy.
    m_Network.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
}

void RemoteSunshineConfig::fail(const QString& message)
{
    m_Status = message;
    emit stateChanged();
}

void RemoteSunshineConfig::login(const QVariantMap& host, const QString& username, const QString& password)
{
    if (m_Busy) return;
    const auto address = host.value("address").toString();
    const auto port = host.value("port").toInt();
    m_Cert = QSslCertificate(QByteArray::fromBase64(host.value("certificate").toByteArray()), QSsl::Der);
    if (address.isEmpty() || port < 1 || port > 65535 || m_Cert.isNull() ||
        username.isEmpty() || password.isEmpty()) {
        fail(tr("Select an online, paired Sunshine PC and enter its web UI credentials."));
        return;
    }

    m_BaseUrl = QUrl();
    m_BaseUrl.setScheme("https");
    m_BaseUrl.setHost(address);
    m_BaseUrl.setPort(port);
    if (!m_BaseUrl.isValid()) {
        fail(tr("Invalid host address."));
        return;
    }

    m_Authorization = "Basic " + (username.toUtf8() + ':' + password.toUtf8()).toBase64();
    m_Csrf.clear();
    m_Connected = false;
    m_Dirty = false;
    m_Status = tr("Authenticating with Sunshine...");
    emit stateChanged();
    request("csrf", "/api/csrf-token");
}

void RemoteSunshineConfig::request(const QString& action, const QString& path,
                                   const QByteArray& method, const QJsonObject& body)
{
    if (m_Busy || m_BaseUrl.isEmpty() || m_Cert.isNull() || m_Authorization.isEmpty()) return;

    QUrl url = m_BaseUrl;
    url.setPath(path);
    QNetworkRequest req(url);
    req.setRawHeader("Authorization", m_Authorization);
    req.setRawHeader("Accept", "application/json");
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
#else
    req.setAttribute(QNetworkRequest::FollowRedirectsAttribute, false);
#endif
    QNetworkReply* reply;
    if (method == "GET") {
        reply = m_Network.get(req);
    } else {
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        req.setRawHeader("X-CSRF-Token", m_Csrf.toUtf8());
        const QByteArray content = QJsonDocument(body).toJson(QJsonDocument::Compact);
        reply = m_Network.sendCustomRequest(req, method, content);
    }

    m_Busy = true;
    emit stateChanged();

    // Sunshine uses its GameStream pairing certificate on the web UI HTTPS
    // endpoint. Trust *only that exact certificate*, not arbitrary self-signed
    // certificates, and never disable verification globally.
    // QNetworkReply::encrypted is emitted after the handshake and *before*
    // HTTP request data (including the Basic admin header) is transmitted.
    // Enforce the certificate pin even if a forged certificate is CA-trusted
    // and therefore never produces sslErrors().
    connect(reply, &QNetworkReply::encrypted, this, [this, reply]() {
        if (reply->sslConfiguration().peerCertificate() != m_Cert)
            reply->abort();
    });

    connect(reply, &QNetworkReply::sslErrors, this,
            [this, reply](const QList<QSslError>& errors) {
        if (reply->sslConfiguration().peerCertificate() != m_Cert) return;
        for (const QSslError& error : errors) {
            if (error.certificate() != m_Cert) return;
        }
        reply->ignoreSslErrors(errors);
    });

    // Avoid hanging Moonlight's UI on unavailable hosts.
    QTimer::singleShot(12000, reply, [reply]() {
        if (reply->isRunning()) reply->abort();
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, action]() {
        m_Busy = false;
        const bool certificateMatches = reply->sslConfiguration().peerCertificate() == m_Cert;
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto error = reply->error();
        const QByteArray data = reply->readAll();
        reply->deleteLater();

        if (!certificateMatches) {
            fail(tr("TLS identity mismatch: Sunshine certificate no longer matches the paired PC."));
            return;
        }

        if (action == "restart") {
            // The server is expected to terminate the HTTP connection during
            // a successful restart. We cannot infer completion from the reply.
            m_Status = tr("Restart requested. Reconnect after Sunshine starts again.");
            emit stateChanged();
            return;
        }
        if (error != QNetworkReply::NoError || statusCode < 200 || statusCode >= 300) {
            if (statusCode == 401) fail(tr("Sunshine login failed. Check your web UI username and password."));
            else if (statusCode == 403) fail(tr("Sunshine denied remote administration. Check origin_web_ui_allowed."));
            else if (statusCode == 404) fail(tr("This Sunshine build does not support the requested remote settings endpoint."));
            else fail(tr("Sunshine request failed (HTTP %1, error %2).").arg(statusCode).arg(int(error)));
            return;
        }
        onResult(action, data);
    });
}

void RemoteSunshineConfig::onResult(const QString& action, const QByteArray& data)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        fail(tr("Sunshine returned an invalid configuration response."));
        return;
    }
    const QJsonObject obj = doc.object();
    if (action == "csrf") {
        m_Csrf = obj.value("csrf_token").toString();
        if (m_Csrf.isEmpty()) {
            fail(tr("Sunshine did not issue an administration CSRF token."));
            return;
        }
        request("config", "/api/config");
    } else if (action == "config") {
        m_Values = obj;
        m_Values.remove("status");
        m_Values.remove("platform");
        m_Values.remove("version");
        m_Connected = true;
        m_Dirty = false;
        rebuildSettings();
        m_Status = tr("Sunshine settings loaded. Only explicitly configured options are shown.");
        emit stateChanged();
        request("presets", "/api/config-presets");
    } else if (action == "presets") {
        m_Presets.clear();
        for (const auto& item : obj.value("presets").toArray())
            if (item.isString()) m_Presets.append(item.toString());
        emit stateChanged();
    } else if (action == "save") {
        m_Dirty = false;
        m_Status = tr("Configuration saved to disk; restart Sunshine to apply.");
        emit stateChanged();
        if (m_RestartAfterSave) {
            m_RestartAfterSave = false;
            restart();
        }
    } else if (action == "createPreset" || action == "deletePreset") {
        m_Status = action == "createPreset" ? tr("Preset saved.") : tr("Preset deleted.");
        emit stateChanged();
        request("presets", "/api/config-presets");
    } else if (action == "switchPreset") {
        m_Dirty = false;
        m_Status = tr("Preset staged; requesting Sunshine restart...");
        emit stateChanged();
        restart();
    }
}

void RemoteSunshineConfig::rebuildSettings()
{
    m_Settings.clear();
    for (auto it = m_Values.constBegin(); it != m_Values.constEnd(); ++it) {
        QString value;
        if (it.value().isString()) value = it.value().toString();
        else value = QString::fromUtf8(QJsonDocument(QJsonArray{it.value()}).toJson(QJsonDocument::Compact));
        if (!it.value().isString() && value.startsWith('[') && value.endsWith(']'))
            value = value.mid(1, value.size() - 2);

        m_Settings.append(QVariantMap{{"key", it.key()}, {"value", value}});
    }
    emit stateChanged();
}

void RemoteSunshineConfig::setOption(const QString& key, const QString& value)
{
    static const QRegularExpression keyPattern("^[A-Za-z][A-Za-z0-9_]{0,79}$");
    if (!keyPattern.match(key).hasMatch() || key == "status" || key == "version" || key == "platform" ||
        value.contains('\n') || value.contains('\r')) {
        fail(tr("Invalid Sunshine option name or multiline value."));
        return;
    }
    m_Values.insert(key, value);
    m_Dirty = true;
    rebuildSettings();
}

void RemoteSunshineConfig::removeOption(const QString& key)
{
    if (m_Values.contains(key)) {
        m_Values.remove(key);
        m_Dirty = true;
        rebuildSettings();
    }
}

void RemoteSunshineConfig::reload()
{
    if (!m_Connected || m_Busy) return;
    request("config", "/api/config");
}

void RemoteSunshineConfig::save(bool restartAfterSave)
{
    if (!m_Connected || m_Busy) return;
    m_RestartAfterSave = restartAfterSave;
    request("save", "/api/config", "POST", m_Values);
}

void RemoteSunshineConfig::restart()
{
    if (!m_Connected || m_Busy) return;
    request("restart", "/api/restart", "POST");
}

void RemoteSunshineConfig::createPreset(const QString& name, bool overwrite)
{
    if (!m_Connected || m_Busy) return;
    request("createPreset", "/api/config-presets", "POST", {{"name", name}, {"overwrite", overwrite}});
}

void RemoteSunshineConfig::switchPreset(const QString& name)
{
    if (!m_Connected || m_Busy) return;
    request("switchPreset", "/api/config-presets/apply", "POST", {{"name", name}});
}

void RemoteSunshineConfig::deletePreset(const QString& name)
{
    if (!m_Connected || m_Busy) return;
    request("deletePreset", "/api/config-presets", "DELETE", {{"name", name}});
}
