#include "mediaengine.h"
#include "toolmanager.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrl>
#include <algorithm>
#include <cmath>

FFmpegFeatures MediaEngine::detectFeatures() {
    FFmpegFeatures features;
    const ToolPaths paths = ToolManager::locate();
    features.ffmpeg = paths.ffmpeg;
    features.ffprobe = paths.ffprobe;
    if (features.ffmpeg.isEmpty()) return features;

    const ProcessResult hw = ProcessRunner::run(features.ffmpeg,
        {QStringLiteral("-hide_banner"), QStringLiteral("-hwaccels")},
        {}, QProcessEnvironment::systemEnvironment(), 10000);
    for (const QString &line : hw.output.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts)) {
        const QString value = line.trimmed().toLower();
        if (!value.startsWith(QStringLiteral("hardware acceleration")) &&
            QRegularExpression(QStringLiteral("^[a-z0-9_+-]+$")).match(value).hasMatch())
            features.hwaccels << value;
    }

    for (const QString &kind : {QStringLiteral("encoders"), QStringLiteral("decoders")}) {
        const ProcessResult result = ProcessRunner::run(features.ffmpeg,
            {QStringLiteral("-hide_banner"), QStringLiteral("-") + kind},
            {}, QProcessEnvironment::systemEnvironment(), 15000);
        QStringList &target = kind == QStringLiteral("encoders") ? features.encoders : features.decoders;
        const QRegularExpression component(QStringLiteral("^\\s*[A-Z.]{6,8}\\s+([A-Za-z0-9_]+)\\s"), QRegularExpression::CaseInsensitiveOption);
        for (const QString &line : result.output.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts)) {
            const auto match = component.match(line);
            if (match.hasMatch()) target << match.captured(1).toLower();
        }
    }
    features.encoders.removeDuplicates();
    features.decoders.removeDuplicates();
    features.hwaccels.removeDuplicates();
    return features;
}

QJsonObject MediaEngine::probeMedia(const QString &path, const FFmpegFeatures &features) {
    if (features.ffprobe.isEmpty()) return {};
    const ProcessResult result = ProcessRunner::run(features.ffprobe,
        {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_format"),
         QStringLiteral("-show_streams"), QStringLiteral("-show_chapters"),
         QStringLiteral("-of"), QStringLiteral("json"), path},
        {}, QProcessEnvironment::systemEnvironment(), 25000);
    if (!result.started || result.exitCode != 0) return {};
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(result.output.toUtf8(), &error);
    return error.error == QJsonParseError::NoError && document.isObject() ? document.object() : QJsonObject{};
}

QJsonObject MediaEngine::probeVideo(const QString &path) {
    const FFmpegFeatures features = detectFeatures();
    if (features.ffprobe.isEmpty()) return {};
    const ProcessResult result = ProcessRunner::run(features.ffprobe,
        {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-select_streams"), QStringLiteral("v:0"),
         QStringLiteral("-show_entries"), QStringLiteral("stream=codec_name,profile,pix_fmt,width,height"),
         QStringLiteral("-of"), QStringLiteral("json"), path},
        {}, QProcessEnvironment::systemEnvironment(), 15000);
    if (!result.started || result.exitCode != 0) return {};
    const QJsonDocument document = QJsonDocument::fromJson(result.output.toUtf8());
    const QJsonArray streams = document.object().value(QStringLiteral("streams")).toArray();
    return streams.isEmpty() ? QJsonObject{} : streams.first().toObject();
}

QJsonObject MediaEngine::probeAudio(const QString &path) {
    const FFmpegFeatures features = detectFeatures();
    if (features.ffprobe.isEmpty()) return {};
    const ProcessResult result = ProcessRunner::run(features.ffprobe,
        {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-select_streams"), QStringLiteral("a:0"),
         QStringLiteral("-show_entries"), QStringLiteral("stream=codec_name,profile,sample_rate,channels"),
         QStringLiteral("-of"), QStringLiteral("json"), path},
        {}, QProcessEnvironment::systemEnvironment(), 15000);
    if (!result.started || result.exitCode != 0) return {};
    const QJsonDocument document = QJsonDocument::fromJson(result.output.toUtf8());
    const QJsonArray streams = document.object().value(QStringLiteral("streams")).toArray();
    return streams.isEmpty() ? QJsonObject{} : streams.first().toObject();
}

