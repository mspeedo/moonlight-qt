#include "latencybenchmarkcontrol.h"

#include "backend/nvcomputer.h"
#include "backend/nvhttp.h"
#include "latencyprobe.h"

#include <QDebug>
#include <QReadWriteLock>
#include <QThread>
#include <QXmlStreamReader>

#include <mutex>
#include <thread>
#include <utility>

namespace LatencyBenchmarkControl {
namespace {

constexpr int kControlTimeoutMs = 5000;

struct HostConfig {
    NvAddress address;
    std::uint16_t httpsPort = 0;
    QSslCertificate serverCert;
    bool useTrueUid = false;
    bool valid = false;
};

std::mutex& configMutex()
{
    static auto* mutex = new std::mutex;
    return *mutex;
}

std::mutex& operationMutex()
{
    // Process-lifetime storage is deliberate because a best-effort request may
    // still be completing during session teardown.
    static auto* mutex = new std::mutex;
    return *mutex;
}

HostConfig& hostConfig()
{
    static auto* config = new HostConfig;
    return *config;
}

HostConfig copyHostConfig()
{
    std::scoped_lock lock(configMutex());
    return hostConfig();
}

void performRequest(const HostConfig& config, const char* action)
{
    // NvHTTP owns a QNetworkAccessManager and runs a local QEventLoop. Execute
    // the request on a real QThread even though the outer request is already
    // asynchronous, so Qt networking has a normal per-thread event dispatcher.
    QThread* worker = QThread::create([config, action]() {
        try {
            NvHTTP http(config.address,
                        config.httpsPort,
                        config.serverCert,
                        config.useTrueUid);

            const QString response = http.openConnectionToString(
                        http.m_BaseUrlHttps,
                        QStringLiteral("latencybenchmark"),
                        QStringLiteral("action=") + QString::fromLatin1(action),
                        kControlTimeoutMs,
                        NvHTTP::NVLL_ERROR);
            NvHTTP::verifyResponseStatus(response);
        }
        catch (const std::exception& e) {
            // This is expected with stock Sunshine, which has no latencybenchmark
            // endpoint. Benchmark readiness never depends on this response.
            qDebug() << "Latency benchmark" << action << "request unavailable:" << e.what();
        }
    });

    worker->start();
    worker->wait();
    delete worker;
}

void performSampleRequest(const HostConfig& config, std::uint64_t sequence)
{
    QThread* worker = QThread::create([config, sequence]() {
        try {
            NvHTTP http(config.address,
                        config.httpsPort,
                        config.serverCert,
                        config.useTrueUid);

            const QString response = http.openConnectionToString(
                        http.m_BaseUrlHttps,
                        QStringLiteral("latencybenchmark"),
                        QStringLiteral("action=sample&sequence=") +
                            QString::number(static_cast<qulonglong>(sequence)),
                        kControlTimeoutMs,
                        NvHTTP::NVLL_ERROR);
            NvHTTP::verifyResponseStatus(response);

            bool sampleReady = false;
            std::uint64_t responseSequence = 0;
            std::uint64_t waitUs = 0;
            bool sequenceValid = false;
            bool waitValid = false;

            QXmlStreamReader xml(response);
            while (!xml.atEnd()) {
                xml.readNext();
                if (!xml.isStartElement()) {
                    continue;
                }

                if (xml.name() == QStringLiteral("latencybenchmark")) {
                    sampleReady = xml.readElementText() == QStringLiteral("sample");
                }
                else if (xml.name() == QStringLiteral("sequence")) {
                    bool ok = false;
                    const qulonglong value = xml.readElementText().toULongLong(&ok);
                    if (ok) {
                        responseSequence = static_cast<std::uint64_t>(value);
                        sequenceValid = true;
                    }
                }
                else if (xml.name() == QStringLiteral("wait_us")) {
                    bool ok = false;
                    const qulonglong value = xml.readElementText().toULongLong(&ok);
                    if (ok) {
                        waitUs = static_cast<std::uint64_t>(value);
                        waitValid = true;
                    }
                }
            }

            if (!xml.hasError() && sampleReady && sequenceValid && waitValid &&
                    responseSequence == sequence) {
                LatencyProbe::instance().onCadenceWait(sequence, waitUs);
            }
            else {
                qDebug() << "Latency benchmark cadence wait unavailable for sequence"
                         << static_cast<qulonglong>(sequence);
            }
        }
        catch (const std::exception& e) {
            qDebug() << "Latency benchmark sample request unavailable:" << e.what();
        }
    });

    worker->start();
    worker->wait();
    delete worker;
}

void perform(const char* action)
{
    const HostConfig config = copyHostConfig();
    if (!config.valid) {
        return;
    }

    // Serialize START and STOP so a release/teardown cannot overtake a still
    // outstanding START request to modified Sunshine.
    std::scoped_lock operationLock(operationMutex());
    performRequest(config, action);
}

void requestAsync(const char* action)
{
    std::thread([action]() {
        perform(action);
    }).detach();
}

} // namespace

void configure(NvComputer* computer)
{
    HostConfig config;

    if (computer != nullptr) {
        QReadLocker lock(&computer->lock);
        config.address = computer->activeAddress;
        config.httpsPort = computer->activeHttpsPort;
        config.serverCert = computer->serverCert;
        config.useTrueUid = !computer->isNvidiaServerSoftware;
        config.valid = !config.address.isNull() &&
                       config.httpsPort != 0 &&
                       !config.serverCert.isNull();
    }

    std::scoped_lock lock(configMutex());
    hostConfig() = std::move(config);
}

void startAsync()
{
    requestAsync("start");
}

void sampleAsync(std::uint64_t sequence)
{
    std::thread([sequence]() {
        const HostConfig config = copyHostConfig();
        if (config.valid) {
            performSampleRequest(config, sequence);
        }
    }).detach();
}

void stopAsync()
{
    requestAsync("stop");
}

} // namespace LatencyBenchmarkControl
