// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#include "threshold_state.h"
#include "ssh_command_builder.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace thresholds
{
    namespace
    {
        [[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
        QString hash(const QByteArray &data)
        {
            return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
        }
        std::optional<double> optionalNumber(const QMap<QString, QString> &values, const QString &key)
        {
            const auto text = values.value(key);
            if (text.isEmpty())
                return {};
            return probability(text);
        }
    }

    QMap<QString, QString> settings(const QByteArray &text)
    {
        if (text.contains('\0') || QString::fromUtf8(text).toUtf8() != text)
            fail("Invalid UTF-8 configuration");
        QMap<QString, QString> result;
        for (const auto &raw : text.split('\n'))
        {
            const auto line = QString::fromUtf8(raw).section('#', 0, 0).trimmed();
            if (line.isEmpty())
                continue;
            const auto at = line.indexOf('=');
            const auto key = line.left(at).trimmed();
            if (at < 1 || !QRegularExpression("^[A-Za-z0-9_.]+$").match(key).hasMatch() || result.contains(key))
                fail("Malformed or duplicate setting: " + key);
            result.insert(key, line.mid(at + 1).trimmed());
        }
        return result;
    }

    double probability(const QString &text)
    {
        static const QRegularExpression number("^[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?$");
        bool ok = false;
        const double value = text.toDouble(&ok);
        if (!number.match(text).hasMatch() || !ok || !std::isfinite(value) || value < 0 || value > 1)
            fail("Threshold must be a finite number from 0 through 1");
        return value;
    }
    bool equal(double a, double b)
    {
        return std::abs(a - b) <= 4 * std::numeric_limits<double>::epsilon() * std::max({1.0, std::abs(a), std::abs(b)});
    }

    Manifest Manifest::parse(const QByteArray &text)
    {
        Manifest m;
        m.contents = text;
        m.revision = hash(text);
        m.values = settings(text);
        m.automatic = probability(m.values.value("threshold"));
        m.manual = optionalNumber(m.values, "manual_threshold");
        m.fp = optionalNumber(m.values, "fp_threshold");
        m.der = optionalNumber(m.values, "der_threshold");
        for (const auto &key : {"model_name", "model_variant"})
            if (m.values.value(key).isEmpty())
                fail("Missing model identity");
        // Older manifests can still save manual values, but cannot write a policy.
        if (!m.values.value("engine_size_bytes").isEmpty())
        {
            bool ok = false;
            m.values.value("engine_size_bytes").toULongLong(&ok);
            if (!ok || !QRegularExpression("^[0-9]+$").match(m.values.value("engine_size_bytes")).hasMatch())
                fail("Malformed engine size");
        }
        if (!m.values.value("engine_crc32").isEmpty() &&
            !QRegularExpression("^[0-9a-f]{8}$").match(m.values.value("engine_crc32")).hasMatch())
            fail("Malformed engine CRC32");
        return m;
    }
    QJsonObject Manifest::identity() const
    {
        QJsonObject result;
        for (const auto &key : {"model_name", "model_variant", "engine_size_bytes", "engine_crc32"})
            result.insert(key, values.value(key));
        if (!values.value("engine_size_bytes").isEmpty())
            result.insert("engine_size_bytes", QString::number(values.value("engine_size_bytes").toULongLong()));
        return result;
    }
    bool Manifest::fingerprinted() const
    {
        return !values.value("engine_size_bytes").isEmpty() && !values.value("engine_crc32").isEmpty();
    }
    QByteArray Manifest::patchManual(const QString &value) const
    {
        if (!value.isEmpty())
            probability(value);
        auto lines = contents.split('\n');
        bool found = false;
        for (auto &line : lines)
        {
            const auto assignment = line.split('#').first();
            const auto at = assignment.indexOf('=');
            if (at >= 0 && assignment.left(at).trimmed() == "manual_threshold")
            {
                const int comment = line.indexOf('#');
                const auto old = line.mid(at + 1, (comment < 0 ? line.size() : comment) - at - 1);
                int start = 0, end = old.size();
                while (start < old.size() && (old[start] == ' ' || old[start] == '\t'))
                    ++start;
                while (end > start && (old[end - 1] == ' ' || old[end - 1] == '\t' || old[end - 1] == '\r'))
                    --end;
                if (old.trimmed().isEmpty())
                    start = end = 0;
                line = line.left(at + 1) + old.left(start) + value.toUtf8() + old.mid(end) +
                       (comment < 0 ? QByteArray() : line.mid(comment));
                found = true;
            }
        }
        auto result = lines.join('\n');
        if (!found)
        {
            if (!result.endsWith('\n'))
                result += '\n';
            result += "manual_threshold=" + value.toUtf8() + '\n';
        }
        return result;
    }
    QString Manifest::numericChoice(const QString &source, const QString &custom) const
    {
        QString number;
        if (source == "custom")
            number = custom;
        else if (source == "der_preset")
            number = values.value("der_threshold");
        else if (source == "fp_preset")
            number = values.value("fp_threshold");
        else
            fail("Unknown numeric source");
        probability(number);
        return number;
    }
    QByteArray Manifest::policy(const QString &number, bool early) const
    {
        probability(number);
        if (!fingerprinted())
            fail("Policy requires an engine fingerprint");
        const auto result = QString("model_name=%1\nmodel_variant=%2\nengine_size_bytes=%3\nengine_crc32=%4\nthreshold=%5\nearly_emission=%6\n")
                                .arg(values.value("model_name"), values.value("model_variant"),
                                     QString::number(values.value("engine_size_bytes").toULongLong()),
                                     values.value("engine_crc32"), number, early ? "true" : "false")
                                .toUtf8();
        if (result.size() > 1024)
            fail("Policy exceeds 1024 bytes");
        return result;
    }
    QString sourceLabel(const QString &source)
    {
        if (source == "custom")
            return "Manual (custom)";
        if (source == "der_preset")
            return "Manual (DER preset)";
        if (source == "fp_preset")
            return "Manual (FP preset)";
        if (source == "automatic")
            return "Automatic/model default";
        return "Manual (source unknown)";
    }

    void Readback::observe(const QJsonObject &e, qint64 time, const QString &c)
    {
        const auto code = e.value("code").toString();
        cursor = c;
        observedAt = time;
        if (code == "manual_threshold_rejected" || code == "threshold_override_rejected")
        {
            message = e.value("message").toString();
            return;
        }
        if (code == "threshold_requested" && e.value("threshold").isDouble() &&
            e.value("early_emission").isBool() && e.value("manual_override").isBool())
        {
            queuedThreshold = e.value("threshold").toDouble();
            queuedEarly = e.value("early_emission").toBool();
            queuedManual = e.value("manual_override").toBool();
        }
        if (code == "input_paused")
            paused = true;
        if (code == "input_signal_recovered")
            paused = false;
        if ((code == "manual_threshold_configured" || code == "runtime_status") &&
            e.value("manual_override_configured").isBool() && e.value("configured_threshold").isDouble())
        {
            manual = e.value("manual_override_configured").toBool();
            configured = e.value("configured_threshold").toDouble();
            acceptedAt = time;
        }
        if ((code == "threshold_changed" || code == "runtime_status") &&
            e.value("threshold").isDouble() && e.value("early_emission").isBool())
        {
            const auto value = e.value("threshold").toDouble();
            if (std::isfinite(value) && value >= 0 && value <= 1)
            {
                active = value;
                early = e.value("early_emission").toBool();
                if (e.value("emission_threshold").isDouble())
                    emission = e.value("emission_threshold").toDouble();
                activeAt = time;
            }
        }
        if (code == "runtime_status" && e.value("input_paused").isBool())
            paused = e.value("input_paused").toBool();
    }
    bool Readback::matches(double value, bool manualExpected, std::optional<bool> earlyExpected, qint64 after) const
    {
        return manual && configured && active && acceptedAt > after && activeAt > after &&
               *manual == manualExpected && equal(*configured, value) && equal(*active, value) &&
               (!earlyExpected || (early && *early == *earlyExpected));
    }

    QString deploymentLock(const QString &command)
    {
        // GUI deployment and threshold writers use this same host lock. Other tools
        // must explicitly participate; no cross-process transaction is promised.
        return "flock -x .jetson-control-deployment.lock bash -c " + posixShellQuote(command);
    }
    QString atomicWrite(const QString &path, const QByteArray &contents, const QString &owner, const QString &group)
    {
        return QString(R"SH(
target=%1
test ! -L "$target" || { echo 'Refusing symlink target' >&2; exit 1; }
tmp=$(mktemp -- "${target}.gui.XXXXXX")
trap 'rm -f -- "$tmp"' EXIT
printf '%s' %2 | base64 --decode > "$tmp"
if test -e "$target"; then
  cp --attributes-only --preserve=mode,ownership -- "$target" "$tmp"
else
  chown -- %3:%4 "$tmp"
  chmod 0640 "$tmp"
fi
sync -f "$tmp"
mv -fT -- "$tmp" "$target"
sync -f "$(dirname -- "$target")"
trap - EXIT
)SH")
            .arg(posixShellQuote(path), posixShellQuote(QString::fromLatin1(contents.toBase64())),
                 posixShellQuote(owner), posixShellQuote(group));
    }
    QString checkedWrite(const Manifest &m, const QString &manifestPath, const QString &policyPath,
                         const QString &policyRevision, const QString &configPath, const QString &configRevision,
                         const QString &invocation, const QString &service, const QString &action,
                         const QString &number, bool early, const QString &owner, const QString &group)
    {
        QString script = "set -euo pipefail\n";
        script += QString("test \"$(systemctl show --value -p InvocationID %1)\" = %2 || { echo 'Service invocation changed' >&2; exit 1; }\n")
                      .arg(posixShellQuote(service), posixShellQuote(invocation));
        const auto checkHash = [](const QString &path, const QString &revision)
        {
            return QString("test \"$(sha256sum -- %1 | cut -d ' ' -f1)\" = %2 || { echo 'Stale model/revision; refresh required' >&2; exit 1; }\n")
                .arg(posixShellQuote(path), posixShellQuote(revision));
        };
        script += checkHash(manifestPath, m.revision) + checkHash(configPath, configRevision);
        if (!policyPath.isEmpty())
        {
            if (policyRevision == "missing")
                script += "test ! -e " + posixShellQuote(policyPath) + " || { echo 'Policy changed; refresh required' >&2; exit 1; }\n";
            else
                script += checkHash(policyPath, policyRevision);
        }
        // The second timestamp after both durable writes is the acknowledgment boundary.
        // The first timestamp is retained only for auditing partial write failures.
        script += "printf 'WRITE_TIME=%s\\n' \"$(date +%s%6N)\"\n";
        if (action == "checkbox" || action == "reset")
        {
            if (!policyPath.isEmpty())
            {
                script += atomicWrite(policyPath, m.policy(number, early), owner, group)
                              .replace("mv -fT -- \"$tmp\" \"$target\"", "mv -fT -- \"$tmp\" \"$target\"\necho POLICY_REPLACED=1");
                script += "echo POLICY_SAVED=1\n";
            }
            else if (action == "checkbox")
                fail("Policy mechanism is disabled");
        }
        if (action != "checkbox")
        {
            script += atomicWrite(manifestPath, m.patchManual(action == "reset" ? QString() : number), owner, group)
                          .replace("mv -fT -- \"$tmp\" \"$target\"", "mv -fT -- \"$tmp\" \"$target\"\necho MANIFEST_REPLACED=1");
            script += "echo MANIFEST_SAVED=1\n";
        }
        script += "printf 'WRITE_TIME=%s\\n' \"$(date +%s%6N)\"\n";
        return script;
    }
} // namespace thresholds
