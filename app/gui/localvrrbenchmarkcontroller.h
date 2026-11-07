#pragma once

#include <QObject>
#include <QPointer>
#include <QVariantMap>
#include <QQuickWindow>

// UI entry point for the explicitly opt-in local render diagnostic.
// No Session object or streaming preference is created or modified.
class LocalVrrBenchmarkController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(QVariantMap results READ results NOTIFY resultsChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
public:
    explicit LocalVrrBenchmarkController(QObject* parent = nullptr);

    bool running() const { return m_Running; }
    QVariantMap results() const { return m_Results; }
    QString status() const { return m_Status; }

    Q_INVOKABLE void start(QObject* guiWindow, bool variableFps, bool acquireLate,
                           int durationSeconds, bool showHud);

signals:
    void runningChanged();
    void resultsChanged();
    void statusChanged();
    void finished();

private:
    void setStatus(const QString& status);

    bool m_Running = false;
    QPointer<QQuickWindow> m_GuiWindow;
    QVariantMap m_Results;
    QString m_Status = QStringLiteral("Ready");
};
