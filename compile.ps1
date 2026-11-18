[CmdletBinding()]
param(
    [switch]$SkipAppBuild,
    [switch]$SkipToolBuild
)

$ErrorActionPreference = "Stop"
$ProjectRoot = $PSScriptRoot
$BuildRoot = Join-Path $ProjectRoot "build"
$QtRoot = "C:\Qt\Qt-6.12.0"
$MsysRoot = "C:\msys64"
$FfmpegSource = Join-Path $ProjectRoot "binaries\ffmpeg"
$YtdlpSource = Join-Path $ProjectRoot "binaries\yt-dlp"

function Invoke-Native {
    param([Parameter(Mandatory)][string]$File, [string[]]$Arguments = @(), [string]$WorkingDirectory = $ProjectRoot)
    Push-Location $WorkingDirectory
    try {
        & $File @Arguments
        if ($LASTEXITCODE -ne 0) { throw "$File exited with code $LASTEXITCODE" }
    } finally { Pop-Location }
}

Write-Host "Checking build dependencies..."
py -3 -m pip install --upgrade pip cmake pyinstaller --quiet
$Cmake = py -3 -c "import cmake,sys,os; print(os.path.join(os.path.dirname(cmake.__file__),'data','bin','cmake.exe'))"
if (!(Test-Path $Cmake)) { throw "CMake Python package did not provide cmake.exe" }

$Vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (!(Test-Path $Vswhere)) { throw "Visual Studio vswhere.exe was not found" }
$VsPath = & $Vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$VsDevCmd = Join-Path $VsPath "Common7\Tools\VsDevCmd.bat"
if (!(Test-Path $VsDevCmd)) { throw "Visual Studio developer environment was not found" }

if (!$SkipToolBuild) {
    if (!(Test-Path (Join-Path $FfmpegSource "configure"))) { throw "FFmpeg source checkout is missing" }
    if (!(Test-Path (Join-Path $YtdlpSource "yt_dlp\__main__.py"))) { throw "yt-dlp source checkout is missing" }

    $Bash = Join-Path $MsysRoot "usr\bin\bash.exe"
    if (!(Test-Path $Bash)) { throw "MSYS2 bash.exe was not found at $Bash" }
    $env:CHERE_INVOKING = "1"
    $env:MSYSTEM = "UCRT64"
    $env:MSYS2_PATH_TYPE = "inherit"
    $env:PATH = (Join-Path $MsysRoot "ucrt64\bin") + ";" + (Join-Path $MsysRoot "usr\bin") + ";" + $env:PATH
    $Prefix = "mingw-w64-ucrt-x86_64"
    $packages = @(
        "$Prefix-pkgconf", "$Prefix-ffnvcodec-headers", "$Prefix-libvpl",
        "$Prefix-lame", "$Prefix-opus", "$Prefix-libx264", "$Prefix-x265",
        "$Prefix-libvpx", "$Prefix-aom", "make", "nasm"
    )
    & $Bash -lc "pacman -S --needed --noconfirm $($packages -join ' ')"
    if ($LASTEXITCODE -ne 0) { throw "MSYS2 dependency installation failed" }

    $FfmpegWork = "C:\DownloaderFFmpegBundleBuild"
    Remove-Item $FfmpegWork -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $FfmpegWork | Out-Null
    Copy-Item (Join-Path $FfmpegSource "*") $FfmpegWork -Recurse -Force

    $Script = @'
set -e
cd /c/DownloaderFFmpegBundleBuild
PREFIX="$PWD/_bundle"
FLAGS=(--enable-gpl --enable-version3 --enable-shared --disable-static --disable-doc --disable-ffplay --enable-d3d11va --enable-dxva2)
pkg-config --exists lame && FLAGS+=(--enable-libmp3lame) || { echo "libmp3lame required" >&2; exit 42; }
pkg-config --exists opus && FLAGS+=(--enable-libopus)
pkg-config --exists x264 && FLAGS+=(--enable-libx264)
pkg-config --exists x265 && FLAGS+=(--enable-libx265)
pkg-config --exists vpx && FLAGS+=(--enable-libvpx)
pkg-config --exists aom && FLAGS+=(--enable-libaom)
pkg-config --exists ffnvcodec && FLAGS+=(--enable-ffnvcodec --enable-nvenc --enable-nvdec)
pkg-config --exists vpl && FLAGS+=(--enable-libvpl)
command -v nasm >/dev/null 2>&1 || FLAGS+=(--disable-x86asm)
./configure --prefix="$PREFIX" --extra-cflags="-O3" "${FLAGS[@]}"
make -j"$NUMBER_OF_PROCESSORS"
make install
'@
    Set-Content -Path (Join-Path $FfmpegWork ".bundle-build.sh") -Value $Script -NoNewline
    & $Bash -lc "bash /c/DownloaderFFmpegBundleBuild/.bundle-build.sh"
    if ($LASTEXITCODE -ne 0) { throw "FFmpeg compilation failed" }

    $FfmpegBin = Join-Path $ProjectRoot "binaries\ffmpeg-build\bin"
    Remove-Item $FfmpegBin -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $FfmpegBin | Out-Null
    Copy-Item (Join-Path $FfmpegWork "_bundle\bin\*") $FfmpegBin -Recurse -Force

    $YtdlpDist = Join-Path $BuildRoot "yt-dlp"
    Remove-Item $YtdlpDist -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $YtdlpDist | Out-Null
    Invoke-Native -File "py" -Arguments @(
        "-3","-m","PyInstaller","--noconfirm","--clean","--onefile",
        "--name","yt-dlp","--distpath",$YtdlpDist,
        "--workpath",(Join-Path $BuildRoot "yt-dlp-work"),
        "--specpath",$BuildRoot,(Join-Path $YtdlpSource "yt_dlp\__main__.py")
    ) -WorkingDirectory $YtdlpSource
}

