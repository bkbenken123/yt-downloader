#include "toolmanager.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QProcess>

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
            return buildYtdlp(log, cancel);
        }
        log(QStringLiteral("Checking yt-dlp source for updates..."));
        const ProcessResult pull = gitRun(git, {QStringLiteral("pull"), QStringLiteral("--ff-only")},
                                          source, 900000, log, cancel);
        if (pull.exitCode != 0) {
            log(QStringLiteral("WARNING: yt-dlp update failed; existing source retained: %1").arg(pull.output.right(1200)));
            return buildYtdlp(log, cancel);
        }
        const bool didChange = pull.output.contains(QStringLiteral("Updating ")) ||
                               pull.output.contains(QStringLiteral("Fast-forward"));
        if (changed) *changed = didChange;
        log(didChange ? QStringLiteral("yt-dlp source updated.") : QStringLiteral("yt-dlp is already up to date."));
        return buildYtdlp(log, cancel);
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
            return buildYtdlp(log, cancel);
        }
        QDir(staging).removeRecursively();
        log(QStringLiteral("WARNING: yt-dlp clone failed: %1").arg(clone.output.right(1200)));
    }
    log(QStringLiteral("ERROR: yt-dlp source is unavailable and Git could not clone it; no binary download fallback is used."));
    return false;
}

bool ToolManager::buildYtdlp(const Log &log, const std::atomic_bool *cancel) {
    const QString source = ytdlpSourceDirectory();
    const QString entry = QDir(source).filePath(QStringLiteral("yt_dlp/__main__.py"));
    if (!QFileInfo(entry).isFile()) {
        log(QStringLiteral("ERROR: yt-dlp source checkout is missing yt_dlp/__main__.py."));
        return false;
    }

    QStringList prefix;
    const QString python = pythonProgram(&prefix);
    if (python.isEmpty()) {
        log(QStringLiteral("ERROR: Python is required to compile yt-dlp."));
        return false;
    }

    log(QStringLiteral("Installing/refreshing PyInstaller and compiling yt-dlp from source..."));
    const ProcessResult pip = ProcessRunner::run(python,
        prefix + QStringList{QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("install"),
                            QStringLiteral("--upgrade"), QStringLiteral("pip"), QStringLiteral("pyinstaller")},
        source, QProcessEnvironment::systemEnvironment(), 900000, cancel,
        [&](const QString &line) { log(QStringLiteral("[pip] ") + line); });
    if (pip.exitCode != 0) {
        log(QStringLiteral("ERROR: Could not prepare PyInstaller: %1").arg(pip.output.right(1200)));
        return false;
    }

    const QString buildRoot = QDir(binariesDirectory()).filePath(QStringLiteral("yt-dlp-build"));
    const QString dist = QDir(buildRoot).filePath(QStringLiteral("dist"));
    const QString work = QDir(buildRoot).filePath(QStringLiteral("work"));
    QDir(dist).removeRecursively();
    QDir(work).removeRecursively();
    QDir().mkpath(dist);
    QDir().mkpath(work);

#ifdef Q_OS_WIN
    const QString outputName = QStringLiteral("yt-dlp");
    const QString outputLeaf = QStringLiteral("yt-dlp.exe");
    const QString destination = QDir(source).filePath(outputLeaf);
#else
    const QString outputName = QStringLiteral("yt-dlp_linux");
    const QString outputLeaf = QStringLiteral("yt-dlp_linux");
    const QString destination = QDir(binariesDirectory()).filePath(outputLeaf);
#endif

    const ProcessResult build = ProcessRunner::run(python,
        prefix + QStringList{QStringLiteral("-m"), QStringLiteral("PyInstaller"),
                            QStringLiteral("--noconfirm"), QStringLiteral("--clean"), QStringLiteral("--onefile"),
                            QStringLiteral("--name"), outputName,
                            QStringLiteral("--distpath"), dist,
                            QStringLiteral("--workpath"), work,
                            QStringLiteral("--specpath"), buildRoot, entry},
        source, QProcessEnvironment::systemEnvironment(), 1800000, cancel,
        [&](const QString &line) { log(QStringLiteral("[yt-dlp build] ") + line); });
    if (build.exitCode != 0) {
        log(QStringLiteral("ERROR: yt-dlp source compilation failed: %1").arg(build.output.right(1200)));
        return false;
    }

    const QString built = QDir(dist).filePath(outputLeaf);
    if (!QFileInfo(built).isFile()) {
        log(QStringLiteral("ERROR: PyInstaller did not produce %1.").arg(outputLeaf));
        return false;
    }
    QFile::remove(destination);
    if (!QFile::copy(built, destination)) {
        log(QStringLiteral("ERROR: Could not install compiled yt-dlp at %1.").arg(destination));
        return false;
    }
#ifdef Q_OS_WIN
    if (!locate().ytdlpCommand.contains(destination)) {
        log(QStringLiteral("Compiled yt-dlp runtime could not be located after installation."));
        return false;
    }
#else
    QFile destinationFile(destination);
    destinationFile.setPermissions(destinationFile.permissions() | QFileDevice::ExeOwner |
                                   QFileDevice::ExeGroup | QFileDevice::ExeOther);
#endif

    const ToolPaths paths = locate();
    if (paths.ytdlpCommand.isEmpty() || paths.ytdlpVersion.isEmpty()) {
        log(QStringLiteral("ERROR: Compiled yt-dlp runtime check failed."));
        return false;
    }
    log(QStringLiteral("Compiled yt-dlp runtime is ready: %1").arg(destination));
    return true;
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
    if (summary.success) {
        summary.rebundleScheduled = rebundleApplication(log);
        summary.success = summary.success && summary.rebundleScheduled;
        summary.changed = summary.changed || summary.rebundleScheduled;
        summary.messages << (summary.rebundleScheduled
            ? QStringLiteral("Application rebundle scheduled; the updater will replace the app and restart it.")
            : QStringLiteral("Application rebundle could not be scheduled."));
    } else {
        summary.messages << QStringLiteral("Application rebundle skipped because an update step failed.");
    }
    return summary;
}