int MediaEngine::attachedPictureStream(const QString &path) {
    const FFmpegFeatures features = detectFeatures();
    if (features.ffprobe.isEmpty()) return -1;
    const ProcessResult result = ProcessRunner::run(features.ffprobe,
        {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-select_streams"), QStringLiteral("v"),
         QStringLiteral("-show_entries"), QStringLiteral("stream=index:stream_disposition=attached_pic"),
         QStringLiteral("-of"), QStringLiteral("json"), path},
        {}, QProcessEnvironment::systemEnvironment(), 15000);
    if (!result.started || result.exitCode != 0) return -1;
    const QJsonArray streams = QJsonDocument::fromJson(result.output.toUtf8())
                                   .object().value(QStringLiteral("streams")).toArray();
    for (const QJsonValue &value : streams) {
        const QJsonObject stream = value.toObject();
        if (stream.value(QStringLiteral("disposition")).toObject()
                .value(QStringLiteral("attached_pic")).toInt() == 1)
            return stream.value(QStringLiteral("index")).toInt(-1);
    }
    return -1;
}

bool MediaEngine::validMedia(const QString &path, const QString &mode) {
    const FFmpegFeatures features = detectFeatures();
    const QJsonObject info = probeMedia(path, features);
    const QJsonArray streams = info.value(QStringLiteral("streams")).toArray();
    if (streams.isEmpty()) return false;
    if (mode == QStringLiteral("audio") || mode == QStringLiteral("video")) {
        bool durationOk = false;
        const double duration = info.value(QStringLiteral("format")).toObject()
                                    .value(QStringLiteral("duration")).toString().toDouble(&durationOk);
        if (durationOk && duration <= 0.0) return false;
    }
    bool hasAudio = false, hasVideo = false;
    for (const QJsonValue &value : streams) {
        const QJsonObject stream = value.toObject();
        if (stream.value(QStringLiteral("codec_type")).toString() == QStringLiteral("audio"))
            hasAudio = true;
        if (stream.value(QStringLiteral("codec_type")).toString() == QStringLiteral("video")) {
            const int attached = stream.value(QStringLiteral("disposition")).toObject()
                                     .value(QStringLiteral("attached_pic")).toInt();
            if (attached == 0) hasVideo = true;
            if (mode == QStringLiteral("thumbnail")) hasVideo = true;
        }
    }
    if (mode == QStringLiteral("audio")) return hasAudio;
    if (mode == QStringLiteral("video")) return hasVideo;
    if (mode == QStringLiteral("thumbnail")) return hasVideo;
    return false;
}

QString MediaEngine::buildFinalPath(const QString &source, const QString &extension) {
    QFileInfo info(source);
    QString stem = info.completeBaseName();
    if (stem.endsWith(QStringLiteral(".temp"), Qt::CaseInsensitive)) stem.chop(5);
    return QDir(info.absolutePath()).filePath(stem + QStringLiteral(".") + extension.toLower().remove(QLatin1Char('.')));
}

bool MediaEngine::isYouTubeMusicUrl(const QString &urlText) {
    const QUrl url(urlText);
    return url.host().compare(QStringLiteral("music.youtube.com"), Qt::CaseInsensitive) == 0;
}

QString MediaEngine::selectThumbnailUrl(const QString &urlText, const QString &mode,
                                        bool embedMetadata, QString *reason) {
    QUrl url(urlText);
    if (url.host().compare(QStringLiteral("music.youtube.com"), Qt::CaseInsensitive) == 0) {
        if (mode == QStringLiteral("video") && embedMetadata) {
            url.setHost(QStringLiteral("www.youtube.com"));
            if (reason) *reason = QStringLiteral("standard YouTube video thumbnail");
            return url.toString();
        }
        if (reason) *reason = QStringLiteral("YouTube Music cover artwork");
        return urlText;
    }
    if (reason) *reason = QStringLiteral("standard YouTube video thumbnail");
    return urlText;
}

