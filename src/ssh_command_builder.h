// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#pragma once

#include <QString>
#include <QStringList>

struct SshConnectionOptions {
  QString host;
  QString user;
  quint16 port{22};
  QString identityFile;
  QString remoteDirectory;
  bool acceptNewHostKey{false};
};

[[nodiscard]] QString posixShellQuote(const QString &value);
[[nodiscard]] QString sshDestination(const SshConnectionOptions &options);
[[nodiscard]] QString remoteShellCommand(const SshConnectionOptions &options,
                                         const QString &command);
[[nodiscard]] QStringList sshArguments(const SshConnectionOptions &options,
                                       const QString &command);
