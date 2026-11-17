#include "mediaengine.h"
#include "toolmanager.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>
#include <algorithm>

static QByteArray bigEndian32(quint32 value) {
    QByteArray bytes(4, char(0));
    bytes[0] = char((value >> 24) & 0xff);
    bytes[1] = char((value >> 16) & 0xff);
    bytes[2] = char((value >> 8) & 0xff);
    bytes[3] = char(value & 0xff);
    return bytes;
}

bool MediaEngine::makeOpusPictureMetadata(const QString &source, const QString &cover,
                                          const QString &ffmetaPath, const FFmpegFeatures &features,
                                          const Log &log) {
    if (cover.isEmpty() || !QFileInfo(cover).isFile() || features.ffprobe.isEmpty())
        return false;
    const QJsonObject metadata = probeMedia(source, features)
                                     .value(QStringLiteral("format")).toObject()
                                     .value(QStringLiteral("tags")).toObject();
    QImageReader reader(cover);
    reader.setAutoTransform(true);
    QImage image = reader.read();
    if (image.isNull()) {
        log(QStringLiteral("Could not read Opus cover image: %1").arg(cover));
        return false;
    }
    QByteArray imageBytes;
    QBuffer buffer(&imageBytes);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "JPEG", 92))
        return false;
    const QString mime = QStringLiteral("image/jpeg");
    const QByteArray mimeBytes = mime.toUtf8();
    const QByteArray description = QByteArrayLiteral("Cover (front)");
    QByteArray picture;
    picture += bigEndian32(3);
    picture += bigEndian32(quint32(mimeBytes.size()));
    picture += mimeBytes;
    picture += bigEndian32(quint32(description.size()));
    picture += description;
    picture += bigEndian32(quint32(image.width()));
    picture += bigEndian32(quint32(image.height()));
    picture += bigEndian32(24);
    picture += bigEndian32(0);
    picture += bigEndian32(quint32(imageBytes.size()));
    picture += imageBytes;

    QByteArray ffmeta = QByteArrayLiteral(";FFMETADATA1\n");
    for (auto it = metadata.begin(); it != metadata.end(); ++it) {
        if (it.key().compare(QStringLiteral("METADATA_BLOCK_PICTURE"), Qt::CaseInsensitive) == 0)
            continue;
        if (it.value().isString())
            ffmeta += it.key().toUtf8() + '=' + ffmetadataEscape(it.value().toString()).toUtf8() + '\n';
    }
    ffmeta += QByteArrayLiteral("METADATA_BLOCK_PICTURE=") + picture.toBase64() + '\n';
    QSaveFile file(ffmetaPath);
    if (!file.open(QIODevice::WriteOnly) || file.write(ffmeta) != ffmeta.size() || !file.commit()) {
        log(QStringLiteral("Could not write the temporary FFmpeg Opus picture metadata file."));
        return false;
    }
    return true;
}

QStringList MediaEngine::audioArguments(const QString &codec) {
    if (codec == QStringLiteral("copy") || codec.isEmpty())
        return {QStringLiteral("-c:a"), QStringLiteral("copy")};
    QString encoder;
    if (codec == QStringLiteral("aac")) encoder = QStringLiteral("aac");
    else if (codec == QStringLiteral("opus")) encoder = QStringLiteral("libopus");
    else if (codec == QStringLiteral("flac")) encoder = QStringLiteral("flac");
    else if (codec == QStringLiteral("lpcm")) encoder = QStringLiteral("pcm_s16le");
    else if (codec == QStringLiteral("mpeg-1")) encoder = QStringLiteral("libmp3lame");
    else if (codec == QStringLiteral("mpeg-2")) encoder = QStringLiteral("mp2");
    else return {QStringLiteral("-c:a"), QStringLiteral("copy")};

    QStringList args{QStringLiteral("-c:a"), encoder};
    if (codec == QStringLiteral("aac")) args << QStringLiteral("-b:a") << QStringLiteral("192k");
    else if (codec == QStringLiteral("opus")) args << QStringLiteral("-b:a") << QStringLiteral("160k");
    else if (codec == QStringLiteral("mpeg-1")) args << QStringLiteral("-q:a") << QStringLiteral("2");
    else if (codec == QStringLiteral("mpeg-2")) args << QStringLiteral("-b:a") << QStringLiteral("192k");
    return args;
}

