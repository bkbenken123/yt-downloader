#include "toolmanager.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>

static QString gitPath() { return QStandardPaths::findExecutable(QStringLiteral("git")); }
static ProcessResult gitRun(const QString &git, const QStringList &args, const QString &cwd,
                            int timeout, const ToolManager::Log &log,
                            const std::atomic_bool *cancel) {
    return ProcessRunner::run(git, args, cwd, QProcessEnvironment::systemEnvironment(),
        timeout, cancel, [&](const QString &line) {
            if (log) log(QStringLiteral("[git] ") + line);
        });
}

bool ToolManager::copyDirectory(const QString &source, const QString &destination,
                                const QStringList &excludedNames) {
    if (!QFileInfo(source).isDir() || !QDir().mkpath(destination)) return false;
    const QFileInfoList entries = QDir(source).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo &entry : entries) {
        if (excludedNames.contains(entry.fileName())) continue;
        const QString target = QDir(destination).filePath(entry.fileName());
        if (entry.isDir() && !entry.isSymLink()) {
            if (!copyDirectory(entry.absoluteFilePath(), target, excludedNames)) return false;
        } else {
            QFile::remove(target);
            if (!QFile::copy(entry.absoluteFilePath(), target)) return false;
        }
    }
    return true;
}

bool ToolManager::updateYtdlp(const Log &log, bool *changed, const std::atomic_bool *cancel) {
    if (changed) *changed = false;
    QDir().mkpath(binariesDirectory());
    const QString source = ytdlpSourceDirectory();
    const QString entry = QDir(source).filePath(QStringLiteral("yt_dlp/__main__.py"));
    const QString git = gitPath();
    if (QFileInfo(entry).isFile()) {
        if (git.isEmpty() || !isGitCheckout(source)) {
            log(QStringLiteral("yt-dlp source exists but is not an updateable Git checkout; using the current runtime."));
            return !locate().ytdlpCommand.isEmpty();
        }
        log(QStringLiteral("Checking yt-dlp source for updates..."));
        const ProcessResult pull = gitRun(git, {QStringLiteral("pull"), QStringLiteral("--ff-only")},
                                          source, 900000, log, cancel);
        if (pull.exitCode != 0) {
            log(QStringLiteral("WARNING: yt-dlp update failed; existing source retained: %1").arg(pull.output.right(1200)));
            return !locate().ytdlpCommand.isEmpty();
        }
        const bool didChange = pull.output.contains(QStringLiteral("Updating ")) ||
                               pull.output.contains(QStringLiteral("Fast-forward"));
        if (changed) *changed = didChange;
        log(didChange ? QStringLiteral("yt-dlp source updated.") : QStringLiteral("yt-dlp is already up to date."));
        return !locate().ytdlpCommand.isEmpty();
    }

    if (!git.isEmpty()) {
        const QString staging = QDir(binariesDirectory()).filePath(QStringLiteral("yt-dlp-new"));
        const QString backup = QDir(binariesDirectory()).filePath(QStringLiteral("yt-dlp-backup"));
        QDir(staging).removeRecursively();
        QDir(backup).removeRecursively();
        log(QStringLiteral("Cloning yt-dlp source..."));
        const ProcessResult clone = gitRun(git, {QStringLiteral("clone"), QStringLiteral("--depth"), QStringLiteral("1"),
            QStringLiteral("https://github.com/yt-dlp/yt-dlp.git"), staging},
            binariesDirectory(), 900000, log, cancel);
        if (clone.exitCode == 0 && QFileInfo(QDir(staging).filePath(QStringLiteral("yt_dlp/__main__.py"))).isFile()) {
            if (QFileInfo::exists(source) && !QDir().rename(source, backup)) return false;
            if (!QDir().rename(staging, source)) {
                if (QFileInfo::exists(backup)) QDir().rename(backup, source);
                return false;
            }
            QDir(backup).removeRecursively();
            if (changed) *changed = true;
            return !locate().ytdlpCommand.isEmpty();
        }
        QDir(staging).removeRecursively();
        log(QStringLiteral("WARNING: yt-dlp clone failed: %1").arg(clone.output.right(1200)));
    }
    const ToolPaths existing = locate();
    if (!existing.ytdlpCommand.isEmpty()) {
        const ProcessResult update = ProcessRunner::run(existing.ytdlpCommand.first(),
            existing.ytdlpCommand.mid(1) + QStringList{QStringLiteral("-U")},
            {}, QProcessEnvironment::systemEnvironment(), 300000, cancel,
            [&](const QString &line) { log(QStringLiteral("[yt-dlp] ") + line); });
        if (update.exitCode == 0) log(QStringLiteral("yt-dlp executable update completed."));
        return !locate().ytdlpCommand.isEmpty();
    }
    log(QStringLiteral("ERROR: yt-dlp is not installed and could not be downloaded."));
    return false;
}

