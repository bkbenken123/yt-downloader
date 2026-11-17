#include "toolmanager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QStandardPaths>

static QString findEnvExe(const QString &name, const QProcessEnvironment &env) {
    for (const QString &part : env.value(QStringLiteral("PATH")).split(QDir::listSeparator(), Qt::SkipEmptyParts)) {
        QString path = QDir(part).filePath(name);
        if (QFileInfo(path).isFile()) return path;
#ifdef Q_OS_WIN
        if (!name.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive) &&
            QFileInfo(path + QStringLiteral(".exe")).isFile())
            return path + QStringLiteral(".exe");
#endif
    }
    return {};
}

static bool initializeVsEnvironment(QProcessEnvironment &env, const ToolManager::Log &log) {
    env = QProcessEnvironment::systemEnvironment();
    auto ready = [&] {
        return !findEnvExe(QStringLiteral("cl.exe"), env).isEmpty() &&
               !findEnvExe(QStringLiteral("link.exe"), env).isEmpty() &&
               !findEnvExe(QStringLiteral("nmake.exe"), env).isEmpty();
    };
    if (ready() && (env.value(QStringLiteral("VSCMD_ARG_TGT_ARCH")).compare(QStringLiteral("x64"), Qt::CaseInsensitive) == 0 ||
        findEnvExe(QStringLiteral("cl.exe"), env).contains(QStringLiteral("Hostx64/x64"), Qt::CaseInsensitive))) {
        log(QStringLiteral("Using the active Visual Studio x64 developer environment."));
        return true;
    }

    const QString pf86 = qEnvironmentVariable("ProgramFiles(x86)", QStringLiteral("C:/Program Files (x86)"));
    const QString vswhere = QDir(pf86).filePath(QStringLiteral("Microsoft Visual Studio/Installer/vswhere.exe"));
    QStringList installs;
    if (QFileInfo(vswhere).isFile()) {
        const ProcessResult found = ProcessRunner::run(vswhere,
            {QStringLiteral("-all"), QStringLiteral("-products"), QStringLiteral("*"),
             QStringLiteral("-requires"), QStringLiteral("Microsoft.VisualStudio.Component.VC.Tools.x86.x64"),
             QStringLiteral("-property"), QStringLiteral("installationPath")},
            {}, env, 20000);
        if (found.exitCode == 0)
            installs = found.output.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    }
    if (!env.value(QStringLiteral("VSINSTALLDIR")).isEmpty())
        installs.prepend(env.value(QStringLiteral("VSINSTALLDIR")));

    const QString cmd = QDir(qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows")))
        .filePath(QStringLiteral("System32/cmd.exe"));
    const QString marker = QStringLiteral("__DOWNLOADER_VS_ENV__");
    for (const QString &install : installs) {
        const QString devcmd = QDir(install).filePath(QStringLiteral("Common7/Tools/VsDevCmd.bat"));
        if (!QFileInfo(devcmd).isFile()) continue;
        QTemporaryFile temp(QDir(QDir::tempPath()).filePath(QStringLiteral("downloader-vs-XXXXXX.bat")));
        temp.setAutoRemove(false);
        if (!temp.open()) continue;
        const QString batPath = temp.fileName();
        const QByteArray bat = QStringLiteral(
            "@echo off\r\ncall \"%1\" -no_logo -arch=amd64 -host_arch=amd64 >nul\r\necho %2\r\nset\r\n")
            .arg(devcmd, marker).toUtf8();
        temp.write(bat);
        temp.close();
        const ProcessResult result = ProcessRunner::run(cmd,
            {QStringLiteral("/d"), QStringLiteral("/c"), batPath}, {}, env, 90000);
        QFile::remove(batPath);
        if (result.exitCode != 0) continue;
        bool parse = false;
        QProcessEnvironment updated = QProcessEnvironment::systemEnvironment();
        for (const QString &line : result.output.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts)) {
            if (!parse) { if (line.trimmed() == marker) parse = true; continue; }
            const qsizetype pos = line.indexOf(QLatin1Char('='));
            if (pos > 0 && !line.startsWith(QLatin1Char('=')))
                updated.insert(line.left(pos), line.mid(pos + 1));
        }
        if (parse) {
            env = updated;
            if (ready()) {
                log(QStringLiteral("Initialized Visual Studio x64 compiler tools via %1").arg(devcmd));
                return true;
            }
        }
    }
    log(QStringLiteral("ERROR: Visual Studio x64 C++ tools could not be initialized."));
    return false;
}