bool MediaEngine::compatibleAudioCopy(const QString &codec, const QString &extension) {
    if (extension == QStringLiteral(".mkv")) return true;
    if (extension == QStringLiteral(".webm")) return codec == QStringLiteral("opus") || codec == QStringLiteral("vorbis");
    if (extension == QStringLiteral(".mp4") || extension == QStringLiteral(".mov") || extension == QStringLiteral(".m4a"))
        return QStringList{QStringLiteral("aac"), QStringLiteral("alac"), QStringLiteral("mp3"),
                QStringLiteral("mp2"), QStringLiteral("ac3"), QStringLiteral("eac3"),
                QStringLiteral("flac")}.contains(codec);
    if (extension == QStringLiteral(".mp3")) return codec == QStringLiteral("mp3");
    if (extension == QStringLiteral(".wav")) return codec.startsWith(QStringLiteral("pcm_"));
    if (extension == QStringLiteral(".flac")) return codec == QStringLiteral("flac");
    if (extension == QStringLiteral(".opus")) return codec == QStringLiteral("opus");
    return false;
}

QString MediaEngine::fallbackAudioCodec(const QString &extension) {
    if (extension == QStringLiteral(".mp4") || extension == QStringLiteral(".mov") || extension == QStringLiteral(".m4a"))
        return QStringLiteral("aac");
    if (extension == QStringLiteral(".mp3")) return QStringLiteral("mpeg-1");
    if (extension == QStringLiteral(".wav")) return QStringLiteral("lpcm");
    if (extension == QStringLiteral(".flac")) return QStringLiteral("flac");
    if (extension == QStringLiteral(".opus") || extension == QStringLiteral(".webm")) return QStringLiteral("opus");
    return {};
}

QStringList MediaEngine::resolvedAudioArguments(const QString &source, const QString &destination,
                                                 const QString &codec, const FFmpegFeatures &features,
                                                 const Log &log) {
    if (!codec.isEmpty() && codec != QStringLiteral("copy"))
        return audioArguments(codec);
    if (!features.ffprobe.isEmpty()) {
        const ProcessResult probe = ProcessRunner::run(features.ffprobe,
            {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-select_streams"), QStringLiteral("a:0"),
             QStringLiteral("-show_entries"), QStringLiteral("stream=codec_name"), QStringLiteral("-of"),
             QStringLiteral("json"), source}, {}, QProcessEnvironment::systemEnvironment(), 12000);
        const QJsonArray streams = QJsonDocument::fromJson(probe.output.toUtf8())
                                       .object().value(QStringLiteral("streams")).toArray();
        const QString sourceCodec = streams.isEmpty() ? QString()
            : streams.first().toObject().value(QStringLiteral("codec_name")).toString().toLower();
        const QString extension = QFileInfo(destination).suffix().isEmpty()
            ? QString() : QStringLiteral(".") + QFileInfo(destination).suffix().toLower();
        if (!sourceCodec.isEmpty() && compatibleAudioCopy(sourceCodec, extension)) {
            log(QStringLiteral("Audio stream copy is compatible: %1 -> %2").arg(sourceCodec, extension));
            return {QStringLiteral("-c:a"), QStringLiteral("copy")};
        }
        const QString fallback = fallbackAudioCodec(extension);
        if (!fallback.isEmpty()) {
            log(QStringLiteral("Audio stream copy is not compatible (%1 -> %2); transcoding as %3.")
                    .arg(sourceCodec.isEmpty() ? QStringLiteral("unknown codec") : sourceCodec, extension, fallback));
            return audioArguments(fallback);
        }
    }
    return {QStringLiteral("-c:a"), QStringLiteral("copy")};
}

QStringList MediaEngine::muxerArguments(const QString &destination, const QString &videoCodec,
                                        bool hasCover) {
    const QString extension = QStringLiteral(".") + QFileInfo(destination).suffix().toLower();
    QStringList args;
    if (extension == QStringLiteral(".mp4") || extension == QStringLiteral(".mov"))
        args << QStringLiteral("-movflags") << QStringLiteral("+faststart+use_metadata_tags");
    else if (extension == QStringLiteral(".m4a") && !hasCover)
        args << QStringLiteral("-movflags") << QStringLiteral("+use_metadata_tags");
    if (videoCodec == QStringLiteral("h265") &&
        (extension == QStringLiteral(".mp4") || extension == QStringLiteral(".mov")))
        args << QStringLiteral("-tag:v:0") << QStringLiteral("hvc1");
    return args;
}

