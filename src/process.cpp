#include "process.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

ProcessResult ProcessRunner::run(const QString &program, const QStringList &arguments,
                                 const QString &workingDirectory,
                                 const QProcessEnvironment &environment, int timeoutMs,
                                 const std::atomic_bool *cancel, OutputCallback callback,
                                 bool mergeChannels) {
    ProcessResult result;
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    QProcessEnvironment processEnvironment = environment;
#if defined(Q_OS_LINUX)
    const QFileInfo programInfo(program);
    QDir binaryDirectory(programInfo.absolutePath());
    const QString siblingLibraryDirectory = QDir::cleanPath(
        binaryDirectory.filePath(QStringLiteral("../install/lib")));
    if (binaryDirectory.dirName() == QStringLiteral("bin") &&
        QFileInfo(siblingLibraryDirectory).isDir()) {
        const QString current = processEnvironment.value(QStringLiteral("LD_LIBRARY_PATH"));
        processEnvironment.insert(QStringLiteral("LD_LIBRARY_PATH"),
            siblingLibraryDirectory + (current.isEmpty() ? QString() : QDir::listSeparator() + current));
    }
#endif
    process.setProcessEnvironment(processEnvironment);
    if (!workingDirectory.isEmpty()) process.setWorkingDirectory(workingDirectory);
    process.setInputChannelMode(QProcess::ManagedInputChannel);
    process.setProcessChannelMode(mergeChannels ? QProcess::MergedChannels : QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted(15000)) {
        result.output = process.errorString();
        return result;
    }
    result.started = true;
    QElapsedTimer timer;
    timer.start();
    auto drain = [&]() {
        QByteArray bytes = process.readAllStandardOutput();
        if (!mergeChannels) bytes += process.readAllStandardError();
        if (bytes.isEmpty()) return;
        const QString chunk = QString::fromUtf8(bytes);
        result.output += chunk;
        if (callback) {
            const QStringList lines = chunk.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
            for (const QString &line : lines) callback(line);
        }
    };
    while (process.state() != QProcess::NotRunning) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            result.cancelled = true;
            terminateTree(process);
            break;
        }
        if (timeoutMs > 0 && timer.elapsed() > timeoutMs) {
            result.timedOut = true;
            terminateTree(process);
            break;
        }
        process.waitForReadyRead(100);
        drain();
    }
    drain();
    if (process.state() != QProcess::NotRunning) {
        process.waitForFinished(5000);
        drain();
    }
    result.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    if (result.cancelled || result.timedOut) result.exitCode = -1;
    return result;
}

QString ProcessRunner::findExecutable(const QStringList &names, const QStringList &extraDirectories) {
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList roots = extraDirectories;
    roots << QDir(appDir).filePath(QStringLiteral("binaries")) << appDir;
    for (const QString &root : roots) {
        for (const QString &name : names) {
            const QString path = QDir(root).filePath(name);
            if (QFileInfo::exists(path) && QFileInfo(path).isFile())
                return QFileInfo(path).absoluteFilePath();
        }
    }
    for (const QString &name : names) {
        const QString found = QStandardPaths::findExecutable(name);
        if (!found.isEmpty()) return found;
    }
    return {};
}

QString ProcessRunner::quoteCommand(const QString &program, const QStringList &arguments) {
    auto quote = [](const QString &value) {
        QString v = value;
        v.replace(QLatin1Char('"'), QStringLiteral("\\\""));
        if (v.contains(QRegularExpression(QStringLiteral("[\\s\\\"&|<>^]"))))
            return QStringLiteral("\"%1\"").arg(v);
        return v;
    };
    QStringList parts{quote(program)};
    for (const QString &argument : arguments) parts << quote(argument);
    return parts.join(QLatin1Char(' '));
}

void ProcessRunner::terminateTree(QProcess &process) {
    if (process.state() == QProcess::NotRunning) return;
#ifdef Q_OS_WIN
    const QString taskkill = QDir(qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows")))
                                 .filePath(QStringLiteral("System32/taskkill.exe"));
    if (QFileInfo::exists(taskkill)) {
        QProcess killer;
        killer.start(taskkill, {QStringLiteral("/PID"), QString::number(process.processId()),
                                QStringLiteral("/T"), QStringLiteral("/F")});
        killer.waitForStarted(2000);
        killer.waitForFinished(5000);
    }
#endif
    if (process.state() != QProcess::NotRunning) process.terminate();
    if (!process.waitForFinished(2500)) {
        process.kill();
        process.waitForFinished(2500);
    }
}

QString ProcessRunner::platformExecutableName(const QString &baseName) {
#ifdef Q_OS_WIN
    return baseName.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)
               ? baseName : baseName + QStringLiteral(".exe");
#else
    return baseName;
#endif
}