$FfmpegBin = Join-Path $ProjectRoot "binaries\ffmpeg-build\bin"
$Bash = Join-Path $MsysRoot "usr\bin\bash.exe"
if ((Test-Path (Join-Path $FfmpegBin "ffmpeg.exe")) -and (Test-Path $Bash)) {
    Copy-Item (Join-Path $MsysRoot "ucrt64\bin\*.dll") $FfmpegBin -Force
}

if (!$SkipAppBuild) {
    $ConfigureCommand = "`"$Cmake`" -S `"$ProjectRoot`" -B `"$BuildRoot\app`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=`"$QtRoot`" && `"$Cmake`" --build `"$BuildRoot\app`" --parallel"
    cmd.exe /d /c "call `"$VsDevCmd`" -arch=x64 -host_arch=x64 >nul && $ConfigureCommand"
    if ($LASTEXITCODE -ne 0) { throw "C++ application build failed" }
}

$AppExe = Join-Path $BuildRoot "app\downloader.exe"
if (!(Test-Path $AppExe)) { throw "Built app executable was not found" }
$QtDeploy = Join-Path $QtRoot "bin\windeployqt.exe"
& $QtDeploy "--release" "--compiler-runtime" "--no-translations" "--no-system-d3d-compiler" "--no-opengl-sw" $AppExe
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }

$FfmpegBin = Join-Path $ProjectRoot "binaries\ffmpeg-build\bin"
$YtdlpExe = Join-Path $BuildRoot "yt-dlp\yt-dlp.exe"
if (!(Test-Path (Join-Path $FfmpegBin "ffmpeg.exe"))) { throw "Compiled ffmpeg.exe was not found" }
if (!(Test-Path (Join-Path $FfmpegBin "ffprobe.exe"))) { throw "Compiled ffprobe.exe was not found" }
if (!(Test-Path $YtdlpExe)) { throw "Compiled yt-dlp.exe was not found" }

$LauncherSpec = Join-Path $BuildRoot "launcher"
Remove-Item $LauncherSpec -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $LauncherSpec | Out-Null

$PyInstallerArgs = @(
    "-3","-m","PyInstaller","--noconfirm","--clean","--onefile","--noconsole",
    "--name","downloader",
    "--distpath",$ProjectRoot,
    "--workpath",(Join-Path $BuildRoot "launcher-work"),
    "--specpath",$LauncherSpec,
    "--add-data",(Join-Path $BuildRoot "app;downloader"),
    "--add-data",($FfmpegBin + ";binaries/ffmpeg-build/bin"),
    "--add-binary",(Join-Path $YtdlpExe ";binaries/yt-dlp/yt-dlp.exe"),
    (Join-Path $ProjectRoot "src\launcher.py")
)
Invoke-Native -File "py" -Arguments $PyInstallerArgs -WorkingDirectory $ProjectRoot

$FinalExe = Join-Path $ProjectRoot "downloader.exe"
if (!(Test-Path $FinalExe)) { throw "Final one-file executable was not created" }
Write-Host "Built self-contained executable: $FinalExe"