bool MediaEngine::runFFmpegAttempt(const QString &label, QStringList command,
                                   const QString &destination, const QString &expectedMode,
                                   const FFmpegFeatures &features, const Log &log,
                                   const std::atomic_bool *cancel) {
    if (command.isEmpty() || command.last() != destination) {
        log(QStringLiteral("%1 refused: output argument was not the requested destination.").arg(label));
        return false;
    }
    const QFileInfo dstInfo(destination);
    const QString tempName = dstInfo.completeBaseName() + QStringLiteral(".temp.") +
                             QUuid::createUuid().toString(QUuid::WithoutBraces) +
                             (dstInfo.suffix().isEmpty() ? QString() : QStringLiteral(".") + dstInfo.suffix());
    const QString temporary = QDir(dstInfo.absolutePath()).filePath(tempName);
    command.last() = temporary;
    log(QStringLiteral("%1: %2").arg(label,
        ProcessRunner::quoteCommand(features.ffmpeg, command)));
    const ProcessResult process = ProcessRunner::run(features.ffmpeg, command, {},
        QProcessEnvironment::systemEnvironment(), 0, cancel,
        [&](const QString &line) { if (!line.trimmed().isEmpty()) log(QStringLiteral("[ffmpeg] ") + line); });
    if (process.cancelled || (cancel && cancel->load(std::memory_order_relaxed))) {
        QFile::remove(temporary);
        return false;
    }
    if (process.exitCode == 0 && QFileInfo(temporary).isFile() && QFileInfo(temporary).size() > 0) {
        if (!expectedMode.isEmpty() && !validMedia(temporary, expectedMode)) {
            log(QStringLiteral("%1 produced an unreadable or incomplete %2 file.").arg(label, expectedMode));
        } else {
            const QString backup = destination + QStringLiteral(".downloader-backup");
            QFile::remove(backup);
            const bool hadDestination = QFileInfo::exists(destination);
            if (hadDestination && !QFile::rename(destination, backup)) {
                log(QStringLiteral("%1 could not stage the previous output file for replacement.").arg(label));
            } else if (QFile::rename(temporary, destination)) {
                QFile::remove(backup);
                log(QStringLiteral("%1 succeeded.").arg(label));
                return true;
            } else {
                if (hadDestination) QFile::rename(backup, destination);
                log(QStringLiteral("%1 could not replace the final file; previous output was restored.").arg(label));
            }
        }
    } else {
        log(QStringLiteral("%1 failed with code %2.").arg(label).arg(process.exitCode));
    }
    QFile::remove(temporary);
    if (!process.output.trimmed().isEmpty())
        log(QStringLiteral("[%1] %2").arg(label, process.output.right(1000)));
    return false;
}

bool MediaEngine::convert(const DownloadOptions &options, const QString &source,
                          const QString &destination, const QString &thumbnail,
                          const Log &log, const std::atomic_bool *cancel) {
    const FFmpegFeatures features = detectFeatures();
    if (features.ffmpeg.isEmpty()) {
        log(QStringLiteral("ERROR: FFmpeg was not found."));
        return false;
    }
    if (options.mode == QStringLiteral("video"))
        return convertVideo(options, source, destination, thumbnail, features, log, cancel);
    if (options.mode == QStringLiteral("audio"))
        return convertAudio(options, source, destination, thumbnail, features, log, cancel);
    if (options.mode == QStringLiteral("thumbnail"))
        return convertThumbnail(source, destination, features, log, cancel);
    log(QStringLiteral("ERROR: Unknown conversion mode: %1").arg(options.mode));
    return false;
}

bool MediaEngine::convertThumbnail(const QString &source, const QString &destination,
                                   const FFmpegFeatures &features, const Log &log,
                                   const std::atomic_bool *cancel) {
    if (QFileInfo(source).absoluteFilePath() == QFileInfo(destination).absoluteFilePath())
        return QFileInfo(source).isFile() && QFileInfo(source).size() > 0 && validMedia(source, QStringLiteral("thumbnail"));
    return runFFmpegAttempt(QStringLiteral("Thumbnail conversion"),
        {QStringLiteral("-y"), QStringLiteral("-nostdin"), QStringLiteral("-i"), source,
         QStringLiteral("-frames:v"), QStringLiteral("1"), destination},
        destination, QStringLiteral("thumbnail"), features, log, cancel);
}

