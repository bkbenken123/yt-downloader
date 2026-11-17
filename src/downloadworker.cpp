#include "downloadworker.h"
#include "mediaengine.h"
#include "process.h"
#include "toolmanager.h"

#include <QDir>
#include <QHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <algorithm>

DownloadWorker::DownloadWorker(DownloadOptions options, QObject *parent)
    : QThread(parent), options_(std::move(options)) {}

void DownloadWorker::cancel() {
    cancelled_.store(true, std::memory_order_relaxed);
}

bool DownloadWorker::isCancelled() const {
    return cancelled_.load(std::memory_order_relaxed);
}

void DownloadWorker::log(const QString &message) {
    emit logMessage(message);
}

bool DownloadWorker::ytdlpVideoNative() const {
    return options_.mode == QStringLiteral("video") &&
           options_.videoCodec == QStringLiteral("copy") &&
           options_.audioCodec == QStringLiteral("copy");
}

bool DownloadWorker::ytdlpAudioNative() const {
    return options_.mode == QStringLiteral("audio") &&
           options_.settings.useYtdlpAudioConversion &&
           !options_.settings.embedMetadata &&
           options_.container.toLower() == QStringLiteral("mp3") &&
           options_.audioCodec == QStringLiteral("mpeg-1");
}

void DownloadWorker::logCapabilities() {
    const FFmpegFeatures features = MediaEngine::detectFeatures();
    log(QStringLiteral("Build: 2026.11.17-cpp"));
    log(QStringLiteral("Encoder backend: %1").arg(options_.settings.encoderBackend));
    log(QStringLiteral("Playlist downloads: %1").arg(options_.settings.downloadPlaylist ? QStringLiteral("enabled") : QStringLiteral("disabled")));
    log(QStringLiteral("yt-dlp audio conversion: %1").arg(options_.settings.useYtdlpAudioConversion ? QStringLiteral("enabled") : QStringLiteral("disabled")));
    log(QStringLiteral("Thumbnail, metadata, and chapter embedding: %1").arg(options_.settings.embedMetadata ? QStringLiteral("enabled") : QStringLiteral("disabled")));
    log(QStringLiteral("FFmpeg: %1").arg(features.ffmpeg.isEmpty() ? QStringLiteral("not found") : features.ffmpeg));
    if (features.hwaccels.isEmpty()) log(QStringLiteral("FFmpeg reports no hardware acceleration methods."));
    else log(QStringLiteral("FFmpeg hwaccels: %1").arg(features.hwaccels.join(QStringLiteral(", "))));
    if (options_.settings.encoderBackend != QStringLiteral("CPU") && options_.videoCodec != QStringLiteral("copy")) {
        QStringList expected;
        if (options_.settings.encoderBackend == QStringLiteral("AMD")) {
            if (options_.videoCodec == QStringLiteral("h264")) expected = {QStringLiteral("h264_amf")};
            if (options_.videoCodec == QStringLiteral("h265")) expected = {QStringLiteral("hevc_amf")};
            if (options_.videoCodec == QStringLiteral("av1")) expected = {QStringLiteral("av1_amf")};
        } else if (options_.settings.encoderBackend == QStringLiteral("INTEL")) {
            if (options_.videoCodec == QStringLiteral("h264")) expected = {QStringLiteral("h264_qsv")};
            if (options_.videoCodec == QStringLiteral("h265")) expected = {QStringLiteral("hevc_qsv")};
            if (options_.videoCodec == QStringLiteral("vp9")) expected = {QStringLiteral("vp9_qsv")};
            if (options_.videoCodec == QStringLiteral("av1")) expected = {QStringLiteral("av1_qsv")};
        } else if (options_.settings.encoderBackend == QStringLiteral("NVIDIA")) {
            if (options_.videoCodec == QStringLiteral("h264")) expected = {QStringLiteral("h264_nvenc")};
            if (options_.videoCodec == QStringLiteral("h265")) expected = {QStringLiteral("hevc_nvenc")};
            if (options_.videoCodec == QStringLiteral("av1")) expected = {QStringLiteral("av1_nvenc")};
        }
        QStringList available;
        for (const QString &name : expected)
            if (features.encoders.contains(name)) available << name;
        log(available.isEmpty()
            ? QStringLiteral("No compiled %1 encoder for %2; the media engine will use its CPU fallback.")
                .arg(options_.settings.encoderBackend, options_.videoCodec)
            : QStringLiteral("Compiled %1 encoder(s): %2").arg(options_.settings.encoderBackend, available.join(QStringLiteral(", "))));
    }
}

