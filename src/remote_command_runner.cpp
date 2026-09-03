// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#include "remote_command_runner.h"

#include <QStandardPaths>
#include <QTimer>

RemoteCommandRunner::RemoteCommandRunner(QObject *parent)
    : QObject(parent),
      sshExecutable_(QStandardPaths::findExecutable(QStringLiteral("ssh"))) {
  process_.setProcessChannelMode(QProcess::SeparateChannels);

  connect(&process_, &QProcess::readyReadStandardOutput, this, [this] {
    emit standardOutputReceived(process_.readAllStandardOutput());
  });
  connect(&process_, &QProcess::readyReadStandardError, this, [this] {
    emit standardErrorReceived(process_.readAllStandardError());
  });
  connect(&process_, &QProcess::errorOccurred, this,
          [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
              emit failedToStart(process_.errorString());
            }
          });
  connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
          this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
            emit finished(exitCode, exitStatus, cancelRequested_);
          });
}

bool RemoteCommandRunner::isRunning() const {
  return process_.state() != QProcess::NotRunning;
}

QString RemoteCommandRunner::sshExecutable() const { return sshExecutable_; }

void RemoteCommandRunner::start(const SshConnectionOptions &options,
                                const QString &command,
                                QByteArray standardInput) {
  if (isRunning()) {
    emit failedToStart(
        QStringLiteral("Another remote command is already running."));
    return;
  }
  if (sshExecutable_.isEmpty()) {
    emit failedToStart(QStringLiteral(
        "OpenSSH was not found. Install the 'ssh' client or add it to PATH."));
    return;
  }
  if (options.host.trimmed().isEmpty()) {
    emit failedToStart(
        QStringLiteral("Enter the Jetson host name or IP address."));
    return;
  }
  if (command.trimmed().isEmpty()) {
    emit failedToStart(QStringLiteral("Enter a command to run."));
    return;
  }

  cancelRequested_ = false;
  process_.setProgram(sshExecutable_);
  process_.setArguments(sshArguments(options, command));
  process_.start();
  if (!standardInput.isEmpty()) {
    process_.write(standardInput);
    standardInput.fill('\0');
  }
  process_.closeWriteChannel();
  emit started(sshDestination(options), command);
}

void RemoteCommandRunner::cancel() {
  if (!isRunning()) {
    return;
  }

  cancelRequested_ = true;
  process_.terminate();
  QTimer::singleShot(2000, this, [this] {
    if (isRunning()) {
      process_.kill();
    }
  });
}