bool MediaEngine::convertAudio(const DownloadOptions &options, const QString &source,
                               const QString &destination, const QString &thumbnail,
                               const FFmpegFeatures &features, const Log &log,
                               const std::atomic_bool *cancel) {
    const QString extension = QStringLiteral(".") + QFileInfo(destination).suffix().toLower();
    const bool embedCover = options.settings.embedMetadata &&
        (extension == QStringLiteral(".mp3") || extension == QStringLiteral(".m4a") ||
         extension == QStringLiteral(".flac") || extension == QStringLiteral(".opus"));
    const bool opusTarget = embedCover && extension == QStringLiteral(".opus");
    const bool externalCover = embedCover && !thumbnail.isEmpty() && QFileInfo(thumbnail).isFile();
    const int sourceCoverIndex = embedCover && !externalCover ? attachedPictureStream(source) : -1;

    QStringList command{QStringLiteral("-y"), QStringLiteral("-nostdin"), QStringLiteral("-i"), source};
    QString metadataFile;
    if (opusTarget && externalCover) {
        metadataFile = QDir(QDir::tempPath()).filePath(QStringLiteral("opus-cover-") +
            QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".ffmeta"));
        if (!makeOpusPictureMetadata(source, thumbnail, metadataFile, features, log)) {
            QFile::remove(metadataFile);
            log(QStringLiteral("WARNING: Could not prepare FFmpeg Opus picture metadata; output will be created without new cover art."));
        } else {
            command << QStringLiteral("-f") << QStringLiteral("ffmetadata") << QStringLiteral("-i") << metadataFile;
            log(QStringLiteral("Ogg Opus cover will be written as METADATA_BLOCK_PICTURE through FFmpeg."));
        }
    }
    if (externalCover && !opusTarget) command << QStringLiteral("-i") << thumbnail;

    command << QStringLiteral("-map") << QStringLiteral("0:a:0?") << QStringLiteral("-vn");
    if (externalCover && !opusTarget) {
        command << QStringLiteral("-map") << QStringLiteral("1:v:0?")
                << QStringLiteral("-c:v:0") << QStringLiteral("mjpeg")
                << QStringLiteral("-disposition:v:0") << QStringLiteral("attached_pic")
                << QStringLiteral("-metadata:s:v:0") << QStringLiteral("title=Album cover")
                << QStringLiteral("-metadata:s:v:0") << QStringLiteral("comment=Cover (front)");
    } else if (sourceCoverIndex >= 0) {
        command << QStringLiteral("-map") << QStringLiteral("0:%1").arg(sourceCoverIndex)
                << QStringLiteral("-c:v:0") << QStringLiteral("mjpeg")
                << QStringLiteral("-disposition:v:0") << QStringLiteral("attached_pic")
                << QStringLiteral("-metadata:s:v:0") << QStringLiteral("title=Album cover")
                << QStringLiteral("-metadata:s:v:0") << QStringLiteral("comment=Cover (front)");
    } else if (embedCover && !externalCover) {
        log(QStringLiteral("No usable cover image was found for %1; converting without new artwork.")
                .arg(QFileInfo(source).fileName()));
    }

    if (!metadataFile.isEmpty() && QFileInfo(metadataFile).isFile()) {
        command << QStringLiteral("-map_metadata") << QStringLiteral("1");
    } else {
        command << QStringLiteral("-map_metadata") << QStringLiteral("0");
    }
    command << QStringLiteral("-map_metadata:s:a") << QStringLiteral("0:s:a");
    if (options.settings.embedMetadata)
        command << QStringLiteral("-map_chapters") << QStringLiteral("0");
    command += resolvedAudioArguments(source, destination, options.audioCodec, features, log);
    command += muxerArguments(destination, {}, externalCover || sourceCoverIndex >= 0);
    command << destination;

    const bool converted = runFFmpegAttempt(QStringLiteral("Audio conversion"), command,
        destination, QStringLiteral("audio"), features, log, cancel);
    if (!metadataFile.isEmpty()) QFile::remove(metadataFile);
    return converted;
}

