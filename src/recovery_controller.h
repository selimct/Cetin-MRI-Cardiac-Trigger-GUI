// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#pragma once
#include "recovery_state.h"
#include "remote_command_runner.h"

class RecoveryController final : public QObject
{
    Q_OBJECT
  public:
    explicit RecoveryController(QObject *parent = nullptr);
    void configure(const SshConnectionOptions &connection, const QString &password);
    void refresh();
    void invalidate();
    void discardReadback();
    void setExternalBusy(bool busy);
    void apply(bool enabled, bool custom, const QString &threshold);
    void restoreCalibrated();
    void notePrimaryChange(const QJsonObject &identity);
    bool blocking() const { return phase_ != Phase::idle; }
    bool ready() const
    {
        return !blocking() && !externalBusy_ && state_.supportsRecovery() && state_.liveSupported();
    }
    bool refreshAvailable() const { return !blocking() && !externalBusy_; }
    const recovery::State &state() const { return state_; }
    QString status() const { return status_; }
  signals:
    void changed();
    void invalidated();

  private:
    enum class Phase
    {
        idle,
        manifest,
        metadata,
        mutation
    };
    void start(Phase phase, const QString &command);
    void finished(bool success, const QString &error);
    void mutate(const QString &arguments);
    RemoteCommandRunner runner_;
    SshConnectionOptions connection_;
    QString password_, status_{"Unconfirmed — Refresh recovery state"};
    recovery::State state_;
    Phase phase_{Phase::idle};
    bool externalBusy_{false};
    QByteArray output_, errors_;
    QJsonObject primaryEditedIdentity_;
};
