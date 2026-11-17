#pragma once
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

struct AppConfig {
    QString encoderBackend = QStringLiteral("CPU");
    QString defaultDownloadDir;
    bool downloadPlaylist = true;
    bool useYtdlpAudioConversion = true;
    bool embedMetadata = false;

    static QString configPath() {
        QDir directory(QCoreApplication::applicationDirPath());
        for (int depth = 0; depth < 6; ++depth) {
            if (QDir(directory.filePath(QStringLiteral("binaries"))).exists())
                return directory.filePath(QStringLiteral("config.json"));
            if (!directory.cdUp()) break;
        }
        return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("config.json"));
    }
    static AppConfig defaults() {
        AppConfig value;
        value.defaultDownloadDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        if (value.defaultDownloadDir.isEmpty())
            value.defaultDownloadDir = QDir::home().filePath(QStringLiteral("Downloads"));
        return value;
    }
    static AppConfig fromJson(const QJsonObject &object) {
        AppConfig value = defaults();
        const QString backend = object.value(QStringLiteral("encoder_backend")).toString().trimmed().toUpper();
        if (backend == QStringLiteral("AMD") || backend == QStringLiteral("INTEL") ||
            backend == QStringLiteral("NVIDIA") || backend == QStringLiteral("CPU"))
            value.encoderBackend = backend;
        const QString directory = object.value(QStringLiteral("default_download_dir")).toString().trimmed();
        if (!directory.isEmpty())
            value.defaultDownloadDir = QDir::cleanPath(QDir::fromNativeSeparators(directory));
        if (object.value(QStringLiteral("download_playlist")).isBool())
            value.downloadPlaylist = object.value(QStringLiteral("download_playlist")).toBool();
        if (object.value(QStringLiteral("use_ytdlp_audio_conversion")).isBool())
            value.useYtdlpAudioConversion = object.value(QStringLiteral("use_ytdlp_audio_conversion")).toBool();
        if (object.value(QStringLiteral("embed_metadata")).isBool())
            value.embedMetadata = object.value(QStringLiteral("embed_metadata")).toBool();
        return value;
    }
    static AppConfig load(QString *warning = nullptr) {
        if (warning) warning->clear();
        QFile file(configPath());
        if (!file.exists()) return defaults();
        if (!file.open(QIODevice::ReadOnly)) {
            if (warning) *warning = QStringLiteral("Could not read config.json; defaults were loaded: %1").arg(file.errorString());
            return defaults();
        }
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            if (warning) *warning = QStringLiteral("Could not parse config.json; defaults were loaded: %1").arg(error.errorString());
            return defaults();
        }
        return fromJson(document.object());
    }
    bool save(QString *error = nullptr) const {
        QJsonObject object;
        object.insert(QStringLiteral("encoder_backend"), encoderBackend.toUpper());
        object.insert(QStringLiteral("default_download_dir"), QDir::toNativeSeparators(QDir::cleanPath(defaultDownloadDir)));
        object.insert(QStringLiteral("download_playlist"), downloadPlaylist);
        object.insert(QStringLiteral("use_ytdlp_audio_conversion"), useYtdlpAudioConversion);
        object.insert(QStringLiteral("embed_metadata"), embedMetadata);
        QSaveFile file(configPath());
        if (!file.open(QIODevice::WriteOnly)) {
            if (error) *error = file.errorString();
            return false;
        }
        const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Indented) + '\n';
        if (file.write(bytes) != bytes.size() || !file.commit()) {
            if (error) *error = file.errorString();
            return false;
        }
        return true;
    }
};

struct DownloadOptions {
    QStringList urls;
    QString mode = QStringLiteral("video");
    QString container = QStringLiteral("mp4");
    QString videoCodec = QStringLiteral("copy");
    QString audioCodec = QStringLiteral("copy");
    QString outputDirectory;
    AppConfig settings;
};
