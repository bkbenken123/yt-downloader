#pragma once
#include "appconfig.h"
#include "downloadworker.h"
#include <QMainWindow>
#include <QPointer>

class QComboBox;
class QLineEdit;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QCheckBox;
class QCloseEvent;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
protected:
    void closeEvent(QCloseEvent *event) override;
private slots:
    void startDownload();
    void cancelDownload();
    void openSettings();
    void openOutputFolder();
    void updateModeControls();
    void updateContainerOptions();
private:
    AppConfig config_;
    QLineEdit *urls_ = nullptr;
    QComboBox *mode_ = nullptr;
    QComboBox *container_ = nullptr;
    QComboBox *videoCodec_ = nullptr;
    QComboBox *audioCodec_ = nullptr;
    QLabel *videoCodecLabel_ = nullptr;
    QLabel *audioCodecLabel_ = nullptr;
    QLineEdit *outputDirectory_ = nullptr;
    QPushButton *startButton_ = nullptr;
    QPushButton *cancelButton_ = nullptr;
    QPushButton *openFolderButton_ = nullptr;
    QPushButton *settingsButton_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QPlainTextEdit *log_ = nullptr;
    QPointer<DownloadWorker> downloadWorker_;
    QPointer<ToolWorker> toolWorker_;
    QString lastOutputFolder_;
    bool toolsReady_ = false;
    bool closing_ = false;

    void appendLog(const QString &message);
    void setBusy(bool busy);
    void startToolTask(ToolWorker::Task task);
    void settingsChanged(const AppConfig &settings);
    bool validateInputs(QStringList *urls, QString *error) const;
    void updateReadyState();
};
