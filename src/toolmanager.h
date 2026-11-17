#pragma once
#include "process.h"
#include <QString>
#include <QStringList>
#include <functional>
#include <atomic>

struct ToolPaths {
    QString ffmpeg;
    QString ffprobe;
    QStringList ytdlpCommand;
    QString ytdlpVersion;
};

struct UpdateSummary {
    bool success = true;
    bool changed = false;
    bool ffmpegRebuilt = false;
    QStringList messages;
};

class ToolManager {
public:
    using Log = std::function<void(const QString &)>;
    static QString appDirectory();
    static QString binariesDirectory();
    static QString ffmpegSourceDirectory();
    static QString ffmpegOutputBinDirectory();
    static QString ytdlpSourceDirectory();
    static ToolPaths locate(QString *diagnostic = nullptr);
    static bool prepare(const Log &log, const std::atomic_bool *cancel = nullptr);
    static UpdateSummary updateToolsAndApplication(const Log &log, const std::atomic_bool *cancel = nullptr);
    static bool isGitCheckout(const QString &directory);
    static QString safePlaylistTempDirectory();
    static bool runYtdlp(const QStringList &prefix, const QStringList &arguments,
                         QString *output = nullptr, int timeoutMs = 30000);
private:
    static QString pythonProgram(QStringList *prefixArguments = nullptr);
    static bool updateYtdlp(const Log &log, bool *changed, const std::atomic_bool *cancel);
    static bool updateFFmpegSource(const Log &log, bool *changed, const std::atomic_bool *cancel);
    static bool updateApplication(const Log &log, bool *changed, const std::atomic_bool *cancel);
    static bool ensureFFmpeg(const Log &log, const std::atomic_bool *cancel);
    static bool buildFFmpeg(const Log &log, const std::atomic_bool *cancel);
    static bool buildFFmpegPosix(const Log &log, const std::atomic_bool *cancel);
    static bool buildFFmpegWindows(const Log &log, const std::atomic_bool *cancel);
    static bool buildFFmpegWindowsMsvc(const Log &log, const std::atomic_bool *cancel);
    static bool verifyFFmpeg(const QString &ffmpeg, const QString &ffprobe, const Log &log);
    static bool copyDirectory(const QString &source, const QString &destination,
                              const QStringList &excludedNames = {});
};
