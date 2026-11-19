#include "mainwindow.h"
#include "toolmanager.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFont>
#include <functional>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <QUrl>
#include <QWidget>

namespace {
struct CodecItem { QString id; QString label; };

const QList<CodecItem> videoCodecs{
    {QStringLiteral("copy"), QStringLiteral("Copy")},
    {QStringLiteral("h264"), QStringLiteral("H.264")},
    {QStringLiteral("h265"), QStringLiteral("H.265 / HEVC")},
    {QStringLiteral("vp9"), QStringLiteral("VP9")},
    {QStringLiteral("av1"), QStringLiteral("AV1")},
    {QStringLiteral("prores_422"), QStringLiteral("ProRes 422")},
    {QStringLiteral("dnxhr_sq"), QStringLiteral("DNxHR SQ")},
    {QStringLiteral("dnxhr_hq"), QStringLiteral("DNxHR HQ")}
};
const QList<CodecItem> videoAudioCodecs{
    {QStringLiteral("copy"), QStringLiteral("Copy")},
    {QStringLiteral("aac"), QStringLiteral("AAC")},
    {QStringLiteral("opus"), QStringLiteral("Opus")},
    {QStringLiteral("flac"), QStringLiteral("FLAC")},
    {QStringLiteral("lpcm"), QStringLiteral("PCM / LPCM")},
    {QStringLiteral("mpeg-1"), QStringLiteral("MP3 / MPEG-1")},
    {QStringLiteral("mpeg-2"), QStringLiteral("MPEG-2 Audio")}
};
const QList<CodecItem> audioCodecs{
    {QStringLiteral("copy"), QStringLiteral("Copy")},
    {QStringLiteral("aac"), QStringLiteral("AAC")},
    {QStringLiteral("opus"), QStringLiteral("Opus")},
    {QStringLiteral("flac"), QStringLiteral("FLAC")},
    {QStringLiteral("lpcm"), QStringLiteral("PCM / LPCM")},
    {QStringLiteral("mpeg-1"), QStringLiteral("MP3 / MPEG-1")}
};

void populate(QComboBox *combo, const QList<CodecItem> &items, const QString &selected) {
    combo->clear();
    int selectedIndex = 0;
    for (int i = 0; i < items.size(); ++i) {
        combo->addItem(items.at(i).label, items.at(i).id);
        if (items.at(i).id == selected) selectedIndex = i;
    }
    combo->setCurrentIndex(selectedIndex);
}

bool listContains(const QStringList &list, const QString &value) {
    return list.contains(value);
}

class SettingsDialog final : public QDialog {
public:
    explicit SettingsDialog(const AppConfig &current, QWidget *parent = nullptr)
        : QDialog(parent), value_(current) {
        setWindowTitle(QStringLiteral("Downloader Settings"));
        setMinimumWidth(540);
        auto *root = new QVBoxLayout(this);
        auto *form = new QFormLayout;
        backend_ = new QComboBox(this);
        backend_->addItem(QStringLiteral("CPU"), QStringLiteral("CPU"));
        backend_->addItem(QStringLiteral("AMD"), QStringLiteral("AMD"));
        backend_->addItem(QStringLiteral("Intel"), QStringLiteral("INTEL"));
        backend_->addItem(QStringLiteral("NVIDIA"), QStringLiteral("NVIDIA"));
        backend_->setCurrentIndex(qMax(0, backend_->findData(current.encoderBackend)));
        form->addRow(QStringLiteral("Video encoder backend"), backend_);

        directory_ = new QLineEdit(current.defaultDownloadDir, this);
        auto *browse = new QPushButton(QStringLiteral("Browse..."), this);
        auto *directoryRow = new QWidget(this);
        auto *directoryLayout = new QHBoxLayout(directoryRow);
        directoryLayout->setContentsMargins(0, 0, 0, 0);
        directoryLayout->addWidget(directory_, 1);
        directoryLayout->addWidget(browse);
        form->addRow(QStringLiteral("Default download folder"), directoryRow);
        connect(browse, &QPushButton::clicked, this, [this] {
            const QString chosen = QFileDialog::getExistingDirectory(this, QStringLiteral("Select download folder"), directory_->text());
            if (!chosen.isEmpty()) directory_->setText(QDir::toNativeSeparators(chosen));
        });

        playlist_ = new QCheckBox(QStringLiteral("Download playlists"), this);
        playlist_->setChecked(current.downloadPlaylist);
        form->addRow(QStringLiteral("Playlist handling"), playlist_);
        ytdlpAudio_ = new QCheckBox(QStringLiteral("Allow yt-dlp to convert compatible audio downloads"), this);
        ytdlpAudio_->setChecked(current.useYtdlpAudioConversion);
        form->addRow(QStringLiteral("Audio conversion"), ytdlpAudio_);
        metadata_ = new QCheckBox(QStringLiteral("Embed thumbnail, metadata, and chapters"), this);
        metadata_->setChecked(current.embedMetadata);
        form->addRow(QStringLiteral("Metadata"), metadata_);
        root->addLayout(form);

        auto *update = new QPushButton(QStringLiteral("Update, Rebundle, and Restart"), this);
        update->setToolTip(QStringLiteral("Update FFmpeg and yt-dlp from source, rebuild them, rebundle the app, and restart."));
        root->addWidget(update);
        connect(update, &QPushButton::clicked, this, [this] {
            if (updateCallback_) updateCallback_();
        });

        auto *buttons = new QHBoxLayout;
        auto *save = new QPushButton(QStringLiteral("Save"), this);
        auto *reset = new QPushButton(QStringLiteral("Reset"), this);
        save->setDefault(true);
        buttons->addWidget(save);
        buttons->addStretch();
        buttons->addWidget(reset);
        root->addLayout(buttons);
        connect(save, &QPushButton::clicked, this, &QDialog::accept);
        connect(reset, &QPushButton::clicked, this, [this] {
            const AppConfig defaults = AppConfig::defaults();
            backend_->setCurrentIndex(backend_->findData(defaults.encoderBackend));
            directory_->setText(defaults.defaultDownloadDir);
            playlist_->setChecked(defaults.downloadPlaylist);
            ytdlpAudio_->setChecked(defaults.useYtdlpAudioConversion);
            metadata_->setChecked(defaults.embedMetadata);
        });
    }