static bool findMsvcShell(QProcessEnvironment &env, QString &bash, QString &make, QString &shellBin,
                          const ToolManager::Log &log, const std::atomic_bool *cancel) {
    QStringList roots;
    if (!env.value(QStringLiteral("MSYS2_ROOT")).isEmpty()) roots << env.value(QStringLiteral("MSYS2_ROOT"));
    roots << QStringLiteral("C:/msys64") << QStringLiteral("C:/tools/msys64");
    for (const QString &root : roots) {
        const QString usr = QDir(root).filePath(QStringLiteral("usr/bin"));
        const QString b = QDir(usr).filePath(QStringLiteral("bash.exe"));
        const QString m = QDir(usr).filePath(QStringLiteral("make.exe"));
        if (QFileInfo(b).isFile()) {
            bash = b; shellBin = usr;
            if (QFileInfo(m).isFile()) make = m;
            break;
        }
    }
    if (bash.isEmpty()) {
        const QStringList candidates{
            QStringLiteral("C:/Program Files/Git/bin/bash.exe"),
            QStringLiteral("C:/Program Files (x86)/Git/bin/bash.exe")
        };
        for (const QString &candidate : candidates)
            if (QFileInfo(candidate).isFile()) { bash = candidate; shellBin = QFileInfo(candidate).absolutePath(); break; }
    }
    if (bash.isEmpty()) bash = findEnvExe(QStringLiteral("bash.exe"), env);
    if (make.isEmpty()) make = findEnvExe(QStringLiteral("make.exe"), env);
    if (bash.isEmpty()) {
        log(QStringLiteral("ERROR: FFmpeg MSVC configuration requires Git Bash or MSYS2 Bash."));
        return false;
    }

    if (make.isEmpty() && !shellBin.isEmpty()) {
        const QString pacman = QDir(shellBin).filePath(QStringLiteral("pacman.exe"));
        if (QFileInfo(pacman).isFile()) {
            QProcessEnvironment msys = env;
            msys.insert(QStringLiteral("MSYSTEM"), QStringLiteral("MSYS"));
            msys.insert(QStringLiteral("MSYS2_PATH_TYPE"), QStringLiteral("inherit"));
            msys.insert(QStringLiteral("CHERE_INVOKING"), QStringLiteral("1"));
            msys.insert(QStringLiteral("PATH"), shellBin + QDir::listSeparator() + msys.value(QStringLiteral("PATH")));
            ProcessRunner::run(bash, {QStringLiteral("-lc"), QStringLiteral("pacman -S --needed --noconfirm make")},
                {}, msys, 900000, cancel, [&](const QString &line) { log(QStringLiteral("[pacman] ") + line); });
            const QString candidate = QDir(shellBin).filePath(QStringLiteral("make.exe"));
            if (QFileInfo(candidate).isFile()) make = candidate;
        }
    }
    if (make.isEmpty()) make = findEnvExe(QStringLiteral("make"), env);
    if (make.isEmpty()) {
        log(QStringLiteral("ERROR: GNU Make is required. NMAKE alone cannot build FFmpeg."));
        return false;
    }
    const ProcessResult version = ProcessRunner::run(make, {QStringLiteral("--version")}, {}, env, 15000);
    if (version.exitCode != 0 || !version.output.contains(QStringLiteral("GNU Make"))) {
        log(QStringLiteral("ERROR: The selected make program is not GNU Make."));
        return false;
    }

    QStringList pathPriority;
    for (const QString &tool : {QStringLiteral("cl.exe"), QStringLiteral("link.exe"), QStringLiteral("nmake.exe")}) {
        const QString path = findEnvExe(tool, env);
        if (!path.isEmpty() && !pathPriority.contains(QFileInfo(path).absolutePath()))
            pathPriority << QFileInfo(path).absolutePath();
    }
    pathPriority << QFileInfo(make).absolutePath() << shellBin;
    QStringList newPath;
    for (const QString &part : pathPriority)
        if (!part.isEmpty() && !newPath.contains(part, Qt::CaseInsensitive)) newPath << part;
    const QString oldPath = env.value(QStringLiteral("PATH"));
    env.insert(QStringLiteral("PATH"), newPath.join(QDir::listSeparator()) +
               (oldPath.isEmpty() ? QString() : QDir::listSeparator() + oldPath));
    if (!shellBin.isEmpty() && QFileInfo(QDir(shellBin).filePath(QStringLiteral("pacman.exe"))).isFile()) {
        env.insert(QStringLiteral("MSYSTEM"), QStringLiteral("MSYS"));
        env.insert(QStringLiteral("MSYS2_PATH_TYPE"), QStringLiteral("inherit"));
        env.insert(QStringLiteral("CHERE_INVOKING"), QStringLiteral("1"));
    }
    return !findEnvExe(QStringLiteral("cl.exe"), env).isEmpty() &&
           !findEnvExe(QStringLiteral("link.exe"), env).isEmpty() &&
           !findEnvExe(QStringLiteral("nmake.exe"), env).isEmpty();
}