bool MediaEngine::squareYouTubeMusicThumbnail(const QString &path, const Log &log) {
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QImage original = reader.read();
    if (original.isNull()) {
        log(QStringLiteral("Could not read thumbnail for square-art detection; keeping original: %1").arg(path));
        return true;
    }
    const int width = original.width(), height = original.height();
    if (width == height) {
        log(QStringLiteral("YouTube Music image is already square; preserving: %1").arg(path));
        return true;
    }
    const double ratio = double(width) / double(height);
    if (width <= height || ratio < 1.70 || ratio > 1.85) {
        log(QStringLiteral("YouTube Music image is not 16:9; preserving its dimensions: %1").arg(path));
        return true;
    }

    const QImage sample = original.scaled(96, 54, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB888);
    const int sideWidth = (96 - 54) / 2;
    struct Stats { double mean[3]{}; double deviation = 0; double maxDeviation = 0; };
    auto stats = [&](int x0, int x1) {
        Stats s;
        const int count = (x1 - x0) * 54;
        for (int y = 0; y < 54; ++y) {
            for (int x = x0; x < x1; ++x) {
                const QColor color = sample.pixelColor(x, y);
                s.mean[0] += color.red(); s.mean[1] += color.green(); s.mean[2] += color.blue();
            }
        }
        for (double &v : s.mean) v /= count;
        double total = 0;
        for (int y = 0; y < 54; ++y) {
            for (int x = x0; x < x1; ++x) {
                const QColor color = sample.pixelColor(x, y);
                const int channels[3] = {color.red(), color.green(), color.blue()};
                for (int c = 0; c < 3; ++c) {
                    const double delta = std::abs(channels[c] - s.mean[c]);
                    total += delta;
                    s.maxDeviation = qMax(s.maxDeviation, delta);
                }
            }
        }
        s.deviation = total / (count * 3);
        return s;
    };
    const Stats left = stats(0, sideWidth);
    const Stats right = stats(96 - sideWidth, 96);
    const Stats center = stats(sideWidth, 96 - sideWidth);
    double colorDifference = 0;
    for (int c = 0; c < 3; ++c) colorDifference += std::abs(left.mean[c] - right.mean[c]);
    colorDifference /= 3.0;
    if (colorDifference > 10.0 || qMax(left.deviation, right.deviation) > 6.0 ||
        qMax(left.maxDeviation, right.maxDeviation) > 18.0 ||
        center.deviation < qMax(14.0, qMax(left.deviation, right.deviation) * 2.0 + 6.0)) {
        log(QStringLiteral("No matching flat side borders forming centered square artwork; preserving original thumbnail: %1").arg(path));
        return true;
    }

    const int side = height;
    const QRect crop((width - side) / 2, 0, side, side);
    const QImage square = original.copy(crop);
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly) || !square.save(&output, "JPEG", 92) || !output.commit()) {
        log(QStringLiteral("ERROR: Could not save cropped YouTube Music artwork: %1").arg(path));
        return false;
    }
    log(QStringLiteral("Cropped centered square YouTube Music artwork: %1").arg(path));
    return true;
}

QString MediaEngine::ffmetadataEscape(const QString &value) {
    QString result = value;
    result.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    result.replace(QLatin1Char('='), QStringLiteral("\\="));
    result.replace(QLatin1Char(';'), QStringLiteral("\\;"));
    result.replace(QLatin1Char('#'), QStringLiteral("\\#"));
    result.replace(QLatin1Char('\n'), QStringLiteral("\\\n"));
    return result;
}

