#include "toolmanager.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QDirIterator>
#include <QRegularExpression>
#include <QSet>


static bool stageMsysRuntimeDependencies(const QString &stageDirectory,
                                         const QString &compilerBin,
                                         const QString &usrBin,
                                         const QProcessEnvironment &environment,
                                         const ToolManager::Log &log) {
    QString objdump = QDir(compilerBin).filePath(QStringLiteral("objdump.exe"));
    if (!QFileInfo(objdump).isFile())
        objdump = QDir(usrBin).filePath(QStringLiteral("objdump.exe"));
    if (!QFileInfo(objdump).isFile()) {
        log(QStringLiteral("ERROR: MSYS2 objdump.exe was not found; runtime DLL dependencies cannot be staged."));
        return false;
    }
    QStringList pending;
    QDirIterator iterator(stageDirectory,
        QStringList{QStringLiteral("*.exe"), QStringLiteral("*.dll")},
        QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext()) pending << iterator.next();
    QSet<QString> inspected;
    const QString systemRoot = qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows"));
    const QDir system32(QDir(systemRoot).filePath(QStringLiteral("System32")));
    const QRegularExpression dllPattern(QStringLiteral("DLL Name:\\s*([^\\s]+)"),
                                         QRegularExpression::CaseInsensitiveOption);
    for (int index = 0; index < pending.size(); ++index) {
        const QString current = pending.at(index);
        const QString currentName = QFileInfo(current).fileName().toCaseFolded();
        if (inspected.contains(currentName)) continue;
        inspected.insert(currentName);
        const ProcessResult imports = ProcessRunner::run(objdump,
            {QStringLiteral("-p"), current}, {}, environment, 20000);
        if (!imports.started || imports.exitCode != 0) {
            log(QStringLiteral("WARNING: Could not inspect imports for %1.").arg(QFileInfo(current).fileName()));
            continue;
        }
        const QStringList lines = imports.output.split(
            QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            const auto match = dllPattern.match(line);
            if (!match.hasMatch()) continue;
            const QString dll = match.captured(1);
            const QString key = dll.toCaseFolded();
            if (inspected.contains(key) ||
                QFileInfo(QDir(stageDirectory).filePath(dll)).isFile())
                continue;
            if (key.startsWith(QStringLiteral("api-ms-win-")) ||
                key.startsWith(QStringLiteral("ext-ms-")) ||
                QFileInfo(system32.filePath(dll)).isFile())
                continue;
            QString sourcePath;
            for (const QString &directory : {compilerBin, usrBin}) {
                const QString candidate = QDir(directory).filePath(dll);
                if (QFileInfo(candidate).isFile()) {
                    sourcePath = candidate;
                    break;
                }
            }
            if (sourcePath.isEmpty()) {
                log(QStringLiteral("WARNING: Dependency %1 was not found in the selected MSYS2 toolchain.").arg(dll));
                continue;
            }
            const QString destination = QDir(stageDirectory).filePath(dll);
            if (!QFile::copy(sourcePath, destination)) {
                log(QStringLiteral("ERROR: Could not stage MSYS2 runtime dependency %1.").arg(dll));
                return false;
            }
            pending << destination;
        }
    }
    const QStringList requiredFamilies{
        QStringLiteral("avcodec-"), QStringLiteral("avdevice-"), QStringLiteral("avfilter-"),
        QStringLiteral("avformat-"), QStringLiteral("avutil-"), QStringLiteral("swresample-"),
        QStringLiteral("swscale-")
    };
    const QStringList dllNames = QDir(stageDirectory).entryList(
        {QStringLiteral("*.dll")}, QDir::Files);
    for (const QString &family : requiredFamilies) {
        bool found = false;
        for (const QString &name : dllNames) {
            if (name.startsWith(family, Qt::CaseInsensitive)) { found = true; break; }
        }
        if (!found) {
            log(QStringLiteral("ERROR: FFmpeg runtime is missing required DLL family %1*.dll.").arg(family));
            return false;
        }
    }
    return true;
}

