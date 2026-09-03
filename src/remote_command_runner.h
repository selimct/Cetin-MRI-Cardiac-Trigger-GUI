// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#pragma once

#include "ssh_command_builder.h"

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QString>

class RemoteCommandRunner final : public QObject {
  Q_OBJECT

public:
  explicit RemoteCommandRunner(QObject *parent = nullptr);

  [[nodiscard]] bool isRunning() const;
  [[nodiscard]] QString sshExecutable() const;

public slots:
  void start(const SshConnectionOptions &options, const QString &command,
             QByteArray standardInput = {});
  void cancel();

signals:
  void started(const QString &destination, const QString &command);
  void standardOutputReceived(const QByteArray &data);
  void standardErrorReceived(const QByteArray &data);
  void failedToStart(const QString &message);
  void finished(int exitCode, QProcess::ExitStatus exitStatus, bool cancelled);

private:
  QProcess process_;
  QString sshExecutable_;
  bool cancelRequested_{false};
};
