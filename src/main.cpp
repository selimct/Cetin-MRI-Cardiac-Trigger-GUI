// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#include "main_window.h"

#include "connection_config.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Jetson Control"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.2.0"));
    QCoreApplication::setOrganizationName(QStringLiteral("DKFZ"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Run live-stack operations on a Jetson over SSH."));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption connectionFileOption(
        {QStringLiteral("c"), QStringLiteral("connection-file")},
        QStringLiteral("Load SSH settings from <path>."), QStringLiteral("path"));
    parser.addOption(connectionFileOption);
    parser.process(application);

    const bool explicitlyConfigured = parser.isSet(connectionFileOption);
    const QString connectionFile =
        explicitlyConfigured
            ? QFileInfo(parser.value(connectionFileOption)).absoluteFilePath()
            : findDefaultConnectionConfig(QCoreApplication::applicationDirPath(),
                                          QDir::currentPath());
    const ConnectionConfigResult connection =
        loadConnectionConfig(connectionFile, explicitlyConfigured);
    if (!connection.error.isEmpty())
    {
        QMessageBox::critical(nullptr, QStringLiteral("Invalid connection file"),
                              connection.error);
        return 2;
    }

    MainWindow window(connection.options,
                      connection.found ? connection.path : QString{});
    window.show();
    return application.exec();
}
