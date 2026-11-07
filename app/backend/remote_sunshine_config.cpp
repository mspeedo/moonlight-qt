#include "remote_sunshine_config.h"
#include "identitymanager.h"

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

void RemoteSunshineConfig::close()
{
    // Close our standalone administration connections when leaving the page,
    // without affecting ComputerManager's separate host polling connection.
    m_Closing = true;
    if (m_ActiveReply) {
        QNetworkReply* reply = m_ActiveReply.data();
        m_ActiveReply.clear();
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    m_Network.clearConnectionCache();
    m_Authorization.fill('\0');
    m_Authorization.clear();
    m_Csrf.clear();
    m_RetryAction.clear();
    m_RetryPath.clear();
    m_RetryMethod.clear();
    m_RetryBody = QJsonObject();
    m_Connected = false;
    m_Busy = false;
}

void RemoteSunshineConfig::fail(const QString& message)
{
    m_Status = message;
    emit stateChanged();
}

void RemoteSunshineConfig::connectPaired(const QVariantMap& host)
{
    if (m_Busy) return;
    m_Closing = false;
    const auto address = host.value("address").toString();
    const auto adminPort = host.value("port").toInt();
    const auto gameStreamPort = host.value("gameStreamHttpsPort").toInt();
    m_Cert = QSslCertificate(QByteArray::fromBase64(host.value("certificate").toByteArray()), QSsl::Der);
    if (address.isEmpty() || adminPort < 1 || adminPort > 65535 ||
        gameStreamPort < 1 || gameStreamPort > 65535 || m_Cert.isNull()) {
        fail(tr("Select an online, paired Sunshine PC with a valid TLS identity."));
        return;
    }

    m_BaseUrl = QUrl();
    m_BaseUrl.setScheme("https");
    m_BaseUrl.setHost(address);
    m_BaseUrl.setPort(adminPort);
    m_GameStreamUrl = m_BaseUrl;
    m_GameStreamUrl.setPort(gameStreamPort);
    if (!m_BaseUrl.isValid() || !m_GameStreamUrl.isValid()) {
        fail(tr("Invalid paired host address."));
        return;
    }

    m_Authorization.clear();
    m_Csrf.clear();
    m_Connected = false;
    m_Platform.clear();
    m_Dirty = false;
    m_RefreshAttempted = false;
    m_RetryAction.clear();
    m_Status = tr("Authorizing with existing Moonlight pairing...");
    emit stateChanged();
    request("pairedToken", "/remote-admin-token");
}

void RemoteSunshineConfig::request(const QString& action, const QString& path,
                                   const QByteArray& method, const QJsonObject& body)
{
    const bool gameStreamAuth = action == "pairedToken";
    if (m_Closing || m_Busy || m_BaseUrl.isEmpty() || m_Cert.isNull() ||
        (!gameStreamAuth && m_Authorization.isEmpty())) return;

    QUrl url = gameStreamAuth ? m_GameStreamUrl : m_BaseUrl;
    url.setPath(path);
    QNetworkRequest req(url);
    if (!gameStreamAuth) {
        req.setRawHeader("Authorization", m_Authorization);
    } else {
        // GameStream HTTPS already requires our paired client certificate.
        req.setSslConfiguration(IdentityManager::get()->getSslConfig());
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#endif
    req.setRawHeader("Accept", "application/json");
    // NvHTTP::openConnection() already disables persistent connections.
    // Reuse the same policy here: our mTLS grant request hits that same
    // GameStream HTTPS listener, which does not tolerate idle keep-alives.
    req.setRawHeader("Connection", "close");
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
    req.setAttribute(QNetworkRequest::ConnectionCacheExpiryTimeoutSecondsAttribute, -1);
#elif QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
    req.setAttribute(QNetworkRequest::ConnectionCacheExpiryTimeoutSecondsAttribute, 0);
#endif
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

    m_ActiveReply = reply;
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
        if (errors.isEmpty() || m_Cert.isNull()) return;
        for (const QSslError& error : errors) {
            if (error.certificate() != m_Cert) return;
        }
        // Accept only errors for the exact paired cert. The encrypted()
        // callback separately checks the peer certificate before HTTP data.
        reply->ignoreSslErrors(errors);
    });

    // Avoid hanging Moonlight's UI on unavailable hosts.
    QTimer::singleShot(12000, reply, [reply]() {
        if (reply->isRunning()) reply->abort();
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, action, path, method, body]() {
        m_ActiveReply.clear();
        m_Busy = false;
        if (m_Closing) {
            reply->deleteLater();
            return;
        }
        const bool certificateMatches = reply->sslConfiguration().peerCertificate() == m_Cert;
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto error = reply->error();
        const QByteArray data = reply->readAll();
        reply->deleteLater();

        if (!certificateMatches) {
            fail(tr("TLS identity mismatch: Sunshine certificate no longer matches the paired PC."));
            return;
        }

        if (statusCode == 401 && action != "pairedToken" && !m_RefreshAttempted) {
            // Renew an expired scoped grant automatically, then retry once.
            m_RefreshAttempted = true;
            m_RetryAction = action;
            m_RetryPath = path;
            m_RetryMethod = method;
            m_RetryBody = body;
            m_Authorization.clear();
            m_Csrf.clear();
            request("pairedToken", "/remote-admin-token");
            return;
        }
        if (action == "restart") {
            // Sunshine's restart() may terminate the HTTPS connection without
            // a response. Never report a definite successful restart here.
            if (statusCode == 401) {
                fail(tr("Sunshine rejected paired-client restart authorization."));
            } else if (statusCode == 403) {
                fail(tr("Sunshine denied the restart request."));
            } else if (statusCode >= 400) {
                fail(tr("Sunshine rejected restart (HTTP %1).").arg(statusCode));
            } else if (statusCode >= 200 && statusCode < 300) {
                fail(tr("Restart request accepted. Reconnect to verify Sunshine is running."));
            } else if (error == QNetworkReply::RemoteHostClosedError) {
                fail(tr("Sunshine closed the connection after the restart request. Reconnect to verify."));
            } else {
                fail(tr("Restart could not be confirmed (network error %1).").arg(int(error)));
            }
            return;
        }
        if (error != QNetworkReply::NoError || statusCode < 200 || statusCode >= 300) {
            if (statusCode == 401) fail(tr("Sunshine rejected Moonlight pairing authorization. Re-pair or check whether this client is enabled."));

            else if (statusCode == 403) fail(tr("Sunshine denied remote administration. Check origin_web_ui_allowed."));
            else if (statusCode == 404) fail(tr("Sunshine does not support paired remote settings. Install the matching Sunshine fork build."));

            else fail(tr("Sunshine request failed (HTTP %1, error %2).").arg(statusCode).arg(int(error)));
            return;
        }
        if (action != "pairedToken" && action != "csrf") m_RefreshAttempted = false;
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
    if (action == "pairedToken") {
        const QString token = obj.value("token").toString();
        if (token.size() != 48) {
            fail(tr("Sunshine did not issue a valid paired-client authorization token."));
            return;
        }
        m_Authorization = "Bearer " + token.toLatin1();
        m_Status = tr("Paired client verified. Loading Sunshine settings...");
        emit stateChanged();
        request("csrf", "/api/csrf-token");
    } else if (action == "csrf") {
        m_Csrf = obj.value("csrf_token").toString();
        if (m_Csrf.isEmpty()) {
            fail(tr("Sunshine did not issue an administration CSRF token."));
            return;
        }
        if (!m_RetryAction.isEmpty()) {
            const QString actionToRetry = m_RetryAction;
            const QString pathToRetry = m_RetryPath;
            const QByteArray methodToRetry = m_RetryMethod;
            const QJsonObject bodyToRetry = m_RetryBody;
            m_RetryAction.clear();
            request(actionToRetry, pathToRetry, methodToRetry, bodyToRetry);
        } else {
            request("config", "/api/config");
        }
    } else if (action == "config") {
        m_Platform = obj.value("platform").toString().toLower();
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
