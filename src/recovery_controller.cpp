// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#include "recovery_controller.h"
#include <QCryptographicHash>
#include <QDir>
#include <QJsonDocument>
#include <QTimer>
#include <stdexcept>

namespace
{
const QString manifestPath = "deploy/model/model.conf";
const QString helper = "build_native/inference/native_live_recovery_control";
QString blob(const QString &key, const QString &command)
{
    return "printf '" + key + "='; " + command + " | base64 -w0; printf '\\n'\n";
}
QMap<QString, QByteArray> envelope(const QByteArray &data)
{
    QMap<QString, QByteArray> result;
    for (const auto &line : data.split('\n'))
    {
        const auto at = line.indexOf('=');
        if (at > 0)
            result[QString::fromLatin1(line.left(at))] = line.mid(at + 1);
    }
    return result;
}
QString apiRead()
{
    return "if test -x " + posixShellQuote(helper) +
           "; then\n"
           "api_tmp=$(mktemp)\ntrap 'rm -f -- \"$api_tmp\"' EXIT\n"
           "if " +
           posixShellQuote(helper) + " --status --json > \"$api_tmp\"; then\n" +
           blob("API", "cat -- \"$api_tmp\"") +
           "else echo API_ERROR=1; fi\nelse echo API_UNAVAILABLE=1; fi\n";
}
} // namespace
RecoveryController::RecoveryController(QObject *parent) : QObject(parent), runner_(this)
{
    connect(&runner_, &RemoteCommandRunner::standardOutputReceived, this,
            [this](const QByteArray &v) { output_ += v; });
    connect(&runner_, &RemoteCommandRunner::standardErrorReceived, this,
            [this](const QByteArray &v) { errors_ += v; });
    connect(&runner_, &RemoteCommandRunner::failedToStart, this,
            [this](const QString &v) { finished(false, v); });
    connect(
        &runner_, &RemoteCommandRunner::finished, this,
        [this](int code, QProcess::ExitStatus status, bool cancelled)
        { finished(code == 0 && status == QProcess::NormalExit && !cancelled, QString::fromUtf8(errors_)); });
}
void RecoveryController::configure(const SshConnectionOptions &c, const QString &password)
{
    if (c.host != connection_.host || c.user != connection_.user || c.port != connection_.port ||
        c.identityFile != connection_.identityFile || c.remoteDirectory != connection_.remoteDirectory)
        invalidate();
    connection_ = c;
    password_ = password;
}
void RecoveryController::invalidate()
{
    primaryEditedIdentity_ = {};
    emit invalidated();
    discardReadback();
}
void RecoveryController::discardReadback()
{
    phase_ = Phase::idle;
    if (runner_.isRunning())
        runner_.cancel();
    state_ = {};
    status_ = "Unconfirmed/stale — Refresh recovery state";
    emit changed();
}
void RecoveryController::setExternalBusy(bool busy)
{
    if (externalBusy_ == busy)
        return;
    externalBusy_ = busy;
    QTimer::singleShot(0, this, [this] { emit changed(); });
}
void RecoveryController::start(Phase phase, const QString &script)
{
    phase_ = phase;
    output_.clear();
    errors_.clear();
    const auto sudo = password_.isEmpty() ? "sudo -n -- bash -c " : "sudo -S -p '' -- bash -c ";
    runner_.start(connection_, sudo + posixShellQuote("set -euo pipefail\n" + script),
                  password_.isEmpty() ? QByteArray() : password_.toUtf8() + '\n');
    emit changed();
}
void RecoveryController::refresh()
{
    if (!refreshAvailable() || runner_.isRunning())
        return;
    state_.api = {}; // No historical status is confirmation of a new snapshot.
    status_ = "Reading recovery deployment over SSH…";
    start(Phase::manifest, blob("MANIFEST", "cat -- " + posixShellQuote(manifestPath)));
}
void RecoveryController::finished(bool success, const QString &error)
{
    const auto phase = phase_;
    phase_ = Phase::idle;
    if (phase == Phase::idle)
        return;
    try
    {
        if (!success)
            throw std::runtime_error(error.trimmed().toStdString());
        const auto fields = envelope(output_);
        if (phase == Phase::manifest)
        {
            state_ = {};
            state_.manifest = thresholds::Manifest::parse(QByteArray::fromBase64(fields.value("MANIFEST")));
            state_.primaryEdited = primaryEditedIdentity_ == state_.manifest.identity();
            const auto source = QDir::cleanPath(state_.manifest.values.value("source_model_dir"));
            const auto variant = state_.manifest.values.value("model_variant");
            QString script = "test \"$(sha256sum -- " + posixShellQuote(manifestPath) +
                             " | cut -d ' ' -f1)\" = " + posixShellQuote(state_.manifest.revision) +
                             " || { echo 'Deployment changed; refresh required' >&2; exit 1; }\n";
            if (!source.isEmpty() && source != "." && !source.startsWith('/') && source != ".." &&
                !source.startsWith("../") && (variant == "best" || variant == "tuned"))
            {
                const auto path =
                    source + (variant == "best" ? "/train_config.json" : "/tuning/tuned_config.json");
                const auto engineValue = state_.manifest.values.value("engine_path", "model.engine");
                const auto engine = QDir::cleanPath(engineValue.startsWith('/')
                    ? engineValue : "deploy/model/" + engineValue);
                // Read only beneath the configured remote checkout, including symlinks.
                script += "source_path=$(readlink -f -- " + posixShellQuote(path) +
                          " || true)\n"
                          "case \"$source_path\" in \"$(pwd -P)\"/*)\n"
                          "if test -r \"$source_path\" && test \"$(stat -c %s -- \"$source_path\")\" -le "
                          "2097152; then\n" +
                          "source_before=$(sha256sum -- \"$source_path\" | cut -d ' ' -f1)\n" +
                          blob("SOURCE", "cat -- \"$source_path\"") +
                          "source_after=$(sha256sum -- \"$source_path\" | cut -d ' ' -f1)\n"
                          "if test \"$source_before\" = \"$source_after\"; then printf "
                          "'SOURCE_REVISION=%s\\n' \"$source_after\"; else echo SOURCE_STALE=1; fi\n" +
                          "if test -e " + posixShellQuote(engine) + " && ! test \"$source_path\" -nt " +
                          posixShellQuote(engine) + "; then echo SOURCE_UNCHANGED=1; fi\nfi;; esac\n";
            }
            script += apiRead();
            script += "test \"$(sha256sum -- " + posixShellQuote(manifestPath) +
                      " | cut -d ' ' -f1)\" = " + posixShellQuote(state_.manifest.revision) +
                      " || { echo 'Deployment changed; refresh required' >&2; exit 1; }\n";
            start(Phase::metadata, script);
            return;
        }
        if (phase == Phase::metadata)
        {
            QJsonParseError parseError;
            const auto document =
                QJsonDocument::fromJson(QByteArray::fromBase64(fields.value("SOURCE")), &parseError);
            state_.sourceAvailable =
                parseError.error == QJsonParseError::NoError && document.isObject() &&
                document.object().value("modelname").toString() ==
                    state_.manifest.values.value("model_name") &&
                document.object().value("recovery_head").toBool(false) == state_.supportsRecovery();
            state_.source = state_.sourceAvailable ? document.object() : QJsonObject{};
            state_.sourceRevision =
                QString::fromLatin1(QCryptographicHash::hash(QByteArray::fromBase64(fields.value("SOURCE")),
                                                             QCryptographicHash::Sha256)
                                        .toHex());
            if (fields.value("SOURCE_REVISION") != state_.sourceRevision.toLatin1() ||
                fields.contains("SOURCE_STALE"))
            {
                state_.sourceAvailable = false;
                state_.source = {};
            }
            state_.sourceUnchanged = state_.sourceAvailable && fields.value("SOURCE_UNCHANGED") == "1";
            if (fields.contains("API"))
                state_.acceptApi(
                    QJsonDocument::fromJson(QByteArray::fromBase64(fields.value("API"))).object());
            else
                state_.warning = "Live recovery unavailable: remote native recovery-control API v1 is absent "
                                 "or could not be checked. Use the deployment policy and existing stop / "
                                 "prepare / validate / start workflow.";
            if (!state_.api.isEmpty() &&
                state_.api.value("source_revision").toString() != state_.sourceRevision)
            {
                state_.sourceAvailable = false;
                state_.sourceUnchanged = false;
                state_.source = {};
                state_.warning = "Calibration unconfirmed — source changed while reading or backend could "
                                 "not bind its revision; Refresh required";
            }
            if (!state_.api.isEmpty())
            {
                // The helper resolves the engine and service mount mappings. Its
                // source checks must also approve restoration of our snapshot.
                state_.sourceAvailable = state_.sourceAvailable &&
                    state_.api.value("source_metadata_available").toBool();
                state_.sourceUnchanged = state_.sourceUnchanged && state_.sourceAvailable &&
                    state_.api.value("source_predates_engine").toBool();
            }
            if (!state_.liveSupported() && state_.warning.isEmpty())
                state_.warning = state_.api.value("reason").toString(
                    "Live recovery unavailable for this target; managed service API v1 required");
            status_ = state_.liveSupported() ? state_.api.value("confirmation").toString("Unconfirmed")
                                             : state_.warning;
            if (state_.liveSupported() && !state_.warning.isEmpty())
                status_ += "\n" + state_.warning;
        }
        else if (phase == Phase::mutation)
        {
            const auto response = QJsonDocument::fromJson(output_).object();
            if (response.value("service_invocation_id") != state_.api.value("service_invocation_id"))
                throw std::runtime_error("Service invocation changed; Refresh required");
            if (!state_.acceptApi(response))
                throw std::runtime_error(state_.warning.toStdString());
            // A successful command saves a request, it does not confirm inference.
            state_.api["active"] = QJsonObject{};
            state_.api["confirmation"] = "saved/requested — pending confirmation";
            status_ = "Saved/requested — pending native confirmation. Refresh explicitly; paused input can "
                      "wait for the next completed frame.";
        }
    }
    catch (const std::exception &e)
    {
        state_ = {};
        status_ = "Unconfirmed / rejected — " + QString::fromUtf8(e.what());
    }
    emit changed();
}
void RecoveryController::mutate(const QString &arguments)
{
    if (!ready())
        return;
    const auto revision = state_.api.value("revision").toString();
    const auto invocation = state_.api.value("service_invocation_id").toString();
    start(Phase::mutation, posixShellQuote(helper) + " " + arguments + " --json --expected-revision " +
                               posixShellQuote(revision) + " --expected-invocation " +
                               posixShellQuote(invocation));
}
void RecoveryController::apply(bool enabled, bool custom, const QString &value)
{
    try
    {
        QString args = enabled ? "--enable" : "--disable";
        if (custom)
            args += " --threshold " + posixShellQuote(QString::number(recovery::threshold(value), 'g', 17));
        else if (state_.calibrated() && state_.sourceUnchanged)
            args += " --threshold " + posixShellQuote(QString::number(*state_.calibrated(), 'g', 17));
        else if (enabled)
            throw std::runtime_error("Recovery disabled by calibration or calibration unconfirmed; choose an "
                                     "explicit advanced custom threshold to enable");
        mutate(args);
    }
    catch (const std::exception &e)
    {
        status_ = QString::fromUtf8(e.what());
        emit changed();
    }
}
void RecoveryController::restoreCalibrated() { mutate("--restore-calibrated"); }
void RecoveryController::setEnabled(bool enabled)
{
    // Omit --threshold so the backend keeps the selected value, including when
    // recovery is disabled or a service restart restored its startup policy.
    mutate(enabled ? "--enable" : "--disable");
}

void RecoveryController::notePrimaryChange(const QJsonObject &identity)
{
    primaryEditedIdentity_ = identity;
    state_.primaryEdited = identity == state_.manifest.identity();
    emit changed();
}
