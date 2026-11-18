#include "toolmanager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <QTemporaryDir>

static const QString kProfile = QStringLiteral("all-vendors-win-linux-v5-required-codecs");

QString ToolManager::appDirectory() {
    const QString bundledRoot = qEnvironmentVariable("DOWNLOADER_BUNDLED_ROOT");
    if (!bundledRoot.isEmpty() && QDir(bundledRoot).exists())
        return QDir(bundledRoot).absolutePath();
    QDir directory(QCoreApplication::applicationDirPath());
    for (int depth = 0; depth < 8; ++depth) {
        if (QDir(directory.filePath(QStringLiteral("binaries"))).exists())
            return directory.absolutePath();
        if (!directory.cdUp()) break;
    }
    return QCoreApplication::applicationDirPath();
}
QString ToolManager::binariesDirectory() { return QDir(appDirectory()).filePath(QStringLiteral("binaries")); }
QString ToolManager::ffmpegSourceDirectory() { return QDir(binariesDirectory()).filePath(QStringLiteral("ffmpeg")); }
QString ToolManager::ffmpegOutputBinDirectory() { return QDir(binariesDirectory()).filePath(QStringLiteral("ffmpeg-build/bin")); }
QString ToolManager::ytdlpSourceDirectory() { return QDir(binariesDirectory()).filePath(QStringLiteral("yt-dlp")); }

static QString pathTool(const QString &name) {
    return QStandardPaths::findExecutable(name);
}

QString ToolManager::pythonProgram(QStringList *prefixArguments) {
    if (prefixArguments) prefixArguments->clear();
#ifdef Q_OS_WIN
    const QString local = qEnvironmentVariable("LOCALAPPDATA");
    const QStringList candidates = {
        QDir(local).filePath(QStringLiteral("Python/pythoncore-3.14-64/python.exe")),
        QDir(local).filePath(QStringLiteral("Programs/Python/Python314/python.exe")),
        QDir(local).filePath(QStringLiteral("Programs/Python/Python313/python.exe")),
        QDir(local).filePath(QStringLiteral("Programs/Python/Python312/python.exe"))
    };
    for (const QString &candidate : candidates)
        if (!local.isEmpty() && QFileInfo(candidate).isFile()) return candidate;
    const QString py = pathTool(QStringLiteral("py.exe"));
    if (!py.isEmpty()) {
        if (prefixArguments) *prefixArguments << QStringLiteral("-3");
        return py;
    }
    QString python = pathTool(QStringLiteral("python.exe"));
    if (python.isEmpty()) python = pathTool(QStringLiteral("python"));
    return python;
#else
    QString python = pathTool(QStringLiteral("python3"));
    if (python.isEmpty()) python = pathTool(QStringLiteral("python"));
    return python;
#endif
}

ToolPaths ToolManager::locate(QString *diagnostic) {
    ToolPaths paths;
    const QString bin = ffmpegOutputBinDirectory();
    paths.ffmpeg = ProcessRunner::findExecutable(
        {ProcessRunner::platformExecutableName(QStringLiteral("ffmpeg")), QStringLiteral("ffmpeg")}, {bin});
    paths.ffprobe = ProcessRunner::findExecutable(
        {ProcessRunner::platformExecutableName(QStringLiteral("ffprobe")), QStringLiteral("ffprobe")}, {bin});

#ifdef Q_OS_LINUX
    const QString linuxBinary = QDir(binariesDirectory()).filePath(QStringLiteral("yt-dlp_linux"));
    if (QFileInfo(linuxBinary).isFile() && QFileInfo(linuxBinary).isExecutable())
        paths.ytdlpCommand << linuxBinary;
#endif
    const QString sourceMain = QDir(ytdlpSourceDirectory()).filePath(QStringLiteral("yt_dlp/__main__.py"));
    if (paths.ytdlpCommand.isEmpty() && QFileInfo(sourceMain).isFile()) {
        QStringList prefix;
        const QString python = pythonProgram(&prefix);
        if (!python.isEmpty()) {
            paths.ytdlpCommand << python;
            paths.ytdlpCommand += prefix;
            paths.ytdlpCommand << sourceMain;
        }
    }
    if (paths.ytdlpCommand.isEmpty()) {
        const QString executable = ProcessRunner::findExecutable(
            {ProcessRunner::platformExecutableName(QStringLiteral("yt-dlp")), QStringLiteral("yt-dlp"),
             QStringLiteral("yt_dlp.exe"), QStringLiteral("yt_dlp")},
            {ytdlpSourceDirectory(), binariesDirectory()});
        if (!executable.isEmpty()) paths.ytdlpCommand << executable;
    }
    if (paths.ytdlpCommand.isEmpty()) {
        QStringList prefix;
        const QString python = pythonProgram(&prefix);
        if (!python.isEmpty()) {
            paths.ytdlpCommand << python;
            paths.ytdlpCommand += prefix;
            paths.ytdlpCommand << QStringLiteral("-m") << QStringLiteral("yt_dlp");
        }
    }
    paths.ytdlpCommand.removeAll(QString());
    if (!paths.ytdlpCommand.isEmpty()) {
        const ProcessResult version = ProcessRunner::run(paths.ytdlpCommand.first(),
            paths.ytdlpCommand.mid(1) + QStringList{QStringLiteral("--version")},
            {}, QProcessEnvironment::systemEnvironment(), 20000);
        if (version.exitCode == 0)
            paths.ytdlpVersion = version.output.trimmed().section(QRegularExpression(QStringLiteral("[\\r\\n]+")), 0, 0);
    }
    if (diagnostic) {
        QStringList missing;
        if (paths.ffmpeg.isEmpty()) missing << QStringLiteral("FFmpeg");
        if (paths.ffprobe.isEmpty()) missing << QStringLiteral("FFprobe");
        if (paths.ytdlpCommand.isEmpty()) missing << QStringLiteral("yt-dlp");
        *diagnostic = missing.isEmpty() ? QStringLiteral("All required tools were found.")
            : QStringLiteral("Missing tool(s): %1").arg(missing.join(QStringLiteral(", ")));
    }
    return paths;
}

