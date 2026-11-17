#pragma once
#include "appconfig.h"
#include <QThread>
#include <QHash>
#include <QString>
#include <QStringList>
#include <atomic>

class DownloadWorker final : public QThread {
    Q_OBJECT
public:
    explicit DownloadWorker(DownloadOptions options, QObject *parent = nullptr);
    void cancel();
signals:
    void logMessage(const QString &message);
    void batchFinished(bool cancelled, int successfulLinks, int failedLinks, const QString &lastOutputFolder);
protected:
    void run() override;
private:
    struct FileSignature { qint64 modifiedMs = 0; qint64 size = 0; };
    struct LinkResult { bool success = false; QString outputFolder; };
    DownloadOptions options_;
    std::atomic_bool cancelled_{false};
    QString activeOutputFolder_;
    QHash<QString, FileSignature> activeBeforeSnapshot_;
    QStringList downloadedFiles_;
    QStringList preservedThumbnailPaths_;

    void log(const QString &message);
    bool isCancelled() const;
    bool ytdlpVideoNative() const;
    bool ytdlpAudioNative() const;
    QString playlistTitle(const QString &url);
    QString outputFolderForUrl(const QString &url);
    QHash<QString, FileSignature> snapshotFiles(const QString &folder) const;
    QStringList changedFiles(const QString &folder, const QHash<QString, FileSignature> &before) const;
    bool runYtdlp(const QString &url, const QString &outputTemplate, const QString &mode,
                  QStringList *errors, QStringList *foundPaths);
    bool finalizeNative(const QString &source, const QString &destination, const QString &mode);
    void cleanupThumbnails(const QStringList &paths, bool preserve);
    void cleanupSuccess(const QString &source, const QString &destination,
                        const QString &thumbnail);
    void cleanupCancelled();
    LinkResult processUrl(const QString &url, int index, int total);
    void logCapabilities();
};

class ToolWorker final : public QThread {
    Q_OBJECT
public:
    enum class Task { Prepare, Update };
    explicit ToolWorker(Task task, QObject *parent = nullptr);
    void cancel();
signals:
    void logMessage(const QString &message);
    void taskFinished(bool success, bool restartRequired, const QStringList &summary);
protected:
    void run() override;
private:
    Task task_;
    std::atomic_bool cancelled_{false};
};