    AppConfig value() const {
        AppConfig result;
        result.encoderBackend = backend_->currentData().toString();
        result.defaultDownloadDir = QDir::cleanPath(QDir::fromNativeSeparators(directory_->text().trimmed()));
        result.downloadPlaylist = playlist_->isChecked();
        result.useYtdlpAudioConversion = ytdlpAudio_->isChecked();
        result.embedMetadata = metadata_->isChecked();
        return result;
    }
    void setUpdateCallback(std::function<void()> callback) { updateCallback_ = std::move(callback); }

private:
    AppConfig value_;
    QComboBox *backend_ = nullptr;
    QLineEdit *directory_ = nullptr;
    QCheckBox *playlist_ = nullptr;
    QCheckBox *ytdlpAudio_ = nullptr;
    QCheckBox *metadata_ = nullptr;
    std::function<void()> updateCallback_;
};
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    QString configWarning;
    config_ = AppConfig::load(&configWarning);
    setWindowTitle(QStringLiteral("Downloader"));
    resize(900, 700);

    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(18, 18, 18, 14);
    root->setSpacing(12);

    auto *heading = new QLabel(QStringLiteral("Downloader"), central);
    QFont headingFont = heading->font();
    headingFont.setPointSize(19);
    headingFont.setWeight(QFont::DemiBold);
    heading->setFont(headingFont);
    root->addWidget(heading);

    auto *urlLabel = new QLabel(QStringLiteral("Video or playlist URLs (separate multiple links with semicolons)"), central);
    root->addWidget(urlLabel);
    auto *urlRow = new QHBoxLayout;
    urls_ = new QLineEdit(central);
    urls_->setPlaceholderText(QStringLiteral("https://www.youtube.com/watch?v=..."));
    auto *paste = new QPushButton(QStringLiteral("Paste"), central);
    urlRow->addWidget(urls_, 1);
    urlRow->addWidget(paste);
    root->addLayout(urlRow);
    connect(paste, &QPushButton::clicked, this, [this] {
        urls_->setText(QApplication::clipboard()->text().trimmed());
    });