bool ToolManager::rebundleApplication(const Log &log) {
#ifdef Q_OS_WIN
    const QString script = QDir(appDirectory()).filePath(QStringLiteral("compile.ps1"));
    if (!QFileInfo(script).isFile()) {
        log(QStringLiteral("ERROR: Application rebundle script was not found."));
        return false;
    }

    const QString base = qEnvironmentVariable("LOCALAPPDATA");
    if (base.isEmpty()) {
        log(QStringLiteral("ERROR: LOCALAPPDATA is unavailable; application rebundle could not be scheduled."));
        return false;
    }
    const QString updateDirectory = QDir(base).filePath(QStringLiteral("DownloaderUpdate"));
    if (!QDir().mkpath(updateDirectory)) {
        log(QStringLiteral("ERROR: Could not create the application update directory."));
        return false;
    }

    const QString helper = QDir(updateDirectory).filePath(QStringLiteral("rebundle-and-restart.ps1"));
    const QString finalApp = QDir(appDirectory()).filePath(QStringLiteral("downloader.exe"));
    const qint64 pid = QCoreApplication::applicationPid();
    QString scriptEscaped = script;
    scriptEscaped.replace(QStringLiteral("'"), QStringLiteral("''"));
    QString finalAppEscaped = finalApp;
    finalAppEscaped.replace(QStringLiteral("'"), QStringLiteral("''"));
    QFile helperFile(helper);
    if (!helperFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        log(QStringLiteral("ERROR: Could not create the application rebundle helper."));
        return false;
    }
    helperFile.write(QString(
        "$ErrorActionPreference = 'Stop'\n"
        "$deadline = (Get-Date).AddMinutes(2)\n"
        "while ((Get-Process -Id %1 -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250 }\n"
        "& powershell.exe -NoProfile -ExecutionPolicy Bypass -File '%2' -SkipToolBuild\n"
        "if ($LASTEXITCODE -ne 0) { throw 'Application rebundle failed' }\n"
        "Start-Process -FilePath '%3'\n").arg(pid).arg(scriptEscaped).arg(finalAppEscaped).toUtf8());
    helperFile.close();

    QProcess helperProcess;
    helperProcess.setProgram(QStringLiteral("powershell.exe"));
    helperProcess.setArguments({
        QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
        QStringLiteral("-WindowStyle"), QStringLiteral("Hidden"),
        QStringLiteral("-File"), helper
    });
    if (!helperProcess.startDetached()) {
        log(QStringLiteral("ERROR: Could not start the application rebundle helper."));
        return false;
    }
    log(QStringLiteral("Scheduled final app rebundle and restart after this process exits."));
    return true;
#else
    log(QStringLiteral("Automatic application rebundling is implemented for Windows."));
    return false;
#endif
}