QStringList MediaEngine::gpuEncoders(const QString &codec, const QString &backend) {
#ifdef Q_OS_LINUX
    if (backend == QStringLiteral("AMD")) {
        if (codec == QStringLiteral("h264")) return {QStringLiteral("h264_vaapi"), QStringLiteral("h264_amf")};
        if (codec == QStringLiteral("h265")) return {QStringLiteral("hevc_vaapi"), QStringLiteral("hevc_amf")};
        if (codec == QStringLiteral("vp9")) return {QStringLiteral("vp9_vaapi")};
        if (codec == QStringLiteral("av1")) return {QStringLiteral("av1_vaapi"), QStringLiteral("av1_amf")};
    }
    if (backend == QStringLiteral("INTEL")) {
        if (codec == QStringLiteral("h264")) return {QStringLiteral("h264_qsv"), QStringLiteral("h264_vaapi")};
        if (codec == QStringLiteral("h265")) return {QStringLiteral("hevc_qsv"), QStringLiteral("hevc_vaapi")};
        if (codec == QStringLiteral("vp9")) return {QStringLiteral("vp9_qsv"), QStringLiteral("vp9_vaapi")};
        if (codec == QStringLiteral("av1")) return {QStringLiteral("av1_qsv"), QStringLiteral("av1_vaapi")};
    }
#endif
    if (backend == QStringLiteral("AMD")) {
        if (codec == QStringLiteral("h264")) return {QStringLiteral("h264_amf")};
        if (codec == QStringLiteral("h265")) return {QStringLiteral("hevc_amf")};
        if (codec == QStringLiteral("av1")) return {QStringLiteral("av1_amf")};
    } else if (backend == QStringLiteral("INTEL")) {
        if (codec == QStringLiteral("h264")) return {QStringLiteral("h264_qsv")};
        if (codec == QStringLiteral("h265")) return {QStringLiteral("hevc_qsv")};
        if (codec == QStringLiteral("vp9")) return {QStringLiteral("vp9_qsv")};
        if (codec == QStringLiteral("av1")) return {QStringLiteral("av1_qsv")};
    } else if (backend == QStringLiteral("NVIDIA")) {
        if (codec == QStringLiteral("h264")) return {QStringLiteral("h264_nvenc")};
        if (codec == QStringLiteral("h265")) return {QStringLiteral("hevc_nvenc")};
        if (codec == QStringLiteral("av1")) return {QStringLiteral("av1_nvenc")};
    }
    return {};
}

QStringList MediaEngine::encoderOutputArguments(const QString &encoder) {
    QStringList args{QStringLiteral("-c:v:0"), encoder};
    if (encoder.endsWith(QStringLiteral("_amf")))
        args << QStringLiteral("-usage:v:0") << QStringLiteral("transcoding");
    return args;
}

QString MediaEngine::linuxRenderNode(const QString &backend) {
#ifdef Q_OS_LINUX
    const QString vendor = backend == QStringLiteral("AMD") ? QStringLiteral("0x1002")
        : backend == QStringLiteral("INTEL") ? QStringLiteral("0x8086") : QString();
    if (vendor.isEmpty()) return {};
    QDir drm(QStringLiteral("/sys/class/drm"));
    const QStringList entries = drm.entryList({QStringLiteral("renderD*")}, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &entry : entries) {
        QFile f(drm.filePath(entry + QStringLiteral("/device/vendor")));
        if (!f.open(QIODevice::ReadOnly)) continue;
        if (QString::fromLatin1(f.readAll()).trimmed().compare(vendor, Qt::CaseInsensitive) == 0) {
            const QString node = QStringLiteral("/dev/dri/") + entry;
            if (QFileInfo::exists(node)) return node;
        }
    }
#else
    Q_UNUSED(backend);
#endif
    return {};
}

