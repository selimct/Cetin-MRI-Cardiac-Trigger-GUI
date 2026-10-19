// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#pragma once

#include "remote_command_runner.h"
#include "threshold_state.h"

// GUI backend. All host I/O uses the existing OpenSSH/sudo transport, never the
// desktop filesystem. The audit is local to this GUI user and remote endpoint.
class ThresholdController final : public QObject
{
    Q_OBJECT
public:
    explicit ThresholdController(QObject *parent = nullptr);
    void configure(const SshConnectionOptions &connection, const QString &password);
    void refresh();
    void invalidate();
    void setExternalBusy(bool busy);
    void apply(const QString &source, const QString &custom = {});
    void restoreAutomatic();
    void setEarlyEmission(bool enabled);
    bool blocking() const { return phase_ != Phase::idle; }
    bool ready() const { return valid_ && !externalBusy_ && phase_ == Phase::idle && !pending_; }
    bool policyAvailable() const { return valid_ && !policyPath_.isEmpty() && manifest_.fingerprinted(); }
    bool canReset() const { return valid_ && (policyPath_.isEmpty() ? disabledAtStartup_ : manifest_.fingerprinted()); }
    const thresholds::Manifest &manifest() const { return manifest_; }
    const thresholds::Readback &readback() const { return readback_; }
    QString savedSource() const;
    QString status() const { return status_ + (saveWarning_.isEmpty() ? QString() : "\n" + saveWarning_); }
    QString savedPolicy() const { return savedPolicy_; }
    QString deployment() const;
    bool selectedEarly() const { return selectedEarly_; }
    bool configuredEarly() const { return configuredEarly_; }
    QString auditPath() const;
signals:
    void changed();

private:
    enum class Phase
    {
        idle,
        properties,
        config,
        snapshot,
        write
    };
    void start(Phase phase, const QString &script, bool locked = false);
    void finished(bool success, const QString &error);
    void readProperties();
    void readSnapshot();
    QString hostPath(const QString &runtimePath) const;
    void request(const QString &action, const QString &number, bool early);
    void reconcile();
    void complete(const QString &result);
    void audit(const QString &result, bool saved = false, bool fileResult = false);
    void consumeJournal(const QByteArray &journal);
    void fail(const QString &error);
    RemoteCommandRunner runner_;
    SshConnectionOptions connection_;
    QString password_;
    Phase phase_{Phase::idle};
    bool valid_{false}, externalBusy_{false}, pending_{false};
    QByteArray output_, errors_, config_;
    QMap<QString, QString> properties_;
    QString workingDirectory_, invocation_, service_{"dkfz-native-uart-live.service"};
    QString configPath_, manifestPath_, policyPath_, configRevision_, policyRevision_, namespace_;
    QString cursor_, savedPolicy_, status_{"Unconfirmed/stale — refresh deployed thresholds"};
    thresholds::Manifest manifest_;
    thresholds::Readback readback_;
    bool configuredEarly_{false}, selectedEarly_{false};
    bool disabledAtStartup_{false};
    QString saveWarning_;
    qint64 remoteNow_{}, freshnessUs_{130000000}, requestedAfter_{}, deadline_{}, identitySince_{};
    QString operation_, action_, number_;
    bool requestedEarly_{false}, expectedManual_{false}, operationSaved_{false};
    QJsonObject operationIdentity_;
    std::optional<double> previousManual_;
};
