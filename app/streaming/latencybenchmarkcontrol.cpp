#include "latencybenchmarkcontrol.h"

#include "backend/nvcomputer.h"
#include "backend/nvhttp.h"
#include "latencyprobe.h"

#include <QDebug>
#include <QReadWriteLock>
#include <QThread>
#include <QXmlStreamReader>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace LatencyBenchmarkControl {
namespace {

constexpr int kControlTimeoutMs = 5000;
constexpr std::size_t kMaxPendingSampleRequests = 16;

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

std::mutex& sampleMutex()
{
    static auto* mutex = new std::mutex;
    return *mutex;
}

std::uint64_t& sampleGeneration()
{
    static auto* generation = new std::uint64_t(0);
    return *generation;
}

bool isSampleGenerationCurrent(std::uint64_t generation)
{
    std::scoped_lock lock(sampleMutex());
    return generation == sampleGeneration();
}

bool deliverSampleIfCurrent(std::uint64_t generation,
                            std::uint64_t sequence,
                            std::uint64_t waitUs)
{
    // Keep generation validation and delivery atomic with respect to benchmark
    // restart/stop. This prevents a response from an earlier run from being
    // applied after sequence numbers restart at 1.
    std::scoped_lock lock(sampleMutex());
    if (generation != sampleGeneration()) {
        return false;
    }

    LatencyProbe::instance().onCadenceWait(sequence, waitUs);
    return true;
}

void performSampleRequest(const HostConfig& config,
                          std::uint64_t sequence,
                          std::uint64_t generation)
{
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
            (void) deliverSampleIfCurrent(generation, sequence, waitUs);
        }
        else {
            qDebug() << "Latency benchmark cadence wait unavailable for sequence"
                     << static_cast<qulonglong>(sequence);
        }
    }
    catch (const std::exception& e) {
        qDebug() << "Latency benchmark sample request unavailable:" << e.what();
    }
}

struct SampleRequest {
    HostConfig config;
    std::uint64_t sequence = 0;
    std::uint64_t generation = 0;
};

std::condition_variable& sampleCondition()
{
    static auto* condition = new std::condition_variable;
    return *condition;
}

std::deque<SampleRequest>& sampleQueue()
{
    static auto* queue = new std::deque<SampleRequest>;
    return *queue;
}

QThread* sampleWorker()
{
    static QThread* worker = []() {
        QThread* thread = QThread::create([]() {
            for (;;) {
                SampleRequest request;
                {
                    std::unique_lock lock(sampleMutex());
                    sampleCondition().wait(lock, []() {
                        return !sampleQueue().empty();
                    });
                    request = std::move(sampleQueue().front());
                    sampleQueue().pop_front();
                }

                if (!isSampleGenerationCurrent(request.generation)) {
                    continue;
                }

                performSampleRequest(request.config,
                                     request.sequence,
                                     request.generation);
            }
        });

        thread->setObjectName(QStringLiteral("LatencyBenchmarkSample"));
        thread->start();
        return thread;
    }();

    return worker;
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
    {
        std::scoped_lock lock(sampleMutex());
        ++sampleGeneration();
        sampleQueue().clear();
    }
    requestAsync("start");
}

void sampleAsync(std::uint64_t sequence)
{
    const HostConfig config = copyHostConfig();
    if (!config.valid || sequence == 0) {
        return;
    }

    (void) sampleWorker();
    {
        std::scoped_lock lock(sampleMutex());
        // Keep the worker bounded if the host endpoint stalls. The oldest
        // correction is least useful once newer measurements are available.
        if (sampleQueue().size() >= kMaxPendingSampleRequests) {
            sampleQueue().pop_front();
        }
        sampleQueue().push_back({config, sequence, sampleGeneration()});
    }
    sampleCondition().notify_one();
}

void stopAsync()
{
    {
        std::scoped_lock lock(sampleMutex());
        ++sampleGeneration();
        sampleQueue().clear();
    }
    requestAsync("stop");
}

} // namespace LatencyBenchmarkControl