    auto *options = new QHBoxLayout;
    mode_ = new QComboBox(central);
    mode_->addItem(QStringLiteral("Video"), QStringLiteral("video"));
    mode_->addItem(QStringLiteral("Audio"), QStringLiteral("audio"));
    mode_->addItem(QStringLiteral("Thumbnail only"), QStringLiteral("thumbnail"));
    container_ = new QComboBox(central);
    videoCodec_ = new QComboBox(central);
    audioCodec_ = new QComboBox(central);
    videoCodecLabel_ = new QLabel(QStringLiteral("Video codec"), central);
    audioCodecLabel_ = new QLabel(QStringLiteral("Audio codec"), central);
    options->addWidget(new QLabel(QStringLiteral("Mode"), central));
    options->addWidget(mode_);
    options->addWidget(new QLabel(QStringLiteral("Container"), central));
    options->addWidget(container_);
    options->addWidget(videoCodecLabel_);
    options->addWidget(videoCodec_);
    options->addWidget(audioCodecLabel_);
    options->addWidget(audioCodec_);
    root->addLayout(options);
    connect(mode_, qOverload<int>(&QComboBox::currentIndexChanged), this, &MainWindow::updateModeControls);
    connect(container_, qOverload<int>(&QComboBox::currentIndexChanged), this, &MainWindow::updateContainerOptions);

    auto *outputRow = new QHBoxLayout;
    outputDirectory_ = new QLineEdit(config_.defaultDownloadDir, central);
    auto *browseOutput = new QPushButton(QStringLiteral("Browse..."), central);
    outputRow->addWidget(new QLabel(QStringLiteral("Save to"), central));
    outputRow->addWidget(outputDirectory_, 1);
    outputRow->addWidget(browseOutput);
    root->addLayout(outputRow);
    connect(browseOutput, &QPushButton::clicked, this, [this] {
        const QString folder = QFileDialog::getExistingDirectory(this, QStringLiteral("Choose download folder"), outputDirectory_->text());
        if (!folder.isEmpty()) outputDirectory_->setText(QDir::toNativeSeparators(folder));
    });

    log_ = new QPlainTextEdit(central);
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(5000);
    log_->setPlaceholderText(QStringLiteral("Application and download messages appear here."));
    root->addWidget(log_, 1);

    auto *bottom = new QHBoxLayout;
    startButton_ = new QPushButton(QStringLiteral("Start"), central);
    cancelButton_ = new QPushButton(QStringLiteral("Cancel"), central);
    cancelButton_->setEnabled(false);
    openFolderButton_ = new QPushButton(QStringLiteral("Open Output Folder"), central);
    openFolderButton_->setEnabled(false);
    settingsButton_ = new QPushButton(QStringLiteral("Settings"), central);
    statusLabel_ = new QLabel(QStringLiteral("Preparing tools..."), central);
    statusLabel_->setMinimumWidth(155);
    bottom->addWidget(startButton_);
    bottom->addWidget(cancelButton_);
    bottom->addWidget(openFolderButton_);
    bottom->addStretch();
    bottom->addWidget(statusLabel_);
    bottom->addWidget(settingsButton_);
    root->addLayout(bottom);
    setCentralWidget(central);

    connect(startButton_, &QPushButton::clicked, this, &MainWindow::startDownload);
    connect(cancelButton_, &QPushButton::clicked, this, &MainWindow::cancelDownload);
    connect(openFolderButton_, &QPushButton::clicked, this, &MainWindow::openOutputFolder);
    connect(settingsButton_, &QPushButton::clicked, this, &MainWindow::openSettings);
    updateModeControls();

    if (!configWarning.isEmpty()) appendLog(configWarning);
    appendLog(QStringLiteral("Native C++/Qt 6 build. External yt-dlp and FFmpeg tools are discovered from binaries/ and PATH."));
    startToolTask(ToolWorker::Task::Prepare);
}

MainWindow::~MainWindow() {
    closing_ = true;
    if (downloadWorker_ && downloadWorker_->isRunning()) {
        downloadWorker_->cancel();
        downloadWorker_->wait();
    }
    if (toolWorker_ && toolWorker_->isRunning()) {
        toolWorker_->cancel();
        toolWorker_->wait();
    }
}

void MainWindow::closeEvent(QCloseEvent *event) {
    closing_ = true;
    if (downloadWorker_ && downloadWorker_->isRunning()) {
        downloadWorker_->cancel();
        statusLabel_->setText(QStringLiteral("Cancelling..."));
        downloadWorker_->wait();
    }
    if (toolWorker_ && toolWorker_->isRunning()) {
        toolWorker_->cancel();
        toolWorker_->wait();
    }
    event->accept();
}