QString DownloadWorker::playlistTitle(const QString &url) {
    if (!options_.settings.downloadPlaylist) return {};
    const ToolPaths tools = ToolManager::locate();
    if (tools.ytdlpCommand.isEmpty()) return {};
    const QString tempDir = ToolManager::safePlaylistTempDirectory();
    if (tempDir.isEmpty()) {
        log(QStringLiteral("Playlist title lookup skipped: could not create yt-dlp temporary directory."));
        return {};
    }
    QStringList args = tools.ytdlpCommand.mid(1);
    args << url << QStringLiteral("--dump-single-json") << QStringLiteral("--flat-playlist")
         << QStringLiteral("--no-warnings") << QStringLiteral("--yes-playlist")
         << QStringLiteral("--paths") << QStringLiteral("temp:") + tempDir;
    const ProcessResult result = ProcessRunner::run(tools.ytdlpCommand.first(), args, {},
        QProcessEnvironment::systemEnvironment(), 45000, &cancelled_);
    if (result.cancelled) return {};
    if (result.exitCode != 0) {
        if (!result.output.trimmed().isEmpty())
            log(QStringLiteral("Playlist title lookup failed: %1").arg(result.output.trimmed().right(800)));
        return {};
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(result.output.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return {};
    const QJsonObject data = document.object();
    if (!data.value(QStringLiteral("entries")).isArray() || data.value(QStringLiteral("entries")).toArray().isEmpty())
        return {};
    const QString title = data.value(QStringLiteral("title")).toString().trimmed();
    return title.isEmpty() ? QStringLiteral("playlist") : title;
}

QString DownloadWorker::outputFolderForUrl(const QString &url) {
    QString folder = options_.outputDirectory;
    const QString title = playlistTitle(url);
    if (!title.isEmpty()) {
        QString safe = title;
        safe.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]")), QStringLiteral("_"));
        safe = safe.trimmed();
        while (safe.endsWith(QLatin1Char('.'))) safe.chop(1);
        if (safe.isEmpty()) safe = QStringLiteral("playlist");
        folder = QDir(folder).filePath(safe);
    }
    if (!QDir().mkpath(folder))
        log(QStringLiteral("ERROR: Could not create output directory: %1").arg(folder));
    return QDir::cleanPath(folder);
}

QHash<QString, DownloadWorker::FileSignature> DownloadWorker::snapshotFiles(const QString &folder) const {
    static const QSet<QString> extensions{
        QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("webm"), QStringLiteral("mov"),
        QStringLiteral("mp3"), QStringLiteral("m4a"), QStringLiteral("wav"), QStringLiteral("flac"),
        QStringLiteral("opus"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"),
        QStringLiteral("webp"), QStringLiteral("m4v"), QStringLiteral("mka"), QStringLiteral("mp2"),
        QStringLiteral("mpg"), QStringLiteral("mpeg"), QStringLiteral("avi"), QStringLiteral("flv"),
        QStringLiteral("3gp"), QStringLiteral("ts"), QStringLiteral("m2ts"), QStringLiteral("ogg"),
        QStringLiteral("aac")
    };
    QHash<QString, FileSignature> result;
    const QFileInfoList entries = QDir(folder).entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo &info : entries) {
        if (!extensions.contains(info.suffix().toLower())) continue;
        QString key = QDir::cleanPath(info.absoluteFilePath());
#ifdef Q_OS_WIN
        key = key.toCaseFolded();
#endif
        result.insert(key, {info.lastModified().toMSecsSinceEpoch(), info.size()});
    }
    return result;
}

QStringList DownloadWorker::changedFiles(const QString &folder,
    const QHash<QString, FileSignature> &before) const {
    QStringList result;
    const QHash<QString, FileSignature> after = snapshotFiles(folder);
    for (auto it = after.cbegin(); it != after.cend(); ++it) {
        if (before.contains(it.key()) && before.value(it.key()).modifiedMs == it.value().modifiedMs &&
            before.value(it.key()).size == it.value().size)
            continue;
        result << it.key();
    }
    return result;
}

bool DownloadWorker::runYtdlp(const QString &url, const QString &outputTemplate,
                              const QString &mode, QStringList *errors,
                              QStringList *foundPaths) {
    const ToolPaths tools = ToolManager::locate();
    if (tools.ytdlpCommand.isEmpty()) {
        log(QStringLiteral("ERROR: yt-dlp was not found."));
        return false;
    }
    const bool playlist = options_.settings.downloadPlaylist;
    const bool embedMetadata = options_.settings.embedMetadata &&
        (mode == QStringLiteral("audio") || mode == QStringLiteral("video"));
    const bool nativeAudio = ytdlpAudioNative();
    const bool nativeVideo = ytdlpVideoNative();
    QString reason;
    const QString downloadUrl = MediaEngine::selectThumbnailUrl(url, mode, embedMetadata, &reason);
    if (embedMetadata) log(QStringLiteral("Thumbnail source selected: %1").arg(reason));
    if (downloadUrl != url)
        log(QStringLiteral("Video mode received a YouTube Music URL; using regular YouTube for its standard 16:9 thumbnail."));

    const QString tempDir = ToolManager::safePlaylistTempDirectory();
    if (tempDir.isEmpty()) {
        log(QStringLiteral("ERROR: Could not create a writable yt-dlp temporary directory."));
        return false;
    }
    QStringList args = tools.ytdlpCommand.mid(1);
    args << downloadUrl << QStringLiteral("-o") << outputTemplate
         << QStringLiteral("--no-warnings") << QStringLiteral("--no-color")
         << QStringLiteral("--newline") << QStringLiteral("--force-overwrites")
         << QStringLiteral("--no-embed-subs") << QStringLiteral("--no-write-subs")
         << QStringLiteral("--no-write-auto-subs")
         << (playlist ? QStringLiteral("--yes-playlist") : QStringLiteral("--no-playlist"))
         << QStringLiteral("--paths") << (QStringLiteral("temp:") + tempDir);
    if (playlist) args << QStringLiteral("--ignore-errors");

    if (embedMetadata) {
        args << QStringLiteral("--embed-metadata") << QStringLiteral("--embed-chapters");
        // Artwork is deliberately handed to FFmpeg. This avoids yt-dlp's Mutagen
        // path and ensures the final muxer owns the actual cover-art operation.
        args << QStringLiteral("--no-embed-thumbnail")
             << QStringLiteral("--write-thumbnail")
             << QStringLiteral("--convert-thumbnails") << QStringLiteral("jpg");
        if (mode == QStringLiteral("audio"))
            log(QStringLiteral("FFmpeg will embed the downloaded sidecar thumbnail in the final audio output."));
        else
            log(QStringLiteral("FFmpeg will attach the downloaded thumbnail during video remux/encode."));
    }

    if (mode == QStringLiteral("thumbnail")) {
        args << QStringLiteral("--skip-download") << QStringLiteral("--write-thumbnail");
    } else if (mode == QStringLiteral("audio")) {
        args << QStringLiteral("-f") << QStringLiteral("bestaudio/best");
        if (nativeAudio) {
            args << QStringLiteral("-x") << QStringLiteral("--audio-format") << options_.container;
            log(QStringLiteral("yt-dlp native audio conversion enabled: --audio-format %1").arg(options_.container));
        } else if (!options_.settings.useYtdlpAudioConversion) {
            log(QStringLiteral("yt-dlp audio conversion is disabled; FFmpeg will convert the audio."));
        }
    } else {
        args << QStringLiteral("-f") << QStringLiteral("bestvideo+bestaudio/best");
        args << QStringLiteral("--merge-output-format")
             << (nativeVideo ? options_.container : QStringLiteral("mkv"));
        if (nativeVideo)
            log(QStringLiteral("yt-dlp native video stream merge enabled (copy/copy)."));
    }

    if (mode != QStringLiteral("thumbnail") && !tools.ffmpeg.isEmpty())
        args << QStringLiteral("--ffmpeg-location") << QFileInfo(tools.ffmpeg).absolutePath();

    log(QStringLiteral("Starting yt-dlp: %1").arg(
        ProcessRunner::quoteCommand(tools.ytdlpCommand.first(), args)));

    static const QRegularExpression destinationRe(QStringLiteral("Destination:\\s+(.*)"));
    static const QRegularExpression mergingRe(QStringLiteral("Merging formats into\\s*(?:\"([^\"]+)\"|(.+))"));
    static const QRegularExpression thumbnailRe(QStringLiteral("Writing video thumbnail\\s+\\d+\\s+to:\\s+(.*)"));
    const ProcessResult process = ProcessRunner::run(tools.ytdlpCommand.first(), args, {},
        QProcessEnvironment::systemEnvironment(), 0, &cancelled_,
        [&](const QString &line) {
            if (line.trimmed().isEmpty()) return;
            log(line);
            if (line.trimmed().startsWith(QStringLiteral("ERROR:"), Qt::CaseInsensitive) && errors)
                errors->append(line.trimmed());
            auto recordPath = [&](const QString &value) {
                QString path = value.trimmed();
                if (path.startsWith(QLatin1Char('"')) && path.endsWith(QLatin1Char('"')) && path.size() >= 2)
                    path = path.mid(1, path.size() - 2);
                if (QFileInfo::exists(path) && foundPaths) foundPaths->append(QFileInfo(path).absoluteFilePath());
            };
            const auto dest = destinationRe.match(line);
            if (dest.hasMatch()) recordPath(dest.captured(1));
            const auto merge = mergingRe.match(line);
            if (merge.hasMatch()) recordPath(merge.captured(1).isEmpty() ? merge.captured(2) : merge.captured(1));
            const auto thumb = thumbnailRe.match(line);
            if (thumb.hasMatch()) recordPath(thumb.captured(1));
        });
    if (process.cancelled || isCancelled()) return false;

    static const QStringList unavailable{
        QStringLiteral("this video is not available"), QStringLiteral("video unavailable"),
        QStringLiteral("this video is private"), QStringLiteral("private video"),
        QStringLiteral("has been removed by the uploader"), QStringLiteral("this video has been removed")
    };
    bool onlyUnavailable = playlist && errors && !errors->isEmpty();
    if (onlyUnavailable) {
        for (const QString &error : *errors) {
            const QString normalized = error.toLower();
            bool matched = false;
            for (const QString &marker : unavailable)
                if (normalized.contains(marker)) { matched = true; break; }
            if (!matched) { onlyUnavailable = false; break; }
        }
    }
    if (errors && !errors->isEmpty() && !onlyUnavailable) {
        log(QStringLiteral("ERROR: yt-dlp reported %1 error(s); see the log above.").arg(errors->size()));
        return false;
    }
    if (process.exitCode != 0 && !onlyUnavailable) {
        log(QStringLiteral("ERROR: yt-dlp exited with code %1.").arg(process.exitCode));
        return false;
    }
    if (onlyUnavailable)
        log(QStringLiteral("yt-dlp skipped %1 unavailable playlist item(s); other entries completed.").arg(errors->size()));
    else
        log(QStringLiteral("yt-dlp finished."));
    return true;
}

bool DownloadWorker::finalizeNative(const QString &source, const QString &destination, const QString &mode) {
    if (!MediaEngine::validMedia(source, mode)) {
        log(QStringLiteral("ERROR: yt-dlp output is unreadable or incomplete; refusing to finalize: %1").arg(source));
        return false;
    }
    if (QFileInfo(source).absoluteFilePath() == QFileInfo(destination).absoluteFilePath())
        return true;
    QFile::remove(destination);
    if (!QFile::rename(source, destination)) {
        log(QStringLiteral("ERROR: Could not finalize yt-dlp output: %1").arg(destination));
        return false;
    }
    log(QStringLiteral("yt-dlp output finalized without a second conversion: %1").arg(destination));
    return true;
}

void DownloadWorker::cleanupThumbnails(const QStringList &paths, bool preserve) {
    QStringList removed;
    QStringList kept;
    QSet<QString> seen;
    for (const QString &candidate : paths) {
        const QString path = QFileInfo(candidate).absoluteFilePath();
        if (seen.contains(path) || !QFileInfo(path).isFile()) continue;
        seen.insert(path);
        if (preserve) {
            kept << path;
            continue;
        }
        if (QFile::remove(path)) removed << path;
        else log(QStringLiteral("Could not remove temporary thumbnail: %1").arg(path));
    }
    if (!kept.isEmpty())
        log(QStringLiteral("Kept thumbnail sidecar(s) because the output format may not have accepted embedded picture metadata: %1")
                .arg(kept.join(QStringLiteral(", "))));
    if (!removed.isEmpty())
        log(QStringLiteral("Removed temporary thumbnail sidecar(s): %1").arg(removed.join(QStringLiteral(", "))));
}

void DownloadWorker::cleanupSuccess(const QString &source, const QString &destination,
                                    const QString &thumbnail) {
    const QString finalPath = QFileInfo(destination).absoluteFilePath();
    const QString directory = QFileInfo(finalPath).absolutePath();
    const QString stem = QFileInfo(finalPath).completeBaseName().toLower();
    QStringList removed;
    const QString sourcePath = QFileInfo(source).absoluteFilePath();
    for (const QFileInfo &entry : QDir(directory).entryInfoList(QDir::Files | QDir::NoDotAndDotDot)) {
        const QString path = entry.absoluteFilePath();
        if (path == finalPath || path == thumbnail) continue;
        const bool sourceFile = path == sourcePath;
        const bool temporaryFile = entry.fileName().toLower().startsWith(stem + QStringLiteral(".temp."));
        if ((sourceFile || temporaryFile) && QFile::remove(path)) removed << path;
    }
    if (!removed.isEmpty())
        log(QStringLiteral("Removed intermediate file(s): %1").arg(removed.join(QStringLiteral(", "))));
}

void DownloadWorker::cleanupCancelled() {
    if (activeOutputFolder_.isEmpty()) return;
    QSet<QString> candidates;
    for (const QString &path : downloadedFiles_) candidates.insert(QFileInfo(path).absoluteFilePath());
    static const QSet<QString> cleanupExtensions{
        QStringLiteral("part"), QStringLiteral("ytdl"), QStringLiteral("webm"), QStringLiteral("m4a"),
        QStringLiteral("mkv"), QStringLiteral("mp4"), QStringLiteral("mka"), QStringLiteral("opus"),
        QStringLiteral("temp"), QStringLiteral("tmp"), QStringLiteral("mp2")
    };
    for (const QFileInfo &entry : QDir(activeOutputFolder_).entryInfoList(QDir::Files | QDir::NoDotAndDotDot)) {
        if (cleanupExtensions.contains(entry.suffix().toLower()) ||
            entry.fileName().contains(QStringLiteral(".temp."), Qt::CaseInsensitive)) {
            candidates.insert(entry.absoluteFilePath());
        }
    }
    QStringList removed;
    for (const QString &path : candidates) {
        const QFileInfo info(path);
        if (!info.isFile()) continue;
        QString key = QDir::cleanPath(info.absoluteFilePath());
#ifdef Q_OS_WIN
        key = key.toCaseFolded();
#endif
        const auto old = activeBeforeSnapshot_.constFind(key);
        if (old != activeBeforeSnapshot_.cend() &&
            old->modifiedMs == info.lastModified().toMSecsSinceEpoch() && old->size == info.size())
            continue;
        if (QFile::remove(path)) removed << path;
        else log(QStringLiteral("Could not remove cancelled output: %1").arg(path));
    }
    if (!removed.isEmpty())
        log(QStringLiteral("Removed cancelled download/intermediate files: %1").arg(removed.join(QStringLiteral(", "))));
}

DownloadWorker::LinkResult DownloadWorker::processUrl(const QString &url, int index, int total) {
    downloadedFiles_.clear();
    preservedThumbnailPaths_.clear();
    log(QStringLiteral("=== Link %1/%2: %3 ===").arg(index).arg(total).arg(url));
    const QString outputFolder = outputFolderForUrl(url);
    if (isCancelled()) return {false, outputFolder};
    activeOutputFolder_ = outputFolder;
    const QString outputTemplate = QDir(outputFolder).filePath(
        options_.mode == QStringLiteral("thumbnail") ? QStringLiteral("%(title)s.%(ext)s")
                                                    : QStringLiteral("%(title)s.temp.%(ext)s"));
    log(QStringLiteral("Output template: %1").arg(outputTemplate));
    activeBeforeSnapshot_ = snapshotFiles(outputFolder);

    QStringList errors;
    QStringList reportedPaths;
    const bool ytdlpComplete = runYtdlp(url, outputTemplate, options_.mode, &errors, &reportedPaths);
    if (isCancelled()) {
        cleanupCancelled();
        return {false, outputFolder};
    }
    if (!ytdlpComplete)
        log(QStringLiteral("yt-dlp reported errors; validating any completed media before marking the link failed."));

    QStringList candidates = reportedPaths;
    candidates += changedFiles(outputFolder, activeBeforeSnapshot_);
    for (QString &path : candidates) path = QFileInfo(path).absoluteFilePath();
    candidates.removeDuplicates();
    QStringList thumbnails;
    for (const QString &path : candidates) {
        const QString ext = QFileInfo(path).suffix().toLower();
        if (ext == QStringLiteral("jpg") || ext == QStringLiteral("jpeg") ||
            ext == QStringLiteral("png") || ext == QStringLiteral("webp"))
            thumbnails << path;
    }
    if (MediaEngine::isYouTubeMusicUrl(url) &&
        (options_.mode == QStringLiteral("audio") || options_.mode == QStringLiteral("thumbnail"))) {
        for (const QString &path : thumbnails)
            MediaEngine::squareYouTubeMusicThumbnail(path, [&](const QString &message) { log(message); });
    }

    if (options_.mode != QStringLiteral("thumbnail")) {
        for (int i = candidates.size() - 1; i >= 0; --i) {
            const QString ext = QFileInfo(candidates.at(i)).suffix().toLower();
            if (ext == QStringLiteral("jpg") || ext == QStringLiteral("jpeg") ||
                ext == QStringLiteral("png") || ext == QStringLiteral("webp"))
                candidates.removeAt(i);
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const QString &a, const QString &b) {
        return QFileInfo(a).lastModified() < QFileInfo(b).lastModified();
    });

    const bool nativeAudio = ytdlpAudioNative();
    const bool nativeVideo = ytdlpVideoNative();
    if (nativeAudio || nativeVideo) {
        const QString suffix = QStringLiteral(".") + options_.container.toLower();
        for (int i = candidates.size() - 1; i >= 0; --i)
            if (QStringLiteral(".") + QFileInfo(candidates.at(i)).suffix().toLower() != suffix)
                candidates.removeAt(i);
    }
    if (candidates.isEmpty()) {
        if (options_.mode != QStringLiteral("thumbnail")) cleanupThumbnails(thumbnails, false);
        log(QStringLiteral("ERROR: Link %1/%2: no new intermediate output file was found.").arg(index).arg(total));
        return {false, outputFolder};
    }

    QHash<QString, QString> thumbnailByStem;
    for (const QString &path : thumbnails) {
        const QString stem = QFileInfo(path).completeBaseName().toCaseFolded();
        if (!thumbnailByStem.contains(stem)) thumbnailByStem.insert(stem, path);
    }
    int successes = 0;
    QStringList successSources, successOutputs, successCovers;
    for (const QString &source : candidates) {
        if (isCancelled()) {
            cleanupCancelled();
            return {false, outputFolder};
        }
        const QString destination = MediaEngine::buildFinalPath(source, options_.container);
        const QString stem = QFileInfo(source).completeBaseName().toCaseFolded();
        const QString thumb = thumbnailByStem.value(stem);
        bool converted = false;
        if (nativeAudio) {
            log(QStringLiteral("Finalizing yt-dlp audio: %1 -> %2").arg(source, destination));
            converted = finalizeNative(source, destination, QStringLiteral("audio"));
        } else if (nativeVideo) {
            log(QStringLiteral("Finalizing yt-dlp video: %1 -> %2").arg(source, destination));
            if (options_.settings.embedMetadata && !thumb.isEmpty()) {
                DownloadOptions remux = options_;
                remux.mode = QStringLiteral("video");
                remux.videoCodec = QStringLiteral("copy");
                remux.audioCodec = QStringLiteral("copy");
                converted = MediaEngine::convert(remux, source, destination, thumb,
                    [&](const QString &message) { log(message); }, &cancelled_);
            } else {
                converted = finalizeNative(source, destination, QStringLiteral("video"));
            }
        } else {
            log(QStringLiteral("Converting: %1 -> %2").arg(source, destination));
            converted = MediaEngine::convert(options_, source, destination, thumb,
                [&](const QString &message) { log(message); }, &cancelled_);
        }
        if (converted) {
            ++successes;
            successSources << source;
            successOutputs << destination;
            successCovers << thumb;
        } else {
            log(QStringLiteral("ERROR: Conversion/finalization failed for %1").arg(source));
        }
    }

    if (options_.mode != QStringLiteral("thumbnail")) {
        bool preserve = false;
        if (options_.mode == QStringLiteral("audio") &&
            options_.container.toLower() == QStringLiteral("opus") &&
            options_.settings.embedMetadata) {
            // Retain source artwork beside Opus outputs if verification does not
            // prove the container accepted METADATA_BLOCK_PICTURE.
            for (const QString &output : successOutputs) {
                const ToolPaths tools = ToolManager::locate();
                if (tools.ffprobe.isEmpty()) { preserve = true; break; }
                const ProcessResult probe = ProcessRunner::run(tools.ffprobe,
                    {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_entries"),
                     QStringLiteral("format_tags=METADATA_BLOCK_PICTURE"), QStringLiteral("-of"),
                     QStringLiteral("json"), output}, {}, QProcessEnvironment::systemEnvironment(), 12000);
                if (!probe.output.contains(QStringLiteral("METADATA_BLOCK_PICTURE"))) { preserve = true; break; }
            }
        }
        cleanupThumbnails(thumbnails, preserve);
    }
    if (successes == 0) {
        log(QStringLiteral("ERROR: Link %1/%2: no output files were created successfully.").arg(index).arg(total));
        return {false, outputFolder};
    }
    for (int i = 0; i < successOutputs.size(); ++i) {
        if (QFileInfo(successOutputs.at(i)).isFile())
            cleanupSuccess(successSources.at(i), successOutputs.at(i), successCovers.value(i));
    }
    log(QStringLiteral("Link %1/%2: created %3 output file(s).").arg(index).arg(total).arg(successes));
    if (!ytdlpComplete) {
        log(QStringLiteral("ERROR: Link %1/%2 was incomplete; retained %3 completed output(s). Retry unavailable items.")
                .arg(index).arg(total).arg(successes));
        return {false, outputFolder};
    }
    return {true, outputFolder};
}

void DownloadWorker::run() {
    int successes = 0;
    int failed = 0;
    QString lastFolder = options_.outputDirectory;
    logCapabilities();
    const int total = options_.urls.size();
    for (int index = 0; index < total; ++index) {
        if (isCancelled()) break;
        const LinkResult result = processUrl(options_.urls.at(index), index + 1, total);
        lastFolder = result.outputFolder;
        if (result.success) ++successes;
        else ++failed;
    }
    if (isCancelled()) {
        cleanupCancelled();
        log(QStringLiteral("Download cancelled."));
    } else {
        log(QStringLiteral("Batch complete: %1 succeeded, %2 failed, %3 total.")
                .arg(successes).arg(failed).arg(total));
    }
    emit batchFinished(isCancelled(), successes, failed, lastFolder);
}

ToolWorker::ToolWorker(Task task, QObject *parent) : QThread(parent), task_(task) {}
void ToolWorker::cancel() { cancelled_.store(true, std::memory_order_relaxed); }

void ToolWorker::run() {
    const ToolManager::Log logger = [&](const QString &message) { emit logMessage(message); };
    if (task_ == Task::Prepare) {
        const bool success = ToolManager::prepare(logger, &cancelled_);
        emit taskFinished(success, false, success
            ? QStringList{QStringLiteral("All required tools are ready.")}
            : QStringList{QStringLiteral("Tool preparation failed; see the log for details.")});
    } else {
        const UpdateSummary summary = ToolManager::updateToolsAndApplication(logger, &cancelled_);
        emit taskFinished(summary.success, summary.changed, summary.messages);
    }
}
