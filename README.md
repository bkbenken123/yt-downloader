# Downloader (C++ / Qt 6)

This branch replaces the Python GUI/application in downloader.py with a native C++20 and Qt 6 application. The only non-C++ runtime components are external tools discovered under binaries/ or on PATH (yt-dlp, FFmpeg/FFprobe, Git, and optional platform build tools).

## Features

- Video, audio, and thumbnail-only downloads.
- Multiple URLs separated by semicolons.
- Playlist downloads, playlist-title folders, and per-link processing.
- Dynamic container/codec compatibility for MP4, MKV, WebM, MOV, MP3, M4A, WAV, FLAC, Opus, JPEG, PNG, and WebP.
- yt-dlp audio conversion when enabled and compatible; FFmpeg conversion for the remaining audio/video paths.
- CPU, AMD AMF, Intel QSV, NVIDIA NVENC, and Linux AMD/Intel VAAPI encoder selection with hardware-decode attempts and CPU fallback.
- FFprobe validation, safe temporary outputs, rollback-preserving replacement, cancellation, and cleanup of incomplete downloads.
- Metadata/chapter embedding and FFmpeg-based cover-art embedding. Opus picture metadata is generated as a FLAC picture block and passed through FFmpeg's metadata muxing path; no Mutagen package is used by the application.
- YouTube Music thumbnail URL handling and conservative detection/cropping of centered square cover art wrapped in flat 16:9 side borders.
- Atomic config.json settings, startup dependency checks, and an explicit Tools & App update action.
- FFmpeg runtime/build separation. Compiled FFmpeg runtime files are installed in binaries/ffmpeg-build/bin/; source checkouts are not part of the repository.

## Build

Required: CMake 3.24 or newer, a C++20 compiler, and Qt 6.6 or newer with Core, Gui, Widgets, and Network modules.

Install a C++20 toolchain, CMake, Qt 6 development packages, FFmpeg build dependencies, pkg-config, git, bash, and GNU make. Configure and build:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel

The FFmpeg builder enables optional hardware/codec features for which development packages are installed. Linux AMD/Intel VAAPI requires the relevant libva development packages and working /dev/dri render-node permissions.

## Runtime tool discovery

The application searches the dedicated build directory first, then the bundled binaries/ directory and PATH.

- FFmpeg and FFprobe: binaries/ffmpeg-build/bin/ or PATH.
- yt-dlp: binaries/yt-dlp/, binaries/yt-dlp_linux on Linux, a packaged executable, or a system installation.

The application invokes yt-dlp as an external tool. No downloader GUI, worker, settings, media pipeline, or application logic is implemented in Python.

## Settings

config.json is written beside the application source root (the directory containing binaries/) and is updated atomically only when settings change. The defaults are CPU encoding, playlist downloads enabled, yt-dlp audio conversion enabled, and metadata/thumbnail/chapter embedding disabled.
