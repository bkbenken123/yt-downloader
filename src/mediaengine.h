#pragma once
#include "appconfig.h"
#include "process.h"
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <functional>
#include <atomic>

struct FFmpegFeatures {
    QString ffmpeg;
    QString ffprobe;
    QStringList hwaccels;
    QStringList encoders;
    QStringList decoders;
};

class MediaEngine {
public:
    using Log = std::function<void(const QString &)>;
    static FFmpegFeatures detectFeatures();
    static bool convert(const DownloadOptions &options, const QString &source,
                        const QString &destination, const QString &thumbnail,
                        const Log &log, const std::atomic_bool *cancel = nullptr);
    static bool validMedia(const QString &path, const QString &mode);
    static QJsonObject probeVideo(const QString &path);
    static QJsonObject probeAudio(const QString &path);
    static int attachedPictureStream(const QString &path);
    static bool squareYouTubeMusicThumbnail(const QString &path, const Log &log);
    static QString buildFinalPath(const QString &source, const QString &extension);
    static bool isYouTubeMusicUrl(const QString &url);
    static QString selectThumbnailUrl(const QString &url, const QString &mode,
                                      bool embedMetadata, QString *reason = nullptr);
private:
    static bool convertVideo(const DownloadOptions &, const QString &, const QString &,
                             const QString &, const FFmpegFeatures &, const Log &,
                             const std::atomic_bool *);
    static bool convertAudio(const DownloadOptions &, const QString &, const QString &,
                             const QString &, const FFmpegFeatures &, const Log &,
                             const std::atomic_bool *);
    static bool convertThumbnail(const QString &, const QString &, const FFmpegFeatures &,
                                 const Log &, const std::atomic_bool *);
    static bool runFFmpegAttempt(const QString &label, QStringList command,
                                 const QString &destination, const QString &expectedMode,
                                 const FFmpegFeatures &, const Log &,
                                 const std::atomic_bool *);
    static QStringList audioArguments(const QString &codec);
    static QStringList resolvedAudioArguments(const QString &source, const QString &destination,
                                              const QString &codec, const FFmpegFeatures &,
                                              const Log &);
    static QStringList muxerArguments(const QString &destination, const QString &videoCodec,
                                      bool hasCover);
    static bool compatibleAudioCopy(const QString &codec, const QString &extension);
    static QString fallbackAudioCodec(const QString &extension);
    static QStringList gpuEncoders(const QString &codec, const QString &backend);
    static QStringList encoderOutputArguments(const QString &encoder);
    static QStringList gpuDecodeArguments(const QString &encoder, const QString &backend,
                                          const FFmpegFeatures &, QString *label);
    static QList<QPair<QString, QStringList>> cpuDecodeGpuEncodeArguments(
        const QString &encoder, const QString &backend);
    static QString linuxRenderNode(const QString &backend);
    static QJsonObject probeMedia(const QString &path, const FFmpegFeatures &);
    static QString ffmetadataEscape(const QString &value);
    static bool makeOpusPictureMetadata(const QString &source, const QString &cover,
                                        const QString &ffmetaPath, const FFmpegFeatures &,
                                        const Log &);
};