bool ToolManager::buildFFmpegWindows(const Log &log, const std::atomic_bool *cancel) {
    const QString source = ffmpegSourceDirectory();
    if (!QFileInfo(QDir(source).filePath(QStringLiteral("configure"))).isFile()) {
        log(QStringLiteral("ERROR: FFmpeg source checkout is missing configure."));
        return false;
    }
    const QString preference = qEnvironmentVariable("DOWNLOADER_FFMPEG_TOOLCHAIN", QStringLiteral("auto")).toLower();
    if (preference != QStringLiteral("auto") && preference != QStringLiteral("msvc") &&
        preference != QStringLiteral("msys2")) {
        log(QStringLiteral("ERROR: DOWNLOADER_FFMPEG_TOOLCHAIN must be auto, msvc, or msys2."));
        return false;
    }
    if (preference == QStringLiteral("msvc"))
        return buildFFmpegWindowsMsvc(log, cancel);
    if (preference == QStringLiteral("auto"))
        log(QStringLiteral("Auto-selecting Windows FFmpeg build environment (MSYS2 multi-vendor build first, then native MSVC)..."));
    QStringList roots;
    const QString configured = qEnvironmentVariable("MSYS2_ROOT");
    if (!configured.isEmpty()) roots << configured;
    roots << QStringLiteral("C:/msys64") << QStringLiteral("C:/tools/msys64") << QStringLiteral("C:/msys32");
    QString bash, compilerBin, usrBin, system;
    const QList<QPair<QString, QString>> toolchains{
        {QStringLiteral("ucrt64"), QStringLiteral("UCRT64")},
        {QStringLiteral("mingw64"), QStringLiteral("MINGW64")},
        {QStringLiteral("clang64"), QStringLiteral("CLANG64")}
    };
    for (const QString &root : roots) {
        const QString candidateBash = QDir(root).filePath(QStringLiteral("usr/bin/bash.exe"));
        if (!QFileInfo(candidateBash).isFile()) continue;
        for (const auto &item : toolchains) {
            const QString candidateBin = QDir(root).filePath(item.first + QStringLiteral("/bin"));
            if (QFileInfo(QDir(candidateBin).filePath(QStringLiteral("gcc.exe"))).isFile() ||
                QFileInfo(QDir(candidateBin).filePath(QStringLiteral("clang.exe"))).isFile()) {
                bash = candidateBash;
                compilerBin = candidateBin;
                usrBin = QDir(root).filePath(QStringLiteral("usr/bin"));
                system = item.second;
                break;
            }
        }
        if (!bash.isEmpty()) break;
    }
    if (bash.isEmpty()) {
        log(QStringLiteral("ERROR: No MSYS2 UCRT64/MINGW64/CLANG64 compiler was found. Install MSYS2 with GCC/Clang, GNU make and FFmpeg dependencies."));
        return false;
    }
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("MSYSTEM"), system);
    env.insert(QStringLiteral("MSYS2_PATH_TYPE"), QStringLiteral("inherit"));
    env.insert(QStringLiteral("CHERE_INVOKING"), QStringLiteral("1"));
    env.insert(QStringLiteral("PATH"), compilerBin + QDir::listSeparator() + usrBin +
                QDir::listSeparator() + env.value(QStringLiteral("PATH")));
    const QString prefix = system == QStringLiteral("UCRT64") ? QStringLiteral("mingw-w64-ucrt-x86_64")
        : system == QStringLiteral("MINGW64") ? QStringLiteral("mingw-w64-x86_64")
        : QStringLiteral("mingw-w64-clang-x86_64");
    const QString pacman = QDir(usrBin).filePath(QStringLiteral("pacman.exe"));
    if (QFileInfo(pacman).isFile()) {
        const QStringList packages{
            prefix + QStringLiteral("-pkgconf"), prefix + QStringLiteral("-ffnvcodec-headers"),
            prefix + QStringLiteral("-libvpl"), prefix + QStringLiteral("-lame"),
            prefix + QStringLiteral("-opus"), prefix + QStringLiteral("-libx264"),
            prefix + QStringLiteral("-x265"), prefix + QStringLiteral("-libvpx"),
            prefix + QStringLiteral("-aom"), QStringLiteral("make"), QStringLiteral("nasm")
        };
        log(QStringLiteral("Ensuring MSYS2 FFmpeg dependencies are installed..."));
        const ProcessResult install = ProcessRunner::run(bash,
            {QStringLiteral("-lc"), QStringLiteral("pacman -S --needed --noconfirm ") + packages.join(QLatin1Char(' '))},
            {}, env, 900000, cancel,
            [&](const QString &line) { log(QStringLiteral("[pacman] ") + line); });
        if (install.exitCode != 0)
            log(QStringLiteral("WARNING: Some optional MSYS2 packages failed; configure will use installed libraries."));
    }
    QString base = qEnvironmentVariable("LOCALAPPDATA");
    if (base.isEmpty()) base = QDir::tempPath();
    QString workspace = QDir(base).filePath(QStringLiteral("DownloaderFFmpegBuild"));
    if (workspace.contains(QLatin1Char(' '))) workspace = QStringLiteral("C:/DownloaderFFmpegBuild");
    QDir(workspace).removeRecursively();
    if (!copyDirectory(source, workspace, {QStringLiteral(".git")})) {
        log(QStringLiteral("ERROR: Could not stage FFmpeg in a no-space build directory."));
        return false;
    }
    env.insert(QStringLiteral("DOWNLOADER_BUILD_ROOT"), QDir::toNativeSeparators(workspace));
    const QString amfRoot = QDir(binariesDirectory()).filePath(QStringLiteral("AMF"));
    const QString amfInclude = QDir(amfRoot).filePath(QStringLiteral("amf/public/include"));
    if (!QFileInfo(amfInclude).isDir()) {
        const QString git = QStandardPaths::findExecutable(QStringLiteral("git"));
        if (!git.isEmpty()) {
            const ProcessResult clone = ProcessRunner::run(git,
                {QStringLiteral("clone"), QStringLiteral("--depth"), QStringLiteral("1"),
                 QStringLiteral("https://github.com/GPUOpen-LibrariesAndSDKs/AMF.git"), amfRoot},
                binariesDirectory(), QProcessEnvironment::systemEnvironment(), 600000, cancel,
                [&](const QString &line) { log(QStringLiteral("[AMF-git] ") + line); });
            if (clone.exitCode != 0)
                log(QStringLiteral("WARNING: AMD AMF headers could not be cloned; FFmpeg may omit AMF."));
        }
    }
    if (QFileInfo(amfInclude).isDir()) {
        const QString stagedAmf = QDir(workspace).filePath(QStringLiteral("_downloader_deps/amf/AMF"));
        QDir(stagedAmf).removeRecursively();
        if (copyDirectory(amfInclude, stagedAmf))
            log(QStringLiteral("Staged AMD AMF public headers for FFmpeg."));
        else
            log(QStringLiteral("WARNING: Could not stage AMD AMF headers."));
    }
    const QString shellDollar = QString(QChar(36));
    const QString script =
        "set -euo pipefail\n"
        "ROOT=\"$(cygpath -u \"$DOWNLOADER_BUILD_ROOT\")\"\n"
        "cd \"$ROOT\"\n"
        "PREFIX=\"$ROOT/_shared_install\"\n"
        "mkdir -p \"$PREFIX\"\n"
        "FLAGS=(--enable-d3d11va --enable-dxva2)\n"
        "EXTRA_CFLAGS=()\n"
        "if [ -d \"$ROOT/_downloader_deps/amf/AMF\" ]; then FLAGS+=(--enable-amf); EXTRA_CFLAGS+=(\"-I$ROOT/_downloader_deps/amf\"); fi\n"
        "if pkg-config --exists ffnvcodec; then FLAGS+=(--enable-ffnvcodec --enable-nvenc --enable-nvdec); fi\n"
        "if pkg-config --exists vpl; then FLAGS+=(--enable-libvpl); fi\n"
        "if pkg-config --exists lame; then FLAGS+=(--enable-libmp3lame); else echo 'ERROR: LAME is required'; exit 42; fi\n"
        "if pkg-config --exists opus; then FLAGS+=(--enable-libopus); fi\n"
        "if pkg-config --exists x264; then FLAGS+=(--enable-libx264); fi\n"
        "if pkg-config --exists x265; then FLAGS+=(--enable-libx265); fi\n"
        "if pkg-config --exists vpx; then FLAGS+=(--enable-libvpx); fi\n"
        "if pkg-config --exists aom; then FLAGS+=(--enable-libaom); fi\n"
        "if ! command -v nasm >/dev/null 2>&1; then FLAGS+=(--disable-x86asm); fi\n"
        "./configure --prefix=\"$PREFIX\" --disable-doc --disable-ffplay --enable-gpl --enable-version3 --enable-shared --disable-static --extra-cflags=\"" + shellDollar + "{EXTRA_CFLAGS[*]}\" \"" + shellDollar + "{FLAGS[@]}\"\n"
        "JOBS=\"" + shellDollar + "{NUMBER_OF_PROCESSORS:-4}\"\n"
        "make -j\"$JOBS\"\n"
        "make install\n";
    const QString scriptPath = QDir(workspace).filePath(QStringLiteral(".downloader-build.sh"));
    QFile file(scriptPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    file.write(script.toUtf8());
    file.close();
    log(QStringLiteral("Building FFmpeg with MSYS2 %1 in %2").arg(system, workspace));
    const ProcessResult build = ProcessRunner::run(bash,
        {QStringLiteral("-lc"), QStringLiteral("bash .downloader-build.sh")},
        workspace, env, 10800000, cancel,
        [&](const QString &line) { log(QStringLiteral("[ffmpeg-build] ") + line); });
    if (!build.started || build.exitCode != 0) {
        log(QStringLiteral("ERROR: FFmpeg build failed: %1").arg(build.output.right(3000)));
        return false;
    }
    const QString stagedBin = QDir(workspace).filePath(QStringLiteral("_shared_install/bin"));
    const QString outputBin = ffmpegOutputBinDirectory();
    const QString replacement = outputBin + QStringLiteral(".new");
    const QString backup = outputBin + QStringLiteral(".old");
    QDir(replacement).removeRecursively();
    QDir(backup).removeRecursively();
    if (!copyDirectory(stagedBin, replacement)) {
        log(QStringLiteral("ERROR: Could not stage FFmpeg runtime files."));
        return false;
    }
    const QString newFFmpeg = QDir(replacement).filePath(QStringLiteral("ffmpeg.exe"));
    const QString newProbe = QDir(replacement).filePath(QStringLiteral("ffprobe.exe"));
    if (!stageMsysRuntimeDependencies(replacement, compilerBin, usrBin, env, log)) {
        QDir(replacement).removeRecursively();
        return false;
    }
    if (!QFileInfo(newFFmpeg).isFile() || !QFileInfo(newProbe).isFile() ||
        !verifyFFmpeg(newFFmpeg, newProbe, log)) {
        QDir(replacement).removeRecursively();
        return false;
    }
    if (QFileInfo::exists(outputBin) && !QDir().rename(outputBin, backup)) return false;
    if (!QDir().rename(replacement, outputBin)) {
        if (QFileInfo::exists(backup)) QDir().rename(backup, outputBin);
        return false;
    }
    QDir(backup).removeRecursively();
    log(QStringLiteral("Windows FFmpeg build installed in %1").arg(outputBin));
    return true;
}
