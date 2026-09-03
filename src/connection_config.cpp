// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#include "connection_config.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>

namespace {
constexpr auto kConnectionFileName = "connection.ini";
}

QString findDefaultConnectionConfig(const QString &applicationDirectory,
                                    const QString &currentDirectory) {
  const QStringList candidates{
      QDir(currentDirectory)
          .absoluteFilePath(QString::fromLatin1(kConnectionFileName)),
      QDir(applicationDirectory)
          .absoluteFilePath(QString::fromLatin1(kConnectionFileName)),
      QDir(applicationDirectory)
          .absoluteFilePath(QStringLiteral("../connection.ini")),
  };

  for (const QString &candidate : candidates) {
    const QFileInfo info(QDir::cleanPath(candidate));
    if (info.isFile() && info.isReadable()) {
      return info.absoluteFilePath();
    }
  }
  return {};
}

ConnectionConfigResult loadConnectionConfig(const QString &path,
                                            bool required) {
  ConnectionConfigResult result;
  if (path.trimmed().isEmpty()) {
    if (required) {
      result.error = QStringLiteral("No connection file was specified.");
    }
    return result;
  }

  const QFileInfo configInfo(path);
  result.path = configInfo.absoluteFilePath();
  if (!configInfo.isFile() || !configInfo.isReadable()) {
    if (required) {
      result.error = QStringLiteral("Connection file is not readable: %1")
                         .arg(result.path);
    }
    return result;
  }

  QSettings settings(result.path, QSettings::IniFormat);
  result.options.host =
      settings.value(QStringLiteral("ssh/host")).toString().trimmed();
  result.options.user =
      settings.value(QStringLiteral("ssh/user")).toString().trimmed();
  result.options.remoteDirectory =
      settings.value(QStringLiteral("ssh/remote_directory"))
          .toString()
          .trimmed();

  bool portIsValid = false;
  const int port =
      settings.value(QStringLiteral("ssh/port"), 22).toInt(&portIsValid);
  if (!portIsValid || port < 1 || port > 65535) {
    result.error = QStringLiteral("ssh/port in %1 must be between 1 and 65535.")
                       .arg(result.path);
    return result;
  }
  result.options.port = static_cast<quint16>(port);

  QString identityFile =
      settings.value(QStringLiteral("ssh/identity_file")).toString().trimmed();
  if (!identityFile.isEmpty()) {
    if (QDir::isRelativePath(identityFile)) {
      identityFile = configInfo.dir().absoluteFilePath(identityFile);
    }
    const QFileInfo identityInfo(QDir::cleanPath(identityFile));
    result.options.identityFile = identityInfo.absoluteFilePath();
    if (!identityInfo.isFile() || !identityInfo.isReadable()) {
      result.error = QStringLiteral("SSH identity file is not readable: %1")
                         .arg(result.options.identityFile);
      return result;
    }
  }

  if (settings.status() != QSettings::NoError) {
    result.error =
        QStringLiteral("Could not parse connection file: %1").arg(result.path);
    return result;
  }
  if (result.options.host.isEmpty()) {
    result.error =
        QStringLiteral("ssh/host is missing from %1.").arg(result.path);
    return result;
  }

  result.found = true;
  return result;
}
