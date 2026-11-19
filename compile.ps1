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

function Ensure-GitSource {
    param(
        [Parameter(Mandatory)][string]$Destination,
        [Parameter(Mandatory)][string]$Marker,
        [Parameter(Mandatory)][string[]]$Urls
    )
    if (Test-Path (Join-Path $Destination $Marker)) { return }

    New-Item -ItemType Directory -Force -Path (Split-Path $Destination -Parent) | Out-Null
    if (Test-Path $Destination) {
        Remove-Item $Destination -Recurse -Force
    }
    $Git = Get-Command git -ErrorAction SilentlyContinue
    if (!$Git) { throw "git is required to clone build sources" }
    foreach ($Url in $Urls) {
        Write-Host "Cloning $Url..."
        & $Git.Source clone --depth 1 $Url $Destination
        if ($LASTEXITCODE -eq 0 -and (Test-Path (Join-Path $Destination $Marker))) { return }
        if (Test-Path $Destination) {
            Remove-Item $Destination -Recurse -Force
        }
    }
    throw "Could not clone source checkout into $Destination"
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
    Ensure-GitSource -Destination $FfmpegSource -Marker "configure" -Urls @(
        "https://git.ffmpeg.org/ffmpeg.git",
        "https://github.com/FFmpeg/FFmpeg.git"
    )
    Ensure-GitSource -Destination $YtdlpSource -Marker "yt_dlp\__main__.py" -Urls @(
        "https://github.com/yt-dlp/yt-dlp.git"
    )

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

    $AmfRoot = Join-Path $ProjectRoot "binaries\dependencies\AMF"
    $AmfInclude = Join-Path $AmfRoot "amf\public\include"
    if (!(Test-Path (Join-Path $AmfInclude "core\Version.h"))) {
        Remove-Item $AmfRoot -Recurse -Force -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force -Path (Split-Path $AmfRoot -Parent) | Out-Null
        Invoke-Native -File "git" -Arguments @("clone", "--depth", "1", "https://github.com/GPUOpen-LibrariesAndSDKs/AMF.git", $AmfRoot)
    }
    $StagedAmf = Join-Path $FfmpegWork "_downloader_deps\amf\AMF"
    New-Item -ItemType Directory -Force -Path (Split-Path $StagedAmf -Parent) | Out-Null
    Copy-Item $AmfInclude $StagedAmf -Recurse -Force

    $Script = @'
set -e
cd /c/DownloaderFFmpegBundleBuild
PREFIX="$PWD/_bundle"
FLAGS=(--enable-gpl --enable-version3 --enable-shared --disable-static --disable-doc --disable-ffplay --enable-d3d11va --enable-dxva2)
EXTRA_CFLAGS=("-O3")
if [ -d "$PWD/_downloader_deps/amf/AMF" ]; then
    FLAGS+=(--enable-amf)
    EXTRA_CFLAGS+=("-I$PWD/_downloader_deps/amf")
fi
pkg-config --exists lame && FLAGS+=(--enable-libmp3lame) || { echo "libmp3lame required" >&2; exit 42; }
pkg-config --exists opus && FLAGS+=(--enable-libopus)
pkg-config --exists x264 && FLAGS+=(--enable-libx264)
pkg-config --exists x265 && FLAGS+=(--enable-libx265)
pkg-config --exists vpx && FLAGS+=(--enable-libvpx)
pkg-config --exists aom && FLAGS+=(--enable-libaom)
pkg-config --exists ffnvcodec && FLAGS+=(--enable-ffnvcodec --enable-nvenc --enable-nvdec)
pkg-config --exists vpl && FLAGS+=(--enable-libvpl)
command -v nasm >/dev/null 2>&1 || FLAGS+=(--disable-x86asm)
./configure --prefix="$PREFIX" --extra-cflags="${EXTRA_CFLAGS[*]}" "${FLAGS[@]}"
make -j"$NUMBER_OF_PROCESSORS"
make install
'@
    Set-Content -Path (Join-Path $FfmpegWork ".bundle-build.sh") -Value $Script -NoNewline
    & $Bash -lc "bash /c/DownloaderFFmpegBundleBuild/.bundle-build.sh"
    if ($LASTEXITCODE -ne 0) { throw "FFmpeg compilation failed" }

    $FfmpegBuildDir = Join-Path $ProjectRoot "binaries\ffmpeg-build"
    $FfmpegBin = Join-Path $FfmpegBuildDir "bin"
    Remove-Item $FfmpegBuildDir -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $FfmpegBin | Out-Null
    Copy-Item (Join-Path $FfmpegWork "_bundle\bin\*") $FfmpegBin -Recurse -Force
    if (Test-Path (Join-Path $FfmpegWork "ffbuild\config.log")) {
        Copy-Item (Join-Path $FfmpegWork "ffbuild\config.log") (Join-Path $FfmpegBuildDir "configure.log") -Force
    }
    '{"profile":"all-vendors-win-linux-v5-required-codecs","platform":"win32"}' | Set-Content -Path (Join-Path $FfmpegBuildDir ".downloader-build-profile.json") -NoNewline

}

