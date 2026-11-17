# Builds one Downloader.exe that contains the Qt app, FFmpeg, FFprobe, and yt-dlp.
# The first launch unpacks that runtime under %LOCALAPPDATA%\Downloader\runtime.

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$QtRoot = "C:\Qt\Qt-6.12.0"
$BuildDir = Join-Path $Root "build"
$DistDir = Join-Path $Root "dist"
$CacheDir = Join-Path $DistDir "cache"
$StageDir = Join-Path $DistDir "stage"
$PayloadZip = Join-Path $DistDir "payload.zip"
$StubExe = Join-Path $DistDir "bundle_stub.exe"
$FinalExe = Join-Path $DistDir "Downloader.exe"

function Write-Step([string]$Message) {
    Write-Host ""
    Write-Host "== $Message" -ForegroundColor Cyan
}

function Get-File([string]$Url, [string]$Destination) {
    if (Test-Path $Destination) {
        Write-Host "Using cached $(Split-Path -Leaf $Destination)"
        return
    }
    Write-Host "Downloading $Url"
    Invoke-WebRequest -Uri $Url -OutFile $Destination -UseBasicParsing
}

function Copy-CompilerRuntime([string]$Destination) {
    $roots = @(
        "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Redist\MSVC",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC"
    )
    $runtimeDir = $null
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        $candidate = Get-ChildItem $root -Directory | Sort-Object Name -Descending |
            ForEach-Object { Join-Path $_.FullName "x64\Microsoft.VC143.CRT" } |
            Where-Object { Test-Path (Join-Path $_ "vcruntime140.dll") } |
            Select-Object -First 1
        if ($candidate) { $runtimeDir = $candidate; break }
    }
    if (-not $runtimeDir) {
        $found = Get-Command vcruntime140.dll -ErrorAction SilentlyContinue
        if ($found) { $runtimeDir = Split-Path -Parent $found.Source }
    }
    if (-not $runtimeDir) {
        Write-Warning "Visual C++ runtime DLLs were not found. The executable still needs the matching redistributable on this PC."
        return
    }
    Write-Host "Copying Visual C++ runtime from $runtimeDir"
    $names = @("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "concrt140.dll", "vccorlib140.dll")
    foreach ($name in $names) {
        $file = Join-Path $runtimeDir $name
        if (Test-Path $file) { Copy-Item $file $Destination -Force }
    }
}

Set-Location $Root
New-Item -ItemType Directory -Force -Path $CacheDir | Out-Null
if (Test-Path $StageDir) { Remove-Item $StageDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $StageDir | Out-Null

Write-Step "Compile the Qt application"
cmake -S $Root -B $BuildDir "-DCMAKE_PREFIX_PATH=$QtRoot"
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }
cmake --build $BuildDir --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "C++ build failed." }

$AppExe = Join-Path $BuildDir "Release\downloader.exe"
if (-not (Test-Path $AppExe)) { throw "Missing $AppExe" }
Copy-Item $AppExe (Join-Path $StageDir "downloader.exe") -Force

Write-Step "Deploy Qt libraries"
& (Join-Path $QtRoot "bin\windeployqt.exe") --release --no-translations --no-opengl-sw (Join-Path $StageDir "downloader.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed." }
Copy-CompilerRuntime $StageDir

Write-Step "Stage FFmpeg and yt-dlp"
$Ffmpeg = $null
$Ffprobe = $null
$LocalBin = Join-Path $Root "binaries\ffmpeg-build\bin"
if ((Test-Path (Join-Path $LocalBin "ffmpeg.exe")) -and (Test-Path (Join-Path $LocalBin "ffprobe.exe"))) {
    Write-Host "Using the local FFmpeg build in binaries\ffmpeg-build\bin"
    Copy-Item (Join-Path $LocalBin "*") $StageDir -Force
    $Ffmpeg = Join-Path $StageDir "ffmpeg.exe"
    $Ffprobe = Join-Path $StageDir "ffprobe.exe"
} else {
    $FfmpegZip = Join-Path $CacheDir "ffmpeg-master-latest-win64-gpl.zip"
    Get-File "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip" $FfmpegZip
    $FfmpegExtract = Join-Path $CacheDir "ffmpeg"
    if (Test-Path $FfmpegExtract) { Remove-Item $FfmpegExtract -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $FfmpegExtract | Out-Null
    tar.exe -xf $FfmpegZip -C $FfmpegExtract
    if ($LASTEXITCODE -ne 0) { throw "Could not unpack the FFmpeg archive." }
    $Ffmpeg = Get-ChildItem $FfmpegExtract -Recurse -Filter ffmpeg.exe | Select-Object -First 1 -ExpandProperty FullName
    $Ffprobe = Get-ChildItem $FfmpegExtract -Recurse -Filter ffprobe.exe | Select-Object -First 1 -ExpandProperty FullName
    if (-not $Ffmpeg -or -not $Ffprobe) { throw "The FFmpeg archive did not contain ffmpeg.exe and ffprobe.exe." }
    Copy-Item $Ffmpeg $StageDir -Force
    Copy-Item $Ffprobe $StageDir -Force
    $Ffmpeg = Join-Path $StageDir "ffmpeg.exe"
    $Ffprobe = Join-Path $StageDir "ffprobe.exe"
}

$encoderList = & $Ffmpeg -hide_banner -encoders 2>&1 | Out-String
if ($encoderList -notmatch "libmp3lame") { throw "The staged FFmpeg does not include the required libmp3lame encoder." }

$YtDlp = Join-Path $CacheDir "yt-dlp.exe"
Get-File "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe" $YtDlp
Copy-Item $YtDlp (Join-Path $StageDir "yt-dlp.exe") -Force
& (Join-Path $StageDir "yt-dlp.exe") --version
if ($LASTEXITCODE -ne 0) { throw "The staged yt-dlp executable did not run." }

Write-Step "Pack the runtime into one executable"
if (Test-Path $PayloadZip) { Remove-Item $PayloadZip -Force }
tar.exe -a -cf $PayloadZip -C $StageDir .
if ($LASTEXITCODE -ne 0) { throw "Could not create the runtime archive." }

$StubSource = Join-Path $Root "packer\bundle_stub.cpp"
& cl.exe /nologo /O2 /W4 /EHsc /DUNICODE /D_UNICODE /utf-8 $StubSource /Fo:(Join-Path $DistDir "bundle_stub.obj") /Fe:$StubExe /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup user32.lib shell32.lib
if ($LASTEXITCODE -ne 0) { throw "Could not compile the single-file launcher." }

$zipInfo = Get-Item $PayloadZip
$out = [System.IO.File]::Create($FinalExe)
try {
    $stub = [System.IO.File]::OpenRead($StubExe)
    try { $stub.CopyTo($out) } finally { $stub.Dispose() }
    $zip = [System.IO.File]::OpenRead($PayloadZip)
    try { $zip.CopyTo($out) } finally { $zip.Dispose() }
    $magic = [System.Text.Encoding]::ASCII.GetBytes("DWNBNDL1")
    $size = [System.BitConverter]::GetBytes([UInt64]$zipInfo.Length)
    $out.Write($magic, 0, $magic.Length)
    $out.Write($size, 0, $size.Length)
} finally {
    $out.Dispose()
}

$megabytes = [math]::Round((Get-Item $FinalExe).Length / 1MB, 1)
Write-Step "Done"
Write-Host "Created $FinalExe ($megabytes MB)"
Write-Host "It contains Downloader, Qt, FFmpeg, FFprobe, and yt-dlp. The first launch unpacks them into %LOCALAPPDATA%\Downloader\runtime."