void MainWindow::appendLog(const QString &message) {
    if (!log_) return;
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    log_->appendPlainText(QStringLiteral("[%1] %2").arg(stamp, message));
}

void MainWindow::setBusy(bool busy) {
    const bool toolBusy = toolWorker_ && toolWorker_->isRunning();
    const bool downloadBusy = downloadWorker_ && downloadWorker_->isRunning();
    const bool active = busy || toolBusy || downloadBusy;
    startButton_->setEnabled(!active && toolsReady_);
    cancelButton_->setEnabled(active);
    mode_->setEnabled(!active);
    container_->setEnabled(!active);
    videoCodec_->setEnabled(!active && mode_->currentData().toString() == QStringLiteral("video"));
    audioCodec_->setEnabled(!active && mode_->currentData().toString() != QStringLiteral("thumbnail"));
    urls_->setEnabled(!active);
    outputDirectory_->setEnabled(!active);
    settingsButton_->setEnabled(!active);
}

void MainWindow::updateReadyState() {
    setBusy(false);
}

void MainWindow::startToolTask(ToolWorker::Task task) {
    if (toolWorker_ && toolWorker_->isRunning()) return;
    if (downloadWorker_ && downloadWorker_->isRunning()) return;
    auto *worker = new ToolWorker(task, this);
    toolWorker_ = worker;
    connect(worker, &ToolWorker::logMessage, this, &MainWindow::appendLog);
    connect(worker, &ToolWorker::taskFinished, this,
        [this, task](bool success, bool restartRequired, const QStringList &summary) {
            for (const QString &line : summary) appendLog(line);
            if (task == ToolWorker::Task::Prepare) {
                toolsReady_ = success;
                statusLabel_->setText(success ? QStringLiteral("Ready") : QStringLiteral("Tool setup failed"));
            } else {
                if (restartRequired)
                    appendLog(QStringLiteral("Tools updated. Rebundling and restarting the application."));
                statusLabel_->setText(success ? QStringLiteral("Update check complete") : QStringLiteral("Update failed"));
            }
            updateReadyState();
            if (task == ToolWorker::Task::Update && success && restartRequired)
                QTimer::singleShot(750, this, [this] { close(); });
        });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this, [this, worker] {
        if (toolWorker_ == worker) toolWorker_.clear();
        updateReadyState();
    });
    statusLabel_->setText(task == ToolWorker::Task::Prepare
        ? QStringLiteral("Preparing tools...") : QStringLiteral("Updating tools..."));
    setBusy(true);
    worker->start();
}

void MainWindow::startDownload() {
    QStringList urls;
    QString error;
    if (!validateInputs(&urls, &error)) {
        QMessageBox::warning(this, QStringLiteral("Invalid download"), error);
        return;
    }
    const QString output = QDir::cleanPath(QDir::fromNativeSeparators(outputDirectory_->text().trimmed()));
    if (output.isEmpty() || !QDir().mkpath(output)) {
        QMessageBox::warning(this, QStringLiteral("Invalid output folder"), QStringLiteral("The output folder could not be created."));
        return;
    }
    DownloadOptions options;
    options.urls = urls;
    options.mode = mode_->currentData().toString();
    options.container = container_->currentData().toString();
    options.videoCodec = videoCodec_->currentData().toString();
    options.audioCodec = audioCodec_->currentData().toString();
    options.outputDirectory = output;
    options.settings = config_;

    auto *worker = new DownloadWorker(options, this);
    downloadWorker_ = worker;
    connect(worker, &DownloadWorker::logMessage, this, &MainWindow::appendLog);
    connect(worker, &DownloadWorker::batchFinished, this,
        [this](bool cancelled, int successful, int failed, const QString &folder) {
            lastOutputFolder_ = folder;
            openFolderButton_->setEnabled(!folder.isEmpty() && QDir(folder).exists());
            statusLabel_->setText(cancelled ? QStringLiteral("Cancelled")
                : QStringLiteral("%1 succeeded / %2 failed").arg(successful).arg(failed));
            appendLog(QStringLiteral("Batch summary: %1 succeeded, %2 failed.").arg(successful).arg(failed));
            updateReadyState();
        });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this, [this, worker] {
        if (downloadWorker_ == worker) downloadWorker_.clear();
        updateReadyState();
    });
    appendLog(QStringLiteral("Starting batch with %1 URL(s).").arg(urls.size()));
    statusLabel_->setText(QStringLiteral("Downloading..."));
    setBusy(true);
    worker->start();
}