$YtdlpDist = Join-Path $BuildRoot "yt-dlp"
$YtdlpExe = Join-Path $YtdlpDist "yt-dlp.exe"
if (!$SkipToolBuild -or !(Test-Path $YtdlpExe)) {
    if (!(Test-Path (Join-Path $YtdlpSource "yt_dlp\__main__.py"))) { throw "yt-dlp source checkout is missing" }
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
$Objdump = Join-Path $MsysRoot "ucrt64\bin\objdump.exe"
if ((Test-Path (Join-Path $FfmpegBin "ffmpeg.exe")) -and (Test-Path $Objdump)) {
    $System32 = Join-Path $env:SystemRoot "System32"
    $Inspected = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    $Queue = New-Object 'System.Collections.Generic.Queue[string]'
    Get-ChildItem -Path $FfmpegBin -File | Where-Object { $_.Extension -in '.exe', '.dll' } | ForEach-Object { $Queue.Enqueue($_.FullName) }
    while ($Queue.Count -gt 0) {
        $Current = $Queue.Dequeue()
        $Leaf = [System.IO.Path]::GetFileName($Current)
        if (-not $Inspected.Add($Leaf)) { continue }
        $Output = & $Objdump -p $Current 2>$null
        foreach ($Line in $Output) {
            if ($Line -match 'DLL Name:\s*([^\s]+)') {
                $Dll = $Matches[1]
                if ($Inspected.Contains($Dll)) { continue }
                if ($Dll -like 'api-ms-win-*' -or $Dll -like 'ext-ms-*') { continue }
                if (Test-Path (Join-Path $System32 $Dll)) { continue }
                $Dest = Join-Path $FfmpegBin $Dll
                if (Test-Path $Dest) { $Queue.Enqueue($Dest); continue }
                $Src = @( (Join-Path $MsysRoot "ucrt64\bin\$Dll"), (Join-Path $MsysRoot "usr\bin\$Dll") ) | Where-Object { Test-Path $_ } | Select-Object -First 1
                if ($Src) {
                    Copy-Item $Src $Dest -Force
                    $Queue.Enqueue($Dest)
                }
            }
        }
    }
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

Copy-Item (Join-Path $BuildRoot "app\*") $ProjectRoot -Recurse -Force

$FfmpegDestination = Join-Path $ProjectRoot "binaries\ffmpeg-build\bin"
New-Item -ItemType Directory -Force -Path $FfmpegDestination | Out-Null
if ([System.IO.Path]::GetFullPath($FfmpegBin).TrimEnd('\') -ne [System.IO.Path]::GetFullPath($FfmpegDestination).TrimEnd('\')) {
    Copy-Item (Join-Path $FfmpegBin "*") $FfmpegDestination -Recurse -Force
}

$YtdlpDestination = Join-Path $ProjectRoot "binaries\yt-dlp"
New-Item -ItemType Directory -Force -Path $YtdlpDestination | Out-Null
Copy-Item $YtdlpExe $YtdlpDestination -Force

$FinalExe = Join-Path $ProjectRoot "downloader.exe"
if (!(Test-Path $FinalExe)) { throw "Final C++ executable was not created" }
Write-Host "Built native C++ executable: $FinalExe"