bool ToolManager::updateFFmpegSource(const Log &log, bool *changed, const std::atomic_bool *cancel) {
    if (changed) *changed = false;
    QDir().mkpath(binariesDirectory());
    const QString source = ffmpegSourceDirectory();
    const QString git = gitPath();
    if (git.isEmpty()) {
        log(QStringLiteral("ERROR: Git is required to update FFmpeg."));
        return false;
    }
    if (!QFileInfo(QDir(source).filePath(QStringLiteral("configure"))).isFile()) {
        log(QStringLiteral("FFmpeg source is missing; cloning a Git checkout..."));
        const QString staging = QDir(binariesDirectory()).filePath(QStringLiteral("ffmpeg.git-new"));
        QDir(staging).removeRecursively();
        ProcessResult clone = gitRun(git, {QStringLiteral("-c"), QStringLiteral("core.autocrlf=false"),
            QStringLiteral("clone"), QStringLiteral("--depth"), QStringLiteral("1"),
            QStringLiteral("https://git.ffmpeg.org/ffmpeg.git"), staging},
            binariesDirectory(), 1800000, log, cancel);
        if (clone.exitCode != 0) {
            QDir(staging).removeRecursively();
            clone = gitRun(git, {QStringLiteral("clone"), QStringLiteral("--depth"), QStringLiteral("1"),
                QStringLiteral("https://github.com/FFmpeg/FFmpeg.git"), staging},
                binariesDirectory(), 1800000, log, cancel);
        }
        if (clone.exitCode != 0 || !QFileInfo(QDir(staging).filePath(QStringLiteral("configure"))).isFile()) {
            QDir(staging).removeRecursively();
            log(QStringLiteral("ERROR: FFmpeg clone failed."));
            return false;
        }
        if (QFileInfo::exists(source) && !QDir(source).removeRecursively()) return false;
        if (!QDir().rename(staging, source)) return false;
        if (changed) *changed = true;
        log(QStringLiteral("FFmpeg source checkout cloned."));
        return true;
    }
    if (!isGitCheckout(source)) {
        log(QStringLiteral("FFmpeg source is a plain tree; cloning a Git checkout before replacement..."));
        const QString staging = QDir(binariesDirectory()).filePath(QStringLiteral("ffmpeg.git-new"));
        const QString backup = QDir(binariesDirectory()).filePath(QStringLiteral("ffmpeg.snapshot-backup"));
        QDir(staging).removeRecursively();
        QDir(backup).removeRecursively();
        ProcessResult clone = gitRun(git, {QStringLiteral("-c"), QStringLiteral("core.autocrlf=false"),
            QStringLiteral("clone"), QStringLiteral("--depth"), QStringLiteral("1"),
            QStringLiteral("https://git.ffmpeg.org/ffmpeg.git"), staging},
            binariesDirectory(), 1800000, log, cancel);
        if (clone.exitCode != 0) {
            QDir(staging).removeRecursively();
            clone = gitRun(git, {QStringLiteral("clone"), QStringLiteral("--depth"), QStringLiteral("1"),
                QStringLiteral("https://github.com/FFmpeg/FFmpeg.git"), staging},
                binariesDirectory(), 1800000, log, cancel);
        }
        if (clone.exitCode != 0 || !QFileInfo(QDir(staging).filePath(QStringLiteral("configure"))).isFile()) {
            QDir(staging).removeRecursively();
            log(QStringLiteral("ERROR: FFmpeg clone failed; current source was preserved."));
            return false;
        }
        if (!QDir().rename(source, backup)) return false;
        if (!QDir().rename(staging, source)) {
            QDir().rename(backup, source);
            return false;
        }
        QDir(backup).removeRecursively();
        if (changed) *changed = true;
        return true;
    }
    const ProcessResult before = gitRun(git, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")},
                                         source, 15000, {}, cancel);
    if (before.exitCode != 0) return false;
    const ProcessResult pull = gitRun(git, {QStringLiteral("pull"), QStringLiteral("--ff-only")},
                                       source, 900000, log, cancel);
    if (pull.exitCode != 0) {
        log(QStringLiteral("ERROR: FFmpeg fast-forward update failed: %1").arg(pull.output.right(1200)));
        return false;
    }
    const ProcessResult after = gitRun(git, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")},
                                        source, 15000, {}, cancel);
    if (after.exitCode != 0) return false;
    const bool didChange = before.output.trimmed() != after.output.trimmed();
    if (changed) *changed = didChange;
    log(didChange ? QStringLiteral("FFmpeg source updated; rebuild required.") : QStringLiteral("FFmpeg source is already up to date."));
    return true;
}

