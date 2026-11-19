#include "mainwindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFont>
#include <QProcessEnvironment>

int main(int argc, char *argv[]) {
    const QString applicationDirectory = QCoreApplication::applicationDirPath();
    qputenv("DOWNLOADER_BUNDLED_ROOT", applicationDirectory.toUtf8());

    const QByteArray localAppData = qgetenv("LOCALAPPDATA");
    if (!localAppData.isEmpty()) {
        const QString configRoot = QDir(QString::fromUtf8(localAppData)).filePath(QStringLiteral("Downloader"));
        QDir().mkpath(configRoot);
        qputenv("DOWNLOADER_CONFIG_ROOT", configRoot.toUtf8());
    }

    const QString pluginsDirectory = QDir(applicationDirectory).filePath(QStringLiteral("plugins"));
    qputenv("QT_PLUGIN_PATH", pluginsDirectory.toUtf8());

    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Downloader"));
    QCoreApplication::setApplicationVersion(QStringLiteral("2026.11.17-cpp"));
    QCoreApplication::setOrganizationName(QStringLiteral("Downloader"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Native C++/Qt 6 media downloader using external yt-dlp and FFmpeg binaries."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.process(application);

    application.setStyle(QStringLiteral("Fusion"));
    MainWindow window;
    window.show();
    return application.exec();
}