bool ToolManager::isGitCheckout(const QString &directory) {
    const QString git = pathTool(QStringLiteral("git"));
    if (git.isEmpty()) return false;
    const ProcessResult result = ProcessRunner::run(git,
        {QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel")}, directory,
        QProcessEnvironment::systemEnvironment(), 15000);
    if (!result.started || result.exitCode != 0) return false;
    QString top = QDir::cleanPath(QFileInfo(result.output.trimmed()).absoluteFilePath());
    QString wanted = QDir::cleanPath(QFileInfo(directory).absoluteFilePath());
#ifdef Q_OS_WIN
    return top.compare(wanted, Qt::CaseInsensitive) == 0;
#else
    return top == wanted;
#endif
}

QString ToolManager::safePlaylistTempDirectory() {
    QStringList bases;
#ifdef Q_OS_WIN
    bases << qEnvironmentVariable("LOCALAPPDATA");
#endif
    bases << qEnvironmentVariable("XDG_CACHE_HOME") << qEnvironmentVariable("TEMP")
          << qEnvironmentVariable("TMP") << QDir::tempPath();
    for (const QString &base : bases) {
        if (base.trimmed().isEmpty()) continue;
        const QString candidate = QDir(base).filePath(QStringLiteral("yt-downloader-temp"));
        if (QDir().mkpath(candidate) && QFileInfo(candidate).isWritable())
            return QDir::cleanPath(candidate);
    }
    return {};
}

bool ToolManager::runYtdlp(const QStringList &prefix, const QStringList &arguments,
                           QString *output, int timeoutMs) {
    if (prefix.isEmpty()) return false;
    const ProcessResult result = ProcessRunner::run(prefix.first(), prefix.mid(1) + arguments,
        {}, QProcessEnvironment::systemEnvironment(), timeoutMs);
    if (output) *output = result.output;
    return result.started && result.exitCode == 0;
}

bool ToolManager::verifyFFmpeg(const QString &ffmpeg, const QString &ffprobe, const Log &log) {
    for (const QString &tool : {ffmpeg, ffprobe}) {
        const ProcessResult check = ProcessRunner::run(tool, {QStringLiteral("-version")},
            {}, QProcessEnvironment::systemEnvironment(), 20000);
        if (!check.started || check.exitCode != 0) {
            log(QStringLiteral("ERROR: %1 runtime check failed: %2")
                    .arg(QFileInfo(tool).fileName(), check.output.left(1200)));
            return false;
        }
    }
    const ProcessResult encoders = ProcessRunner::run(ffmpeg,
        {QStringLiteral("-hide_banner"), QStringLiteral("-encoders")},
        {}, QProcessEnvironment::systemEnvironment(), 20000);
    if (!encoders.started || encoders.exitCode != 0 ||
        !encoders.output.contains(QRegularExpression(QStringLiteral("\\blibmp3lame\\b"),
                                                      QRegularExpression::CaseInsensitiveOption))) {
        log(QStringLiteral("ERROR: FFmpeg is missing the required libmp3lame encoder."));
        return false;
    }
    return true;
}

bool ToolManager::prepare(const Log &log, const std::atomic_bool *cancel) {
    const bool sourcePresent = QFileInfo(QDir(ffmpegSourceDirectory()).filePath(QStringLiteral("configure"))).isFile();
    if (!sourcePresent) {
        QString diagnostic;
        const ToolPaths bundled = locate(&diagnostic);
        if (!bundled.ffmpeg.isEmpty() && !bundled.ffprobe.isEmpty() && !bundled.ytdlpCommand.isEmpty() &&
            verifyFFmpeg(bundled.ffmpeg, bundled.ffprobe, log)) {
            log(QStringLiteral("Using FFmpeg, FFprobe, and yt-dlp bundled beside the application."));
            if (!bundled.ytdlpVersion.isEmpty())
                log(QStringLiteral("yt-dlp version: %1").arg(bundled.ytdlpVersion));
            return true;
        }
        log(QStringLiteral("Bundled tool check failed. %1").arg(diagnostic));
    }

    log(QStringLiteral("Preparing local FFmpeg and yt-dlp tools..."));
    bool ffmpegChanged = false;
    const bool ffmpegUpdateChecked = updateFFmpegSource(log, &ffmpegChanged, cancel);
    if (!ffmpegUpdateChecked)
        log(QStringLiteral("WARNING: FFmpeg source update check did not complete; validating the existing source/build."));
    if (ffmpegChanged) {
        const QString marker = QDir(binariesDirectory()).filePath(
            QStringLiteral("ffmpeg-build/.downloader-build-profile.json"));
        QFile::remove(marker);
        log(QStringLiteral("FFmpeg source changed; the runtime will be rebuilt."));
    }
    const bool ffmpegReady = ensureFFmpeg(log, cancel);

    bool ytdlpChanged = false;
    const bool ytdlpReady = updateYtdlp(log, &ytdlpChanged, cancel);
    Q_UNUSED(ytdlpChanged);
    QString diagnostic;
    const ToolPaths paths = locate(&diagnostic);
    if (ffmpegReady) log(QStringLiteral("FFmpeg and FFprobe runtime checks passed."));
    if (ytdlpReady && !paths.ytdlpVersion.isEmpty())
        log(QStringLiteral("yt-dlp source/runtime version: %1").arg(paths.ytdlpVersion));
    if (!ytdlpReady) log(QStringLiteral("ERROR: yt-dlp preparation failed. %1").arg(diagnostic));
    return ffmpegReady && ytdlpReady && !paths.ffmpeg.isEmpty() &&
           !paths.ffprobe.isEmpty() && !paths.ytdlpCommand.isEmpty();
}
bool ToolManager::ensureFFmpeg(const Log &log, const std::atomic_bool *cancel) {
    const QString sourceConfigure = QDir(ffmpegSourceDirectory()).filePath(QStringLiteral("configure"));
    const QString builtFFmpeg = QDir(ffmpegOutputBinDirectory()).filePath(ProcessRunner::platformExecutableName(QStringLiteral("ffmpeg")));
    const QString builtProbe = QDir(ffmpegOutputBinDirectory()).filePath(ProcessRunner::platformExecutableName(QStringLiteral("ffprobe")));
    const QString markerPath = QDir(binariesDirectory()).filePath(QStringLiteral("ffmpeg-build/.downloader-build-profile.json"));
    if (QFileInfo(sourceConfigure).isFile()) {
        bool markerReady = false;
        QFile marker(markerPath);
        if (marker.open(QIODevice::ReadOnly)) {
            const QJsonDocument doc = QJsonDocument::fromJson(marker.readAll());
            markerReady = doc.isObject() && doc.object().value(QStringLiteral("profile")).toString() == kProfile &&
                doc.object().value(QStringLiteral("platform")).toString() ==
#ifdef Q_OS_WIN
                QStringLiteral("win32");
#else
                QStringLiteral("linux");
#endif
        }
        if (QFileInfo(builtFFmpeg).isFile() && QFileInfo(builtProbe).isFile() &&
            markerReady && verifyFFmpeg(builtFFmpeg, builtProbe, log)) {
            log(QStringLiteral("Separate FFmpeg multi-vendor build already exists and is complete; skipping compilation."));
            return true;
        }
        log(QStringLiteral("FFmpeg build output is missing, incomplete, or has an old profile. Starting compilation..."));
        if (!buildFFmpeg(log, cancel)) return false;
        QDir().mkpath(QFileInfo(markerPath).absolutePath());
        QSaveFile markerFile(markerPath);
        if (markerFile.open(QIODevice::WriteOnly)) {
            const QJsonObject profile{{QStringLiteral("profile"), kProfile},
                {QStringLiteral("platform"),
#ifdef Q_OS_WIN
                    QStringLiteral("win32")
#else
                    QStringLiteral("linux")
#endif
                }};
            markerFile.write(QJsonDocument(profile).toJson(QJsonDocument::Indented));
            markerFile.commit();
        }
        return verifyFFmpeg(builtFFmpeg, builtProbe, log);
    }
    const ToolPaths paths = locate();
    if (!paths.ffmpeg.isEmpty() && !paths.ffprobe.isEmpty()) {
        log(QStringLiteral("FFmpeg source checkout was not found; checking the existing FFmpeg installation."));
        return verifyFFmpeg(paths.ffmpeg, paths.ffprobe, log);
    }
    log(QStringLiteral("ERROR: No FFmpeg source tree or usable FFmpeg/FFprobe installation was found."));
    return false;
}

bool ToolManager::buildFFmpeg(const Log &log, const std::atomic_bool *cancel) {
#ifdef Q_OS_WIN
    return buildFFmpegWindows(log, cancel);
#else
    return buildFFmpegPosix(log, cancel);
#endif
}

static bool packageExists(const QStringList &packages, const QProcessEnvironment &env) {
    QString pkg = QStandardPaths::findExecutable(QStringLiteral("pkg-config"));
    if (pkg.isEmpty()) pkg = QStandardPaths::findExecutable(QStringLiteral("pkgconf"));
    if (pkg.isEmpty()) return false;
    const ProcessResult result = ProcessRunner::run(pkg, QStringList{QStringLiteral("--exists")} + packages,
        {}, env, 20000);
    return result.started && result.exitCode == 0;
}

bool ToolManager::buildFFmpegPosix(const Log &log, const std::atomic_bool *cancel) {
    const QString source = ffmpegSourceDirectory();
    if (!QFileInfo(QDir(source).filePath(QStringLiteral("configure"))).isFile()) return false;
    QString shell = QStandardPaths::findExecutable(QStringLiteral("bash"));
    if (shell.isEmpty()) shell = QStandardPaths::findExecutable(QStringLiteral("sh"));
    const QString make = QStandardPaths::findExecutable(QStringLiteral("make"));
    if (shell.isEmpty() || make.isEmpty()) {
        log(QStringLiteral("ERROR: FFmpeg compilation requires bash/sh and GNU make."));
        return false;
    }
    const QString outputRoot = QDir(binariesDirectory()).filePath(QStringLiteral("ffmpeg-build"));
    const QString objectDir = QDir(outputRoot).filePath(QStringLiteral("obj"));
    const QString installDir = QDir(outputRoot).filePath(QStringLiteral("install"));
    QDir(objectDir).removeRecursively();
    QDir(installDir).removeRecursively();
    if (!QDir().mkpath(objectDir) || !QDir().mkpath(installDir)) return false;

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QStringList configureArgs{QStringLiteral("--prefix=") + installDir,
        QStringLiteral("--disable-doc"), QStringLiteral("--disable-ffplay"),
        QStringLiteral("--enable-gpl"), QStringLiteral("--enable-version3"),
        QStringLiteral("--enable-shared"), QStringLiteral("--disable-static")};
    if (packageExists({QStringLiteral("ffnvcodec")}, env)) {
        configureArgs << QStringLiteral("--enable-ffnvcodec") << QStringLiteral("--enable-nvenc") << QStringLiteral("--enable-nvdec");
        log(QStringLiteral("Linux NVIDIA NVENC/NVDEC build support enabled."));
    }
    if (packageExists({QStringLiteral("vpl")}, env))
        configureArgs << QStringLiteral("--enable-libvpl");
    else if (packageExists({QStringLiteral("libmfx")}, env))
        configureArgs << QStringLiteral("--enable-libmfx");
    if (packageExists({QStringLiteral("libva"), QStringLiteral("libva-drm")}, env))
        configureArgs << QStringLiteral("--enable-vaapi");
    if (packageExists({QStringLiteral("vulkan")}, env))
        configureArgs << QStringLiteral("--enable-vulkan");

    // AMD AMF is header-only for FFmpeg. Stage its public SDK headers when available.
    const QString amfSource = QDir(binariesDirectory()).filePath(QStringLiteral("AMF"));
    const QString amfInclude = QDir(amfSource).filePath(QStringLiteral("amf/public/include"));
    const QString amfVersion = QDir(amfInclude).filePath(QStringLiteral("core/Version.h"));
    if (!QFileInfo(amfVersion).isFile()) {
        const QString git = QStandardPaths::findExecutable(QStringLiteral("git"));
        if (!git.isEmpty()) {
            const ProcessResult clone = ProcessRunner::run(git,
                {QStringLiteral("clone"), QStringLiteral("--depth"), QStringLiteral("1"),
                 QStringLiteral("https://github.com/GPUOpen-LibrariesAndSDKs/AMF.git"), amfSource},
                binariesDirectory(), QProcessEnvironment::systemEnvironment(), 600000, cancel,
                [&](const QString &line) { log(QStringLiteral("[AMF-git] ") + line); });
            if (clone.exitCode != 0)
                log(QStringLiteral("WARNING: AMD AMF headers could not be fetched; AMF encoding will be omitted."));
        }
    }
    if (QFileInfo(amfVersion).isFile()) {
        const QString stagedHeaders = QDir(objectDir).filePath(QStringLiteral("_downloader_deps/include"));
        QDir(stagedHeaders).removeRecursively();
        if (copyDirectory(amfInclude, QDir(stagedHeaders).filePath(QStringLiteral("AMF")))) {
            configureArgs << QStringLiteral("--enable-amf")
                          << QStringLiteral("--extra-cflags=-I") + stagedHeaders;
            log(QStringLiteral("Linux AMD AMF header support enabled."));
        } else {
            log(QStringLiteral("WARNING: Could not stage AMD AMF headers."));
        }
    }

    const QList<QPair<QString, QString>> libraries{
        {QStringLiteral("x264"), QStringLiteral("--enable-libx264")},
        {QStringLiteral("x265"), QStringLiteral("--enable-libx265")},
        {QStringLiteral("vpx"), QStringLiteral("--enable-libvpx")},
        {QStringLiteral("aom"), QStringLiteral("--enable-libaom")},
        {QStringLiteral("opus"), QStringLiteral("--enable-libopus")},
        {QStringLiteral("lame"), QStringLiteral("--enable-libmp3lame")}
    };
    for (const auto &entry : libraries)
        if (packageExists({entry.first}, env)) configureArgs << entry.second;
    if (!packageExists({QStringLiteral("lame")}, env)) {
        log(QStringLiteral("ERROR: libmp3lame development files are required; install your distribution's LAME development package."));
        return false;
    }

    log(QStringLiteral("FFmpeg source: %1").arg(source));
    log(QStringLiteral("FFmpeg object directory: %1").arg(objectDir));
    auto emitLine = [&](const QString &line) { log(QStringLiteral("[ffmpeg-build] ") + line); };
    ProcessResult result = ProcessRunner::run(shell,
        QStringList{QDir(source).filePath(QStringLiteral("configure"))} + configureArgs,
        objectDir, env, 1800000, cancel, emitLine);
    if (!result.started || result.exitCode != 0) {
        log(QStringLiteral("ERROR: FFmpeg configure failed: %1").arg(result.output.right(2000)));
        return false;
    }
    const int jobs = qMax(2, QThread::idealThreadCount());
    result = ProcessRunner::run(make, {QStringLiteral("-j%1").arg(jobs)},
        objectDir, env, 7200000, cancel, emitLine);
    if (!result.started || result.exitCode != 0) {
        log(QStringLiteral("ERROR: FFmpeg compilation failed."));
        return false;
    }
    result = ProcessRunner::run(make, {QStringLiteral("install")},
        objectDir, env, 600000, cancel, emitLine);
    if (!result.started || result.exitCode != 0) {
        log(QStringLiteral("ERROR: FFmpeg install step failed."));
        return false;
    }
    const QString installedBin = QDir(installDir).filePath(QStringLiteral("bin"));
    const QString outputBin = ffmpegOutputBinDirectory();
    QDir(outputBin).removeRecursively();
    if (!copyDirectory(installedBin, outputBin)) {
        log(QStringLiteral("ERROR: Could not stage compiled FFmpeg binaries."));
        return false;
    }
    return true;
}