void MainWindow::cancelDownload() {
    if (downloadWorker_ && downloadWorker_->isRunning()) {
        appendLog(QStringLiteral("Cancellation requested."));
        statusLabel_->setText(QStringLiteral("Cancelling..."));
        downloadWorker_->cancel();
    } else if (toolWorker_ && toolWorker_->isRunning()) {
        appendLog(QStringLiteral("Tool task cancellation requested."));
        statusLabel_->setText(QStringLiteral("Cancelling tool task..."));
        toolWorker_->cancel();
    }
}

void MainWindow::openSettings() {
    if ((toolWorker_ && toolWorker_->isRunning()) ||
        (downloadWorker_ && downloadWorker_->isRunning())) return;
    SettingsDialog dialog(config_, this);
    dialog.setUpdateCallback([this] {
        startToolTask(ToolWorker::Task::Update);
    });
    if (dialog.exec() == QDialog::Accepted) {
        const AppConfig next = dialog.value();
        settingsChanged(next);
    }
}

void MainWindow::settingsChanged(const AppConfig &settings) {
    const bool changed = settings.encoderBackend != config_.encoderBackend ||
        settings.defaultDownloadDir != config_.defaultDownloadDir ||
        settings.downloadPlaylist != config_.downloadPlaylist ||
        settings.useYtdlpAudioConversion != config_.useYtdlpAudioConversion ||
        settings.embedMetadata != config_.embedMetadata;
    if (!changed) return;
    QString error;
    if (!settings.save(&error)) {
        QMessageBox::critical(this, QStringLiteral("Settings save failed"), error);
        return;
    }
    config_ = settings;
    outputDirectory_->setText(config_.defaultDownloadDir);
    appendLog(QStringLiteral("Settings saved to config.json."));
}

void MainWindow::openOutputFolder() {
    QString folder = lastOutputFolder_;
    if (folder.isEmpty()) folder = outputDirectory_->text();
    if (!folder.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(QDir::cleanPath(folder)));
}

void MainWindow::updateModeControls() {
    const QString mode = mode_->currentData().toString();
    container_->clear();
    if (mode == QStringLiteral("video")) {
        container_->addItem(QStringLiteral("MP4"), QStringLiteral("mp4"));
        container_->addItem(QStringLiteral("MKV"), QStringLiteral("mkv"));
        container_->addItem(QStringLiteral("WebM"), QStringLiteral("webm"));
        container_->addItem(QStringLiteral("MOV"), QStringLiteral("mov"));
    } else if (mode == QStringLiteral("audio")) {
        container_->addItem(QStringLiteral("MP3"), QStringLiteral("mp3"));
        container_->addItem(QStringLiteral("M4A"), QStringLiteral("m4a"));
        container_->addItem(QStringLiteral("WAV"), QStringLiteral("wav"));
        container_->addItem(QStringLiteral("FLAC"), QStringLiteral("flac"));
        container_->addItem(QStringLiteral("Opus"), QStringLiteral("opus"));
    } else {
        container_->addItem(QStringLiteral("JPEG"), QStringLiteral("jpg"));
        container_->addItem(QStringLiteral("PNG"), QStringLiteral("png"));
        container_->addItem(QStringLiteral("WebP"), QStringLiteral("webp"));
    }
    videoCodec_->setVisible(mode == QStringLiteral("video"));
    videoCodecLabel_->setVisible(mode == QStringLiteral("video"));
    audioCodec_->setVisible(mode != QStringLiteral("thumbnail"));
    audioCodecLabel_->setVisible(mode != QStringLiteral("thumbnail"));
    updateContainerOptions();
}

