// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#include "threshold_controller.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <stdexcept>

namespace
{
    QString digest(const QByteArray &data)
    {
        return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
    }
    QMap<QString, QByteArray> envelope(const QByteArray &data)
    {
        QMap<QString, QByteArray> result;
        for (const auto &line : data.split('\n'))
        {
            const auto at = line.indexOf('=');
            if (at > 0)
                result.insert(QString::fromLatin1(line.left(at)), line.mid(at + 1));
        }
        return result;
    }
    QString blob(const QString &name, const QString &command)
    {
        return QString("printf '%1='; %2 | base64 -w0; printf '\\n'\n").arg(name, command);
    }
    QString readFile(const QString &name, const QString &path)
    {
        return blob(name, "cat -- " + posixShellQuote(path));
    }
    QString absolute(const QString &base, const QString &path)
    {
        return QDir::cleanPath(path.startsWith('/') ? path : base + '/' + path);
    }
    [[noreturn]] void error(const QString &text) { throw std::runtime_error(text.toStdString()); }
}

ThresholdController::ThresholdController(QObject *parent) : QObject(parent), runner_(this)
{
    connect(&runner_, &RemoteCommandRunner::standardOutputReceived, this,
            [this](const QByteArray &data)
            { output_ += data; });
    connect(&runner_, &RemoteCommandRunner::standardErrorReceived, this,
            [this](const QByteArray &data)
            { errors_ += data; });
    connect(&runner_, &RemoteCommandRunner::failedToStart, this,
            [this](const QString &message)
            { finished(false, message); });
    connect(&runner_, &RemoteCommandRunner::finished, this,
            [this](int code, QProcess::ExitStatus status, bool cancelled)
            {
                finished(code == 0 && status == QProcess::NormalExit && !cancelled, QString::fromUtf8(errors_));
            });
}
void ThresholdController::configure(const SshConnectionOptions &connection, const QString &password)
{
    if (connection.host != connection_.host || connection.user != connection_.user ||
        connection.port != connection_.port || connection.identityFile != connection_.identityFile ||
        connection.remoteDirectory != connection_.remoteDirectory)
        invalidate();
    connection_ = connection;
    password_ = password;
}
void ThresholdController::invalidate()
{
    if (pending_)
        complete("Unconfirmed/stale — connection or deployment changed");
    valid_ = false;
    readback_ = {};
    cursor_.clear();
    invocation_.clear();
    disabledAtStartup_ = false;
    if (runner_.isRunning())
        runner_.cancel();
    phase_ = Phase::idle;
    identitySince_ = 0;
    status_ = "Unconfirmed/stale — refresh deployed thresholds";
    emit changed();
}
void ThresholdController::setExternalBusy(bool busy)
{
    if (externalBusy_ == busy)
        return;
    externalBusy_ = busy;
    // The panel also observes changes through the main-window availability update.
    QTimer::singleShot(0, this, [this]
                       { emit changed(); });
}
void ThresholdController::refresh()
{
    if (phase_ != Phase::idle || runner_.isRunning() || externalBusy_)
        return;
    readProperties();
}
void ThresholdController::start(Phase phase, const QString &script, bool locked)
{
    phase_ = phase;
    output_.clear();
    errors_.clear();
    const auto sudo = password_.isEmpty() ? "sudo -n -- bash -c " : "sudo -S -p '' -- bash -c ";
    QString command = sudo + posixShellQuote("set -euo pipefail\n" + script);
    if (locked)
        command = thresholds::deploymentLock(command);
    auto input = password_.isEmpty() ? QByteArray() : password_.toUtf8() + '\n';
    runner_.start(connection_, command, input);
    emit changed();
}
void ThresholdController::readProperties()
{
    start(Phase::properties,
          blob("PROPERTIES", "systemctl show " + posixShellQuote(service_) +
                                 " -p WorkingDirectory -p BindReadOnlyPaths -p RootDirectory -p InvocationID -p MainPID -p User -p Group -p LogNamespace -p NeedDaemonReload") +
              blob("ROOT", "pwd -P") +
              "pid=$(systemctl show --value -p MainPID " + posixShellQuote(service_) + ")\n"
                                                                                       "test \"$pid\" -gt 0 || { echo 'Live threshold controls require the running installed service' >&2; exit 1; }\n" +
              readFile("ARGV", "/dev/null").replace("cat -- '/dev/null'", "cat -- \"/proc/$pid/cmdline\""));
}
QString ThresholdController::hostPath(const QString &path) const
{
    const auto target = absolute(workingDirectory_, path);
    QString mapped = target;
    int longest = -1;
    // Refuse escaped/ambiguous mappings instead of editing a guessed host path.
    const auto binds = properties_.value("BindReadOnlyPaths").split(' ', Qt::SkipEmptyParts);
    for (const auto &bind : binds)
    {
        const auto parts = bind.split(':');
        if (parts.size() < 1 || parts.size() > 3 || bind.contains('\\'))
            error("Unsupported systemd bind mapping");
        auto source = parts[0];
        if (source.startsWith('-'))
            source.remove(0, 1);
        const auto destination = parts.size() > 1 ? parts[1] : source;
        if (destination.isEmpty() || !source.startsWith('/') || !destination.startsWith('/'))
            error("Invalid bind mapping");
        if ((target == destination || target.startsWith(destination + '/')) && destination.size() > longest)
        {
            mapped = source + target.mid(destination.size());
            longest = destination.size();
        }
    }
    return mapped;
}
void ThresholdController::readSnapshot()
{
    QString script = readFile("MANIFEST", manifestPath_);
    if (!policyPath_.isEmpty())
    {
        script += "if test -e " + posixShellQuote(policyPath_) + "; then\n" +
                  readFile("POLICY", policyPath_) + "else echo POLICY_MISSING=1; fi\n";
    }
    else
        script += "echo POLICY_MISSING=1\n";
    const auto journal = "journalctl --no-pager --output=json --unit=" + posixShellQuote(service_) +
                         (namespace_.isEmpty() ? QString() : " --namespace=" + posixShellQuote(namespace_)) +
                         " _SYSTEMD_INVOCATION_ID=" + posixShellQuote(invocation_) +
                         (cursor_.isEmpty() ? " --lines=200" : " --after-cursor=" + posixShellQuote(cursor_));
    // Journal permission/rotation failures must not conceal a readable saved state.
    script += "journal_tmp=$(mktemp)\ntrap 'rm -f -- \"$journal_tmp\"' EXIT\n";
    script += "if " + journal + " > \"$journal_tmp\"; then\n" +
              blob("JOURNAL", "cat -- \"$journal_tmp\"") + "else echo JOURNAL_ERROR=1; fi\n";
    if (policyPath_.isEmpty() && !disabledAtStartup_)
    {
        const auto startup = "journalctl --no-pager --output=json --unit=" + posixShellQuote(service_) +
                             (namespace_.isEmpty() ? QString() : " --namespace=" + posixShellQuote(namespace_)) +
                             " _SYSTEMD_INVOCATION_ID=" + posixShellQuote(invocation_) + " --grep=runtime_starting --lines=1";
        script += "if " + startup + " > \"$journal_tmp\"; then\n" +
                  blob("STARTUP", "cat -- \"$journal_tmp\"") + "fi\n";
    }
    script += "printf 'NOW=%s\\n' \"$(date +%s%6N)\"\n";
    script += blob("INVOCATION", "systemctl show --value -p InvocationID " + posixShellQuote(service_));
    start(Phase::snapshot, script);
}
void ThresholdController::finished(bool success, const QString &message)
{
    const auto phase = phase_;
    phase_ = Phase::idle;
    if (phase == Phase::idle)
        return;
    try
    {
        const auto fields = envelope(output_);
        if (phase == Phase::write)
        {
            const bool manifestSaved = fields.value("MANIFEST_SAVED") == "1";
            const bool policySaved = fields.value("POLICY_SAVED") == "1";
            operationSaved_ = manifestSaved || fields.contains("MANIFEST_REPLACED") ||
                              (action_ == "checkbox" && (policySaved || fields.contains("POLICY_REPLACED")));
            requestedAfter_ = fields.value("WRITE_TIME").toLongLong();
            deadline_ = requestedAfter_ + freshnessUs_;
            QString result = success ? "Saved/Pending" : "Save failed: " + message.trimmed();
            if (!success && (policySaved || fields.contains("POLICY_REPLACED")))
                result = "Partial save: policy replaced; " + QString(manifestSaved ? "manifest saved; " : "manifest not confirmed saved; ") + message.trimmed();
            else if (!success && fields.contains("MANIFEST_REPLACED"))
                result = "Partial save: manifest replaced; durability check failed. " + message.trimmed();
            if (!success)
                saveWarning_ = result;
            try
            {
                audit(result, operationSaved_, true);
            }
            catch (const std::exception &e)
            {
                pending_ = false;
                status_ = result;
                saveWarning_ = "Audit failed after the file operation: " + QString::fromUtf8(e.what());
                valid_ = false;
                emit changed();
                return;
            }
            status_ = result;
            if (!success)
                pending_ = false;
            // Readback is explicitly requested with Refresh, including after
            // partial failures. A pending confirmation must not lock that button.
            valid_ = false;
            emit changed();
            return;
        }
        if (!success)
            error(message.isEmpty() ? "Remote threshold read failed" : message.trimmed());
        if (phase == Phase::properties)
        {
            properties_ = thresholds::settings(QByteArray::fromBase64(fields.value("PROPERTIES")));
            if (!properties_.value("RootDirectory").isEmpty() || properties_.value("NeedDaemonReload") == "yes")
                error("Unsupported service root or stale unit definition; refresh the installed unit first");
            const auto nextInvocation = properties_.value("InvocationID");
            if (!QRegularExpression("^[0-9a-f]{32}$").match(nextInvocation).hasMatch())
                error("No current service invocation");
            if (invocation_ != nextInvocation)
            {
                if (pending_)
                    complete("Unconfirmed/stale — service invocation changed");
                readback_ = {};
                cursor_.clear();
                valid_ = false;
                identitySince_ = 0;
                disabledAtStartup_ = false;
                invocation_ = nextInvocation;
            }
            workingDirectory_ = properties_.value("WorkingDirectory");
            namespace_ = properties_.value("LogNamespace");
            if (!workingDirectory_.startsWith('/'))
                error("Service working directory is unavailable");
            const auto checkout = QString::fromUtf8(QByteArray::fromBase64(fields.value("ROOT"))).trimmed();
            if (hostPath(workingDirectory_) != checkout)
                error("Remote project root must match the backing service working directory (shared deployment lock)");
            const auto args = QByteArray::fromBase64(fields.value("ARGV")).split('\0');
            QString runtimeConfig;
            for (int i = 1; i < args.size(); ++i)
            {
                if (args[i] == "--config" && i + 1 < args.size())
                    runtimeConfig = QString::fromUtf8(args[++i]);
                else if (args[i].startsWith("--config="))
                    runtimeConfig = QString::fromUtf8(args[i].mid(9));
                // A command-line override would make the config alone insufficient.
                else if (args[i].startsWith("--manifest") || args[i].startsWith("--threshold") ||
                         args[i].startsWith("--early-emission") || args[i] == "--no-early-emission")
                    error("Runtime command-line policy overrides are unsupported; use the service config");
            }
            if (runtimeConfig.isEmpty())
                error("Cannot resolve running service --config");
            configPath_ = hostPath(runtimeConfig);
            start(Phase::config, readFile("CONFIG", configPath_));
            return;
        }
        if (phase == Phase::config)
        {
            config_ = QByteArray::fromBase64(fields.value("CONFIG"));
            configRevision_ = digest(config_);
            const auto config = thresholds::settings(config_);
            if (config.value("manifest_path").isEmpty())
                error("Runtime config has no manifest_path");
            manifestPath_ = hostPath(config.value("manifest_path"));
            policyPath_ = config.value("threshold_override_path").isEmpty() ? QString() : hostPath(config.value("threshold_override_path"));
            const auto early = config.value("early_emission", "false");
            if (early != "true" && early != "false")
                error("Malformed configured early_emission");
            configuredEarly_ = early == "true";
            bool a = false, b = false;
            const auto running = config.value("status_interval_ms", "5000").toLongLong(&a);
            const auto paused = config.value("paused_status_interval_ms", "60000").toLongLong(&b);
            if (!a || !b || running < 0 || paused < 0)
                error("Malformed status intervals");
            freshnessUs_ = std::max<qint64>(15000000, (std::max(running, paused) * 2 + 10000) * 1000);
            readSnapshot();
            return;
        }
        if (phase == Phase::snapshot)
        {
            if (QString::fromUtf8(QByteArray::fromBase64(fields.value("INVOCATION"))).trimmed() != invocation_)
            {
                invalidate();
                error("Service restarted while reading; refresh required");
            }
            remoteNow_ = fields.value("NOW").toLongLong();
            if (fields.contains("STARTUP"))
            {
                for (const auto &line : QByteArray::fromBase64(fields.value("STARTUP")).split('\n'))
                {
                    const auto entry = QJsonDocument::fromJson(line).object();
                    const auto e = QJsonDocument::fromJson(entry.value("MESSAGE").toString().toUtf8()).object();
                    if (entry.value("_SYSTEMD_INVOCATION_ID") == invocation_ && e.value("schema") == "dkfz-live-diagnostic-v1" &&
                        e.value("code") == "runtime_starting" && e.value("threshold_override_path").isString() &&
                        e.value("threshold_override_path").toString().isEmpty())
                        disabledAtStartup_ = true;
                }
            }
            const auto next = thresholds::Manifest::parse(QByteArray::fromBase64(fields.value("MANIFEST")));
            if (!manifest_.contents.isEmpty() && next.identity() != manifest_.identity())
            {
                if (pending_)
                    complete("Unconfirmed/stale — deployed model changed");
                readback_ = {};
                cursor_.clear();
                identitySince_ = remoteNow_;
                // Discard this batch: diagnostics have no fingerprint and could predate activation.
                manifest_ = next;
                valid_ = true;
                status_ = "Unconfirmed/stale — deployed model changed; waiting for fresh status";
                emit changed();
                return;
            }
            manifest_ = next;
            policyRevision_ = fields.contains("POLICY_MISSING") ? "missing" : digest(QByteArray::fromBase64(fields.value("POLICY")));
            savedPolicy_ = "No policy file (running fallback may still be retained)";
            selectedEarly_ = readback_.early.value_or(configuredEarly_);
            if (fields.contains("POLICY"))
            {
                const auto raw = QByteArray::fromBase64(fields.value("POLICY"));
                try
                {
                    if (raw.size() > 1024)
                        error("Policy exceeds 1024 bytes");
                    const auto policy = thresholds::settings(raw);
                    for (const auto &key : {"model_name", "model_variant", "engine_size_bytes", "engine_crc32"})
                        if (policy.value(key) != manifest_.identity().value(key).toString())
                            error("Saved policy belongs to another model/engine");
                    const auto value = thresholds::probability(policy.value("threshold"));
                    const auto early = policy.value("early_emission");
                    if (!early.isEmpty() && early != "true" && early != "false")
                        error("Malformed saved checkbox request");
                    if (!early.isEmpty())
                        selectedEarly_ = early == "true";
                    savedPolicy_ = QString("Saved fallback %1; early emission %2").arg(QString::number(value, 'g', 17), early.isEmpty() ? "configured default" : early);
                }
                catch (const std::exception &e)
                {
                    savedPolicy_ = "Invalid saved policy: " + QString::fromUtf8(e.what());
                }
            }
            valid_ = true;
            if (fields.contains("JOURNAL_ERROR"))
            {
                cursor_.clear();
                status_ = "Unconfirmed/stale — status reporting unavailable (journal read failed)";
            }
            else
            {
                consumeJournal(QByteArray::fromBase64(fields.value("JOURNAL")));
                if (readback_.activeAt == 0 && !pending_)
                    status_ = "Unconfirmed/stale — no usable native status readback";
                reconcile();
            }
            if (pending_ && remoteNow_ > deadline_)
                complete(readback_.activeAt == 0
                             ? "Unconfirmed/stale — status reporting unavailable; no matching native confirmation"
                             : "Unconfirmed/stale — no matching native confirmation");
            emit changed();
        }
    }
    catch (const std::exception &e)
    {
        fail(QString::fromUtf8(e.what()));
    }
}
void ThresholdController::consumeJournal(const QByteArray &journal)
{
    for (const auto &line : journal.split('\n'))
    {
        if (line.trimmed().isEmpty())
            continue;
        const auto entry = QJsonDocument::fromJson(line).object();
        if (entry.value("_SYSTEMD_INVOCATION_ID").toString() != invocation_)
            continue;
        const auto nextCursor = entry.value("__CURSOR").toString();
        if (nextCursor.isEmpty() || nextCursor == cursor_)
            continue;
        cursor_ = nextCursor;
        const auto event = QJsonDocument::fromJson(entry.value("MESSAGE").toString().toUtf8()).object();
        if (event.value("schema") != "dkfz-live-diagnostic-v1")
            continue;
        const auto time = entry.value("__REALTIME_TIMESTAMP").toString().toLongLong();
        if (time <= identitySince_ || time > remoteNow_ || remoteNow_ - time > freshnessUs_)
            continue;
        readback_.observe(event, time, cursor_);
        if (pending_ && time > requestedAfter_ &&
            ((event.value("code") == "manual_threshold_rejected" && action_ != "checkbox") ||
             (event.value("code") == "threshold_override_rejected" && (action_ == "reset" || action_ == "checkbox"))))
            complete("Rejected — " + event.value("message").toString());
    }
}
void ThresholdController::reconcile()
{
    if (pending_)
    {
        const auto wanted = thresholds::probability(number_);
        // Disk state must still agree. A concurrent edit cannot acknowledge this operation.
        const bool savedMatches = expectedManual_ ? (manifest_.manual && thresholds::equal(*manifest_.manual, wanted)) : !manifest_.manual;
        if (!savedMatches)
        {
            complete("Unconfirmed/stale — saved value changed by another writer");
            return;
        }
        if (readback_.matches(wanted, expectedManual_, action_ == "checkbox" || action_ == "reset" ? std::optional<bool>(requestedEarly_) : std::nullopt, requestedAfter_))
            complete("Applied");
        else
            status_ = readback_.paused ? "Waiting for input — saved; previous confirmed active policy retained" : "Saved/Pending";
    }
    else if (readback_.activeAt && remoteNow_ - readback_.activeAt > freshnessUs_)
    {
        status_ = "Unconfirmed/stale — native status has expired";
    }
}
void ThresholdController::request(const QString &action, const QString &number, bool early)
{
    if (!ready())
        return;
    try
    {
        // Config disabled from process start cannot have accepted a fallback in this invocation.
        if (action == "reset" && policyPath_.isEmpty() && !disabledAtStartup_)
            error("Cannot prove the policy mechanism was disabled for this invocation; fallback reset is required");
        operation_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        action_ = action;
        number_ = number;
        requestedEarly_ = early;
        previousManual_ = manifest_.manual;
        operationIdentity_ = manifest_.identity();
        expectedManual_ = action == "checkbox" ? manifest_.manual.has_value() : action != "reset";
        operationSaved_ = false;
        saveWarning_.clear();
        readback_.message.clear();
        audit("Requested"); // A failed initial audit prevents an unrecorded write.
        const auto script = thresholds::checkedWrite(manifest_, manifestPath_, policyPath_, policyRevision_,
                                                     configPath_, configRevision_, invocation_, service_, action, number, early,
                                                     properties_.value("User").isEmpty() ? "root" : properties_.value("User"),
                                                     properties_.value("Group").isEmpty() ? (properties_.value("User").isEmpty() ? "root" : properties_.value("User")) : properties_.value("Group"));
        pending_ = true;
        status_ = "Saving…";
        start(Phase::write, script, true);
    }
    catch (const std::exception &e)
    {
        status_ = QString::fromUtf8(e.what());
        emit changed();
    }
}
void ThresholdController::apply(const QString &source, const QString &custom)
{
    try
    {
        request(source, manifest_.numericChoice(source, custom), selectedEarly_);
    }
    catch (const std::exception &e)
    {
        status_ = QString::fromUtf8(e.what());
        emit changed();
    }
}
void ThresholdController::restoreAutomatic() { request("reset", manifest_.values.value("threshold"), selectedEarly_); }
void ThresholdController::setEarlyEmission(bool enabled)
{
    if (!policyAvailable())
        return;
    request("checkbox", manifest_.manual ? manifest_.values.value("manual_threshold") : manifest_.values.value("threshold"), enabled);
}
void ThresholdController::complete(const QString &result)
{
    pending_ = false;
    status_ = result;
    try
    {
        audit(result, operationSaved_);
    }
    catch (const std::exception &e)
    {
        status_ += "; audit failed: " + QString::fromUtf8(e.what());
    }
}
void ThresholdController::fail(const QString &message)
{
    valid_ = false;
    if (pending_ && phase_ != Phase::write)
        complete("Unconfirmed/stale — " + message);
    else
        status_ = "Unconfirmed/stale — " + message;
    emit changed();
}
QString ThresholdController::deployment() const
{
    return valid_ ? QString("%1 / %2 — %3\nInvocation %4").arg(manifest_.values.value("model_name"), manifest_.values.value("model_variant"), manifestPath_, invocation_) : "Deployed model unavailable";
}
QString ThresholdController::auditPath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/threshold-actions.json";
}
void ThresholdController::audit(const QString &result, bool saved, bool fileResult)
{
    const auto path = auditPath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        error("Cannot create threshold audit directory");
    QLockFile lock(path + ".lock");
    if (!lock.tryLock(1000))
        error("Threshold audit is locked");
    QJsonArray records;
    QFile input(path);
    if (input.exists())
    {
        if (!input.open(QIODevice::ReadOnly))
            error(input.errorString());
        QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(input.readAll(), &parse);
        if (parse.error != QJsonParseError::NoError || !document.isArray())
            error("Invalid threshold audit file");
        records = document.array();
    }
    QJsonObject record{{"timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                       {"operation_id", operation_},
                       {"action", action_},
                       {"identity", operationIdentity_},
                       {"invocation", invocation_},
                       {"endpoint", sshDestination(connection_) + ':' + QString::number(connection_.port) + ':' + connection_.remoteDirectory},
                       {"previous_saved", previousManual_ ? QJsonValue(*previousManual_) : QJsonValue::Null},
                       {"requested_value", number_},
                       {"requested_early", requestedEarly_},
                       {"new_saved", action_ == "reset" ? QJsonValue::Null : (action_ == "checkbox" ? (previousManual_ ? QJsonValue(*previousManual_) : QJsonValue::Null) : QJsonValue(number_))},
                       {"result", result},
                       {"saved", saved},
                       {"file_result", fileResult},
                       {"diagnostic_cursor", readback_.cursor},
                       {"diagnostic_message", readback_.message},
                       {"observed_at_us", QString::number(readback_.observedAt)}};
    records.append(record);
    QSaveFile output(path);
    const auto bytes = QJsonDocument(records).toJson(QJsonDocument::Compact);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit())
        error("Cannot persist threshold audit: " + output.errorString());
}
QString ThresholdController::savedSource() const
{
    if (!manifest_.manual)
    {
        if (readback_.active && !thresholds::equal(*readback_.active, manifest_.automatic))
            return "External/fallback override";
        return thresholds::sourceLabel("automatic");
    }
    QFile input(auditPath());
    if (input.open(QIODevice::ReadOnly))
    {
        const auto records = QJsonDocument::fromJson(input.readAll()).array();
        const auto endpoint = sshDestination(connection_) + ':' + QString::number(connection_.port) + ':' + connection_.remoteDirectory;
        for (auto i = records.size(); i > 0; --i)
        {
            const auto record = records[i - 1].toObject();
            const auto source = record.value("action").toString();
            if (record.value("saved").toBool() && record.value("file_result").toBool() &&
                record.value("identity").toObject() == manifest_.identity() &&
                record.value("endpoint").toString() == endpoint &&
                (source == "custom" || source == "der_preset" || source == "fp_preset" || source == "reset"))
            {
                if (source == "reset")
                    break;
                try
                {
                    if (thresholds::equal(thresholds::probability(record.value("requested_value").toString()), *manifest_.manual))
                        return thresholds::sourceLabel(source);
                }
                catch (...)
                {
                }
                break;
            }
        }
    }
    return thresholds::sourceLabel("unknown");
}