bool MediaEngine::convertVideo(const DownloadOptions &options, const QString &source,
                               const QString &destination, const QString &thumbnail,
                               const FFmpegFeatures &features, const Log &log,
                               const std::atomic_bool *cancel) {
    const QString extension = QStringLiteral(".") + QFileInfo(destination).suffix().toLower();
    const QString backend = options.settings.encoderBackend.toUpper();
    const QString audioCodec = options.audioCodec.isEmpty() ? QStringLiteral("copy") : options.audioCodec;
    const QString videoCodec = options.videoCodec.isEmpty() ? QStringLiteral("copy") : options.videoCodec;
    const QStringList audioArgs = resolvedAudioArguments(source, destination, audioCodec, features, log);
    const bool externalCover = !thumbnail.isEmpty() && QFileInfo(thumbnail).isFile() &&
        (extension == QStringLiteral(".mp4") || extension == QStringLiteral(".mov") || extension == QStringLiteral(".mkv"));

    const QJsonObject videoInfo = probeVideo(source);
    if (!videoInfo.isEmpty())
        log(QStringLiteral("Input video: codec=%1, pixel_format=%2, size=%3x%4")
            .arg(videoInfo.value(QStringLiteral("codec_name")).toString(QStringLiteral("unknown")),
                 videoInfo.value(QStringLiteral("pix_fmt")).toString(QStringLiteral("unknown")),
                 videoInfo.value(QStringLiteral("width")).toVariant().toString(),
                 videoInfo.value(QStringLiteral("height")).toVariant().toString()));

    auto inputCommand = [&](const QStringList &prefix) {
        QStringList cmd{QStringLiteral("-y"), QStringLiteral("-nostdin")};
        cmd += prefix;
        cmd << QStringLiteral("-i") << source;
        if (externalCover && (extension == QStringLiteral(".mp4") || extension == QStringLiteral(".mov")))
            cmd << QStringLiteral("-i") << thumbnail;
        return cmd;
    };
    auto outputArguments = [&](const QStringList &videoArgs, const QString &filter) {
        QStringList args{
            QStringLiteral("-map"), QStringLiteral("0:v:0?"),
            QStringLiteral("-map"), QStringLiteral("0:a:0?"),
            QStringLiteral("-map_metadata"), QStringLiteral("0"),
            QStringLiteral("-map_metadata:s:v"), QStringLiteral("0:s:v"),
            QStringLiteral("-map_metadata:s:a"), QStringLiteral("0:s:a")
        };
        if (options.settings.embedMetadata)
            args << QStringLiteral("-map_chapters") << QStringLiteral("0");
        if (extension == QStringLiteral(".mkv"))
            args << QStringLiteral("-map") << QStringLiteral("0:t?");
        args << QStringLiteral("-sn");
        if (!filter.isEmpty()) args << QStringLiteral("-filter:v:0") << filter;
        args += videoArgs;
        args += audioArgs;
        args += muxerArguments(destination, videoCodec, externalCover);
        if (externalCover) {
            if (extension == QStringLiteral(".mp4") || extension == QStringLiteral(".mov")) {
                args << QStringLiteral("-map") << QStringLiteral("1:v:0?")
                     << QStringLiteral("-c:v:1") << QStringLiteral("mjpeg")
                     << QStringLiteral("-disposition:v:1") << QStringLiteral("attached_pic");
            } else if (extension == QStringLiteral(".mkv")) {
                args << QStringLiteral("-attach") << thumbnail
                     << QStringLiteral("-metadata:s:t:0") << QStringLiteral("mimetype=image/jpeg")
                     << QStringLiteral("-metadata:s:t:0") << QStringLiteral("filename=cover.jpg");
            }
        }
        args << destination;
        return args;
    };

    if (videoCodec == QStringLiteral("copy")) {
        return runFFmpegAttempt(QStringLiteral("Stream-copy/remux"),
            inputCommand({}) + outputArguments({QStringLiteral("-c:v:0"), QStringLiteral("copy")}, {}),
            destination, QStringLiteral("video"), features, log, cancel);
    }

    auto cpuVideoArgs = [&]() {
        if (videoCodec == QStringLiteral("h264")) return QStringList{QStringLiteral("-c:v:0"), QStringLiteral("libx264")};
        if (videoCodec == QStringLiteral("h265")) return QStringList{QStringLiteral("-c:v:0"), QStringLiteral("libx265")};
        if (videoCodec == QStringLiteral("vp9")) return QStringList{QStringLiteral("-c:v:0"), QStringLiteral("libvpx-vp9")};
        if (videoCodec == QStringLiteral("av1")) return QStringList{QStringLiteral("-c:v:0"), QStringLiteral("libaom-av1")};
        if (videoCodec == QStringLiteral("prores_422")) return QStringList{QStringLiteral("-c:v:0"), QStringLiteral("prores_ks"), QStringLiteral("-profile:v:0"), QStringLiteral("3")};
        if (videoCodec == QStringLiteral("dnxhr_sq")) return QStringList{QStringLiteral("-c:v:0"), QStringLiteral("dnxhd"), QStringLiteral("-profile:v:0"), QStringLiteral("dnxhr_sq")};
        if (videoCodec == QStringLiteral("dnxhr_hq")) return QStringList{QStringLiteral("-c:v:0"), QStringLiteral("dnxhd"), QStringLiteral("-profile:v:0"), QStringLiteral("dnxhr_hq")};
        return QStringList{};
    };
    const QStringList cpuArgs = cpuVideoArgs();
    if (cpuArgs.isEmpty()) {
        log(QStringLiteral("ERROR: No FFmpeg encoder mapping exists for %1.").arg(videoCodec));
        return false;
    }
    auto cpuFallback = [&](const QString &reason) {
        log(reason);
        return runFFmpegAttempt(QStringLiteral("CPU decode + CPU encode"),
            inputCommand({}) + outputArguments(cpuArgs, {}),
            destination, QStringLiteral("video"), features, log, cancel);
    };
    if (backend == QStringLiteral("CPU"))
        return cpuFallback(QStringLiteral("Forced CPU encoder for %1.").arg(videoCodec));

    QStringList candidates = gpuEncoders(videoCodec, backend);
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
        [&](const QString &encoder) { return !features.encoders.contains(encoder); }), candidates.end());
    if (candidates.isEmpty()) {
        log(QStringLiteral("Selected %1 backend has no available encoder for %2; falling back to CPU.")
                .arg(backend, videoCodec));
        return cpuFallback(QStringLiteral("CPU fallback selected."));
    }

    for (const QString &encoder : candidates) {
        QString decodeLabel;
        const QStringList decodeArgs = gpuDecodeArguments(encoder, backend, features, &decodeLabel);
        if (!decodeArgs.isEmpty()) {
            QStringList cmd = inputCommand(decodeArgs);
            cmd += outputArguments(encoderOutputArguments(encoder), {});
            if (runFFmpegAttempt(decodeLabel + QStringLiteral(" + ") + backend + QStringLiteral(" encode"),
                    cmd, destination, QStringLiteral("video"), features, log, cancel))
                return true;
        }
        log(QStringLiteral("Hardware decoding failed/unavailable; retrying CPU decode with %1 encoding.").arg(encoder));
        QStringList filterArgs;
        QStringList prefixArgs;
        if (encoder.endsWith(QStringLiteral("_vaapi"))) {
            const QString node = linuxRenderNode(backend);
            if (!node.isEmpty()) {
                const QString device = backend == QStringLiteral("AMD") ? QStringLiteral("amd_va") : QStringLiteral("intel_va");
                prefixArgs = {QStringLiteral("-init_hw_device"), QStringLiteral("vaapi=%1:%2").arg(device, node),
                              QStringLiteral("-filter_hw_device"), device};
                filterArgs = {QStringLiteral("format=nv12,hwupload")};
            }
        } else {
            filterArgs = {QStringLiteral("format=nv12")};
        }
        if (!filterArgs.isEmpty()) {
            const QString label = encoder.endsWith(QStringLiteral("_vaapi")) ? backend + QStringLiteral(" VAAPI")
                : encoder.endsWith(QStringLiteral("_qsv")) ? QStringLiteral("Intel QSV")
                : encoder.endsWith(QStringLiteral("_amf")) ? QStringLiteral("AMD AMF")
                : QStringLiteral("NVIDIA NVENC");
            QStringList cmd = inputCommand(prefixArgs);
            cmd += outputArguments(encoderOutputArguments(encoder), filterArgs.first());
            if (runFFmpegAttempt(QStringLiteral("CPU decode + ") + label + QStringLiteral(" encode"),
                    cmd, destination, QStringLiteral("video"), features, log, cancel))
                return true;
        }
    }
    return cpuFallback(QStringLiteral("All %1 GPU encoding attempts failed; falling back to CPU.").arg(backend));
}