void MainWindow::updateContainerOptions() {
    const QString mode = mode_->currentData().toString();
    const QString container = container_->currentData().toString();
    const QString previousVideo = videoCodec_->currentData().toString();
    const QString previousAudio = audioCodec_->currentData().toString();
    if (mode == QStringLiteral("video")) {
        QStringList allowedVideo;
        if (container == QStringLiteral("mp4")) allowedVideo = {QStringLiteral("copy"), QStringLiteral("h264"), QStringLiteral("h265"), QStringLiteral("vp9"), QStringLiteral("av1")};
        else if (container == QStringLiteral("mkv")) allowedVideo = {QStringLiteral("copy"), QStringLiteral("h264"), QStringLiteral("h265"), QStringLiteral("vp9"), QStringLiteral("av1"), QStringLiteral("prores_422"), QStringLiteral("dnxhr_sq"), QStringLiteral("dnxhr_hq")};
        else if (container == QStringLiteral("webm")) allowedVideo = {QStringLiteral("copy"), QStringLiteral("vp9"), QStringLiteral("av1")};
        else allowedVideo = {QStringLiteral("copy"), QStringLiteral("h264"), QStringLiteral("h265"), QStringLiteral("av1"), QStringLiteral("prores_422"), QStringLiteral("dnxhr_sq"), QStringLiteral("dnxhr_hq")};
        QStringList allowedAudio;
        if (container == QStringLiteral("webm")) allowedAudio = {QStringLiteral("copy"), QStringLiteral("opus")};
        else if (container == QStringLiteral("mkv")) allowedAudio = {QStringLiteral("copy"), QStringLiteral("aac"), QStringLiteral("opus"), QStringLiteral("flac"), QStringLiteral("lpcm"), QStringLiteral("mpeg-1"), QStringLiteral("mpeg-2")};
        else allowedAudio = {QStringLiteral("copy"), QStringLiteral("aac"), QStringLiteral("flac"), QStringLiteral("lpcm"), QStringLiteral("mpeg-1"), QStringLiteral("mpeg-2")};
        QList<CodecItem> videoItems, audioItems;
        for (const CodecItem &item : videoCodecs) if (listContains(allowedVideo, item.id)) videoItems << item;
        for (const CodecItem &item : videoAudioCodecs) if (listContains(allowedAudio, item.id)) audioItems << item;
        populate(videoCodec_, videoItems, listContains(allowedVideo, previousVideo) ? previousVideo : QStringLiteral("copy"));
        populate(audioCodec_, audioItems, listContains(allowedAudio, previousAudio) ? previousAudio : QStringLiteral("copy"));
    } else if (mode == QStringLiteral("audio")) {
        QStringList allowed;
        if (container == QStringLiteral("mp3")) allowed = {QStringLiteral("copy"), QStringLiteral("mpeg-1")};
        else if (container == QStringLiteral("m4a")) allowed = {QStringLiteral("copy"), QStringLiteral("aac")};
        else if (container == QStringLiteral("wav")) allowed = {QStringLiteral("copy"), QStringLiteral("lpcm")};
        else if (container == QStringLiteral("flac")) allowed = {QStringLiteral("copy"), QStringLiteral("flac")};
        else if (container == QStringLiteral("opus")) allowed = {QStringLiteral("copy"), QStringLiteral("opus")};
        QList<CodecItem> items;
        for (const CodecItem &item : audioCodecs) if (listContains(allowed, item.id)) items << item;
        populate(audioCodec_, items, listContains(allowed, previousAudio) ? previousAudio : QStringLiteral("copy"));
    }
    const bool active = !((downloadWorker_ && downloadWorker_->isRunning()) ||
                          (toolWorker_ && toolWorker_->isRunning()));
    videoCodec_->setEnabled(active && mode == QStringLiteral("video"));
    audioCodec_->setEnabled(active && mode != QStringLiteral("thumbnail"));
    container_->setEnabled(active);
}

bool MainWindow::validateInputs(QStringList *urls, QString *error) const {
    if (urls) urls->clear();
    if (error) error->clear();
    const QString text = urls_->text().trimmed();
    if (text.isEmpty()) {
        if (error) *error = QStringLiteral("Enter at least one URL.");
        return false;
    }
    const QStringList parts = text.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (QString item : parts) {
        item = item.trimmed();
        if (item.isEmpty()) continue;
        const QUrl url(item);
        if (!url.isValid() || url.host().isEmpty() ||
            (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https"))) {
            if (error) *error = QStringLiteral("This does not look like a valid HTTP/HTTPS URL:\n%1").arg(item);
            return false;
        }
        if (urls && !urls->contains(item)) urls->append(item);
    }
    if (!urls || urls->isEmpty()) {
        if (error) *error = QStringLiteral("Enter one or more valid URLs separated by semicolons.");
        return false;
    }
    return true;
}
