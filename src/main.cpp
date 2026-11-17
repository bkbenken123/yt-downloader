#include "mainwindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFont>

int main(int argc, char *argv[]) {
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
