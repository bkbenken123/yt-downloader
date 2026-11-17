#pragma once
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <functional>
#include <atomic>

struct ProcessResult {
    int exitCode = -1;
    bool started = false;
    bool timedOut = false;
    bool cancelled = false;
    QString output;
};

class ProcessRunner {
public:
    using OutputCallback = std::function<void(const QString &)>;
    static ProcessResult run(const QString &program, const QStringList &arguments,
                             const QString &workingDirectory = {},
                             const QProcessEnvironment &environment = QProcessEnvironment::systemEnvironment(),
                             int timeoutMs = 180000, const std::atomic_bool *cancel = nullptr,
                             OutputCallback callback = {}, bool mergeChannels = true);
    static QString findExecutable(const QStringList &names, const QStringList &extraDirectories = {});
    static QString quoteCommand(const QString &program, const QStringList &arguments);
    static void terminateTree(QProcess &process);
    static QString platformExecutableName(const QString &baseName);
};