bool ToolManager::buildFFmpegWindowsMsvc(const Log &log, const std::atomic_bool *cancel) {
    QProcessEnvironment env;
    if (!initializeVsEnvironment(env, log)) return false;
    QString bash, make, shellBin;
    if (!findMsvcShell(env, bash, make, shellBin, log, cancel)) return false;

    QString base = qEnvironmentVariable("LOCALAPPDATA");
    if (base.isEmpty()) base = QDir::tempPath();
    QString workspace = QDir(base).filePath(QStringLiteral("DownloaderFFmpegBuild"));
    if (workspace.contains(QLatin1Char(' '))) workspace = QStringLiteral("C:/DownloaderFFmpegBuild");
    QDir(workspace).removeRecursively();
    if (!copyDirectory(ffmpegSourceDirectory(), workspace, {QStringLiteral(".git")})) return false;

    const QString lameSource = QDir(binariesDirectory()).filePath(QStringLiteral("lame"));
    const QString makefile = QDir(lameSource).filePath(QStringLiteral("Makefile.MSVC"));
    if (!QFileInfo(makefile).isFile()) {
        const QString git = QStandardPaths::findExecutable(QStringLiteral("git"));
        if (git.isEmpty()) { log(QStringLiteral("ERROR: Git is required to fetch LAME source.")); return false; }
        const ProcessResult clone = ProcessRunner::run(git,
            {QStringLiteral("clone"), QStringLiteral("--depth"), QStringLiteral("1"),
             QStringLiteral("https://github.com/lameproject/lame.git"), lameSource},
            binariesDirectory(), QProcessEnvironment::systemEnvironment(), 600000, cancel,
            [&](const QString &line) { log(QStringLiteral("[LAME-git] ") + line); });
        if (clone.exitCode != 0 || !QFileInfo(makefile).isFile()) return false;
    }
    const QString nmake = findEnvExe(QStringLiteral("nmake.exe"), env);
    const ProcessResult lame = ProcessRunner::run(nmake,
        {QStringLiteral("/nologo"), QStringLiteral("/f"), QStringLiteral("Makefile.MSVC"),
         QStringLiteral("libmp3lame.dll"), QStringLiteral("MSVCVER=Win64"),
         QStringLiteral("MACHINE=/machine:X64"), QStringLiteral("ASM=NO")},
        lameSource, env, 1800000, cancel,
        [&](const QString &line) { log(QStringLiteral("[LAME] ") + line); });
    if (lame.exitCode != 0) { log(QStringLiteral("ERROR: MSVC LAME build failed.")); return false; }

    const QString lameOutput = QDir(lameSource).filePath(QStringLiteral("output"));
    const QString lameDll = QDir(lameOutput).filePath(QStringLiteral("libmp3lame.dll"));
    const QString lameLib = QDir(lameOutput).filePath(QStringLiteral("libmp3lame.lib"));
    QString lameHeader = QDir(lameSource).filePath(QStringLiteral("include/lame/lame.h"));
    if (!QFileInfo(lameHeader).isFile()) lameHeader = QDir(lameSource).filePath(QStringLiteral("include/lame.h"));
    if (!QFileInfo(lameDll).isFile() || !QFileInfo(lameLib).isFile() || !QFileInfo(lameHeader).isFile()) {
        log(QStringLiteral("ERROR: LAME failed to produce its header, import library and DLL."));
        return false;
    }
    const QString deps = QDir(workspace).filePath(QStringLiteral("_downloader_deps/lame"));
    const QString inc = QDir(deps).filePath(QStringLiteral("include/lame"));
    const QString lib = QDir(deps).filePath(QStringLiteral("lib"));
    if (!QDir().mkpath(inc) || !QDir().mkpath(lib) ||
        !QFile::copy(lameHeader, QDir(inc).filePath(QStringLiteral("lame.h"))) ||
        !QFile::copy(lameLib, QDir(lib).filePath(QStringLiteral("libmp3lame.lib"))) ||
        !QFile::copy(lameLib, QDir(lib).filePath(QStringLiteral("mp3lame.lib"))))
        return false;

    const QString amfSource = QDir(binariesDirectory()).filePath(QStringLiteral("AMF"));
    const QString amfInclude = QDir(amfSource).filePath(QStringLiteral("amf/public/include"));
    const QString amfHeader = QDir(amfInclude).filePath(QStringLiteral("core/Version.h"));
    if (!QFileInfo(amfHeader).isFile()) {
        const QString git = QStandardPaths::findExecutable(QStringLiteral("git"));
        if (!git.isEmpty()) {
            const ProcessResult clone = ProcessRunner::run(git,
                {QStringLiteral("clone"), QStringLiteral("--depth"), QStringLiteral("1"),
                 QStringLiteral("https://github.com/GPUOpen-LibrariesAndSDKs/AMF.git"), amfSource},
                binariesDirectory(), QProcessEnvironment::systemEnvironment(), 600000, cancel);
            Q_UNUSED(clone);
        }
    }
    const QString stagedAmf = QDir(workspace).filePath(QStringLiteral("_downloader_deps/include/AMF"));
    if (QFileInfo(amfHeader).isFile())
        copyDirectory(amfInclude, stagedAmf);

    env.insert(QStringLiteral("DOWNLOADER_BUILD_ROOT"), QDir::toNativeSeparators(workspace));
    const QString script =
        "set -o pipefail\n"
        "ROOT=\"$(cygpath -u \"$DOWNLOADER_BUILD_ROOT\" 2>/dev/null || printf '%s' \"$DOWNLOADER_BUILD_ROOT\")\"\n"
        "INSTALLROOT=\"$ROOT/_shared_install\"\n"
        "cd \"$ROOT\" || exit 90\n"
        "command -v make >/dev/null 2>&1 || exit 91\n"
        "command -v cl.exe >/dev/null 2>&1 || exit 92\n"
        "command -v link.exe >/dev/null 2>&1 || exit 93\n"
        "rm -rf \"$INSTALLROOT\" && mkdir -p \"$INSTALLROOT\"\n"
        "CFLAGS=\"-I./_downloader_deps/lame/include\"\n"
        "if [ -f ./_downloader_deps/include/AMF/core/Version.h ]; then CFLAGS=\"$CFLAGS -I./_downloader_deps/include\"; fi\n"
        "LDFLAGS=\"/LIBPATH:./_downloader_deps/lame/lib\"\n"
        "X86ASM=\"\"\n"
        "if ! command -v nasm >/dev/null 2>&1 && ! command -v nasm.exe >/dev/null 2>&1; then X86ASM=\"--disable-x86asm\"; fi\n"
        "bash ./configure --prefix=\"$INSTALLROOT\" --toolchain=msvc --disable-doc --disable-ffplay --enable-gpl --enable-version3 --enable-shared --disable-static --enable-d3d11va --enable-dxva2 --enable-libmp3lame $X86ASM --extra-cflags=\"$CFLAGS\" --extra-ldflags=\"$LDFLAGS\" || exit $?\n"
        "JOBS=\"$NUMBER_OF_PROCESSORS\"; if [ -z \"$JOBS\" ]; then JOBS=4; fi\n"
        "make -j\"$JOBS\" || exit $?\n"
        "make install || exit $?\n";
    log(QStringLiteral("Building FFmpeg with native MSVC x64; Bash/GNU Make are build utilities only."));
    const ProcessResult build = ProcessRunner::run(bash, {QStringLiteral("-lc"), script},
        workspace, env, 10800000, cancel,
        [&](const QString &line) { log(QStringLiteral("[ffmpeg-msvc] ") + line); });
    if (!build.started || build.exitCode != 0) {
        log(QStringLiteral("ERROR: MSVC FFmpeg build failed: %1").arg(build.output.right(2500)));
        return false;
    }

    const QString stagedBin = QDir(workspace).filePath(QStringLiteral("_shared_install/bin"));
    const QString stagedLameDll = QDir(stagedBin).filePath(QStringLiteral("libmp3lame.dll"));
    if (!QFileInfo(stagedLameDll).isFile() && !QFile::copy(lameDll, stagedLameDll)) return false;
    const QStringList required{
        QStringLiteral("avcodec-"), QStringLiteral("avdevice-"), QStringLiteral("avfilter-"),
        QStringLiteral("avformat-"), QStringLiteral("avutil-"), QStringLiteral("swresample-"),
        QStringLiteral("swscale-")
    };
    const QStringList dlls = QDir(stagedBin).entryList({QStringLiteral("*.dll")}, QDir::Files);
    for (const QString &prefix : required) {
        bool found = false;
        for (const QString &dll : dlls)
            if (dll.startsWith(prefix, Qt::CaseInsensitive)) { found = true; break; }
        if (!found) {
            log(QStringLiteral("ERROR: MSVC FFmpeg missing shared DLL family %1*.dll.").arg(prefix));
            return false;
        }
    }
    if (!QFileInfo(QDir(stagedBin).filePath(QStringLiteral("ffmpeg.exe"))).isFile() ||
        !QFileInfo(QDir(stagedBin).filePath(QStringLiteral("ffprobe.exe"))).isFile())
        return false;

    const QString outputBin = ffmpegOutputBinDirectory();
    const QString fresh = outputBin + QStringLiteral(".new");
    const QString backup = outputBin + QStringLiteral(".old");
    QDir(fresh).removeRecursively();
    QDir(backup).removeRecursively();
    if (!copyDirectory(stagedBin, fresh)) return false;
    if (!verifyFFmpeg(QDir(fresh).filePath(QStringLiteral("ffmpeg.exe")),
                      QDir(fresh).filePath(QStringLiteral("ffprobe.exe")), log)) {
        QDir(fresh).removeRecursively();
        return false;
    }
    if (QFileInfo::exists(outputBin) && !QDir().rename(outputBin, backup)) return false;
    if (!QDir().rename(fresh, outputBin)) {
        if (QFileInfo::exists(backup)) QDir().rename(backup, outputBin);
        return false;
    }
    QDir(backup).removeRecursively();
    log(QStringLiteral("MSVC FFmpeg runtime installed into %1").arg(outputBin));
    return true;
}
