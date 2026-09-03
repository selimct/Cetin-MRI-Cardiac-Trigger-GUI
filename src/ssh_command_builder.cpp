// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#include "ssh_command_builder.h"

QString posixShellQuote(const QString &value) {
  QString quoted = value;
  quoted.replace(QStringLiteral("'"), QStringLiteral("'\"'\"'"));
  return QStringLiteral("'") + quoted + QStringLiteral("'");
}

QString sshDestination(const SshConnectionOptions &options) {
  if (options.user.trimmed().isEmpty()) {
    return options.host.trimmed();
  }
  return options.user.trimmed() + QStringLiteral("@") + options.host.trimmed();
}

QString remoteShellCommand(const SshConnectionOptions &options,
                           const QString &command) {
  const QString shellCommand =
      QStringLiteral("exec bash -lc %1").arg(posixShellQuote(command));
  if (options.remoteDirectory.trimmed().isEmpty()) {
    return shellCommand;
  }

  return QStringLiteral("cd -- %1 && %2")
      .arg(posixShellQuote(options.remoteDirectory.trimmed()), shellCommand);
}

QStringList sshArguments(const SshConnectionOptions &options,
                         const QString &command) {
  QStringList arguments{
      QStringLiteral("-T"),
      QStringLiteral("-p"),
      QString::number(options.port),
      QStringLiteral("-o"),
      QStringLiteral("BatchMode=yes"),
      QStringLiteral("-o"),
      options.acceptNewHostKey
          ? QStringLiteral("StrictHostKeyChecking=accept-new")
          : QStringLiteral("StrictHostKeyChecking=yes"),
      QStringLiteral("-o"),
      QStringLiteral("ConnectTimeout=10"),
      QStringLiteral("-o"),
      QStringLiteral("ServerAliveInterval=15"),
      QStringLiteral("-o"),
      QStringLiteral("ServerAliveCountMax=2"),
  };

  if (!options.identityFile.trimmed().isEmpty()) {
    arguments.append({QStringLiteral("-o"),
                      QStringLiteral("IdentitiesOnly=yes"),
                      QStringLiteral("-i"), options.identityFile.trimmed()});
  }

  arguments.append(
      {sshDestination(options), remoteShellCommand(options, command)});
  return arguments;
}