bool ToolManager::updateApplication(const Log &log, bool *changed, const std::atomic_bool *cancel) {
    if (changed) *changed = false;
    const QString git = gitPath();
    if (git.isEmpty() || !isGitCheckout(appDirectory())) {
        log(QStringLiteral("Application folder is not a Git checkout; automatic update skipped."));
        return true;
    }
    const ProcessResult branchResult = gitRun(git, {QStringLiteral("branch"), QStringLiteral("--show-current")},
                                                appDirectory(), 15000, {}, cancel);
    const QString branch = branchResult.output.trimmed();
    if (branch.isEmpty()) {
        log(QStringLiteral("Application is on detached HEAD; source left unchanged."));
        return true;
    }
    const ProcessResult pull = gitRun(git, {QStringLiteral("pull"), QStringLiteral("--ff-only"),
        QStringLiteral("origin"), branch}, appDirectory(), 300000, log, cancel);
    if (pull.exitCode != 0) {
        log(QStringLiteral("ERROR: Application update failed: %1").arg(pull.output.right(1200)));
        return false;
    }
    const bool didChange = pull.output.contains(QStringLiteral("Updating ")) ||
                           pull.output.contains(QStringLiteral("Fast-forward"));
    if (changed) *changed = didChange;
    log(didChange ? QStringLiteral("Application source updated. Rebuild/restart the native C++ app.")
                  : QStringLiteral("Application is already up to date."));
    return true;
}

UpdateSummary ToolManager::updateToolsAndApplication(const Log &log, const std::atomic_bool *cancel) {
    UpdateSummary summary;
    bool ffmpegChanged = false, ytdlpChanged = false, appChanged = false;
    const bool ffmpegOk = updateFFmpegSource(log, &ffmpegChanged, cancel);
    summary.success = summary.success && ffmpegOk;
    summary.changed = summary.changed || ffmpegChanged;
    if (ffmpegOk && ffmpegChanged) {
        const QString marker = QDir(binariesDirectory()).filePath(QStringLiteral("ffmpeg-build/.downloader-build-profile.json"));
        QFile::remove(marker);
        summary.ffmpegRebuilt = ensureFFmpeg(log, cancel);
        summary.success = summary.success && summary.ffmpegRebuilt;
        summary.messages << (summary.ffmpegRebuilt
            ? QStringLiteral("FFmpeg source updated and rebuilt successfully.")
            : QStringLiteral("FFmpeg source updated, but rebuilding failed."));
    } else {
        summary.messages << (ffmpegOk ? QStringLiteral("FFmpeg is already up to date.")
                                      : QStringLiteral("FFmpeg update failed."));
    }
    const bool ytdlpOk = updateYtdlp(log, &ytdlpChanged, cancel);
    summary.success = summary.success && ytdlpOk;
    summary.changed = summary.changed || ytdlpChanged;
    summary.messages << (ytdlpChanged ? QStringLiteral("yt-dlp updated successfully.")
        : ytdlpOk ? QStringLiteral("yt-dlp is already up to date.") : QStringLiteral("yt-dlp update failed."));
    const bool appOk = updateApplication(log, &appChanged, cancel);
    summary.success = summary.success && appOk;
    summary.changed = summary.changed || appChanged;
    summary.messages << (appChanged ? QStringLiteral("Application source updated; rebuild and restart.")
                                    : QStringLiteral("Application source checked."));
    return summary;
}