QStringList MediaEngine::gpuDecodeArguments(const QString &encoder, const QString &backend,
                                             const FFmpegFeatures &features, QString *label) {
#ifdef Q_OS_LINUX
    if (encoder.endsWith(QStringLiteral("_vaapi")) && features.hwaccels.contains(QStringLiteral("vaapi"))) {
        const QString node = linuxRenderNode(backend);
        if (!node.isEmpty()) {
            const QString device = backend == QStringLiteral("AMD") ? QStringLiteral("amd_va") : QStringLiteral("intel_va");
            if (label) *label = backend + QStringLiteral(" VAAPI decode");
            return {QStringLiteral("-init_hw_device"), QStringLiteral("vaapi=%1:%2").arg(device, node),
                QStringLiteral("-filter_hw_device"), device, QStringLiteral("-hwaccel"), QStringLiteral("vaapi"),
                QStringLiteral("-hwaccel_device"), device, QStringLiteral("-hwaccel_output_format"), QStringLiteral("vaapi"),
                QStringLiteral("-extra_hw_frames"), QStringLiteral("32")};
        }
    }
#endif
    if (backend == QStringLiteral("AMD")) {
#ifdef Q_OS_WIN
        if (features.hwaccels.contains(QStringLiteral("d3d11va"))) {
            if (label) *label = QStringLiteral("AMD D3D11VA decode");
            return {QStringLiteral("-init_hw_device"), QStringLiteral("d3d11va=amd_d3d11:,vendor_id=0x1002"),
                QStringLiteral("-init_hw_device"), QStringLiteral("amf=amd_amf@amd_d3d11"),
                QStringLiteral("-filter_hw_device"), QStringLiteral("amd_amf"),
                QStringLiteral("-hwaccel"), QStringLiteral("d3d11va"), QStringLiteral("-hwaccel_device"),
                QStringLiteral("amd_d3d11"), QStringLiteral("-hwaccel_output_format"), QStringLiteral("d3d11"),
                QStringLiteral("-extra_hw_frames"), QStringLiteral("32")};
        }
#endif
    } else if (backend == QStringLiteral("INTEL") && encoder.endsWith(QStringLiteral("_qsv")) &&
               features.hwaccels.contains(QStringLiteral("qsv"))) {
        if (label) *label = QStringLiteral("Intel QSV decode");
#ifdef Q_OS_WIN
        return {QStringLiteral("-init_hw_device"), QStringLiteral("d3d11va=intel_d3d11:,vendor_id=0x8086"),
            QStringLiteral("-init_hw_device"), QStringLiteral("qsv=intel_qsv@intel_d3d11"),
            QStringLiteral("-filter_hw_device"), QStringLiteral("intel_qsv"), QStringLiteral("-hwaccel"),
            QStringLiteral("qsv"), QStringLiteral("-hwaccel_device"), QStringLiteral("intel_qsv"),
            QStringLiteral("-hwaccel_output_format"), QStringLiteral("qsv"), QStringLiteral("-extra_hw_frames"),
            QStringLiteral("32")};
#else
        const QString node = linuxRenderNode(QStringLiteral("INTEL"));
        if (!node.isEmpty())
            return {QStringLiteral("-init_hw_device"), QStringLiteral("vaapi=intel_va:%1").arg(node),
                QStringLiteral("-init_hw_device"), QStringLiteral("qsv=intel_qsv@intel_va"),
                QStringLiteral("-filter_hw_device"), QStringLiteral("intel_qsv"), QStringLiteral("-hwaccel"),
                QStringLiteral("qsv"), QStringLiteral("-hwaccel_device"), QStringLiteral("intel_qsv"),
                QStringLiteral("-hwaccel_output_format"), QStringLiteral("qsv"), QStringLiteral("-extra_hw_frames"),
                QStringLiteral("32")};
        return {QStringLiteral("-hwaccel"), QStringLiteral("qsv"), QStringLiteral("-hwaccel_output_format"),
                QStringLiteral("qsv"), QStringLiteral("-extra_hw_frames"), QStringLiteral("32")};
#endif
    } else if (backend == QStringLiteral("NVIDIA") && features.hwaccels.contains(QStringLiteral("cuda"))) {
        if (label) *label = QStringLiteral("NVIDIA CUDA decode");
        return {QStringLiteral("-hwaccel"), QStringLiteral("cuda"), QStringLiteral("-hwaccel_device"),
            QStringLiteral("0"), QStringLiteral("-hwaccel_output_format"), QStringLiteral("cuda"),
            QStringLiteral("-extra_hw_frames"), QStringLiteral("32")};
    }
    return {};
}

QList<QPair<QString, QStringList>> MediaEngine::cpuDecodeGpuEncodeArguments(
    const QString &encoder, const QString &backend) {
    if (encoder.endsWith(QStringLiteral("_vaapi"))) {
        const QString node = linuxRenderNode(backend);
        if (!node.isEmpty()) {
            const QString device = backend == QStringLiteral("AMD") ? QStringLiteral("amd_va") : QStringLiteral("intel_va");
            return {{backend + QStringLiteral(" VAAPI upload"),
                {QStringLiteral("-init_hw_device"), QStringLiteral("vaapi=%1:%2").arg(device, node),
                 QStringLiteral("-filter_hw_device"), device, QStringLiteral("-filter:v:0"),
                 QStringLiteral("format=nv12,hwupload")}}};
        }
        return {};
    }
    if (encoder.endsWith(QStringLiteral("_qsv")))
        return {{QStringLiteral("Intel QSV"), {QStringLiteral("-filter:v:0"), QStringLiteral("format=nv12")}}};
    if (encoder.endsWith(QStringLiteral("_amf")))
        return {{QStringLiteral("AMD AMF"), {QStringLiteral("-filter:v:0"), QStringLiteral("format=nv12")}}};
    if (encoder.endsWith(QStringLiteral("_nvenc")))
        return {{QStringLiteral("NVIDIA NVENC"), {QStringLiteral("-filter:v:0"), QStringLiteral("format=nv12")}}};
    return {};
}
