// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#include "recovery_state.h"
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStringList>
#include <cmath>
#include <stdexcept>

namespace recovery
{
namespace
{
QString number(const QJsonValue &v)
{
    return v.isDouble() ? QString::number(v.toDouble(), 'g', 12) : "Unavailable";
}
QString boolean(const QJsonValue &v) { return v.isBool() ? (v.toBool() ? "yes" : "no") : "Unknown"; }
QString coordinate(const QJsonValue &v)
{
    if (!v.isDouble()) return "Unavailable";
    const auto integer = v.toInteger();
    return v.toDouble() == static_cast<double>(integer) ? QString::number(integer)
                                                       : QString::number(v.toDouble(), 'g', 17);
}
QString policy(const QJsonObject &p)
{
    if (!p.value("enabled").isBool())
        return "Unknown / unconfirmed";
    return p.value("enabled").toBool() ? "enabled; effective threshold " + number(p.value("threshold"))
                                       : "disabled; effective threshold null";
}
} // namespace
double threshold(const QString &text)
{
    const auto value = thresholds::probability(text);
    if (value <= 0)
        throw std::runtime_error("Recovery threshold must be finite and in (0, 1]");
    return value;
}
QString modelKind(const QJsonObject &source)
{
    if (!source.value("recovery_head").toBool(false))
        return "Normal (single output)";
    if (source.value("cv_method") == "nested_recovery_lopo")
        return "LOPO-recovery";
    if (!source.value("recovery_only").isBool())
        return "Recovery (training mode unknown)";
    return source.value("recovery_only").toBool() ? "Frozen-recovery" : "Joint-recovery";
}
bool State::supportsRecovery() const { return manifest.recoveryHead; }
bool State::liveSupported() const
{
    return api.value("schema") == schema && api.value("api_version").toInt() == 1 &&
           api.value("live_api_supported").toBool() && !api.value("revision").toString().isEmpty() &&
           QRegularExpression("^[0-9a-f]{32}$")
               .match(api.value("service_invocation_id").toString())
               .hasMatch();
}
std::optional<double> State::calibrated() const
{
    const auto value = source.value("recovery_threshold");
    if (!sourceAvailable || !value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() <= 0 ||
        value.toDouble() > 1)
        return {};
    return value.toDouble();
}
bool State::sourceEnabled() const
{
    return sourceAvailable && source.value("recovery_head").toBool(false) &&
           source.value("recovery_enabled").toBool(true) && calibrated().has_value();
}
bool State::acceptApi(const QJsonObject &response)
{
    const auto reject = [this](const QString &message)
    {
        api = {};
        warning = message;
        return false;
    };
    const auto validProbability = [](const QJsonValue &value)
    {
        return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() > 0 &&
               value.toDouble() <= 1;
    };
    const auto validPolicy = [&](const QJsonObject &value)
    {
        return value.value("enabled").isBool() &&
               (value.value("enabled").toBool()
                    ? supportsRecovery() && validProbability(value.value("threshold"))
                    : value.value("threshold").isNull());
    };
    if (response.value("schema") != schema || !response.value("api_version").isDouble() ||
        response.value("api_version").toDouble() != 1 ||
        response.value("identity").toObject() != manifest.identity() ||
        !response.value("model_supports_recovery").isBool() ||
        response.value("model_supports_recovery").toBool() != supportsRecovery())
        return reject("Live recovery unavailable: unsupported API version or deployment identity mismatch");
    const auto selected = response.value("selected_threshold");
    if (!response.value("live_api_supported").isBool() ||
        !QRegularExpression("^[0-9a-f]{32}$")
             .match(response.value("service_invocation_id").toString())
             .hasMatch() ||
        response.value("revision").toString().isEmpty() ||
        response.value("request_id").toString().isEmpty() ||
        response.value("confirmation").toString().isEmpty() ||
        (!selected.isNull() && !validProbability(selected)) || !response.value("requested").isObject() ||
        !validPolicy(response.value("requested").toObject()) || !response.value("active").isObject())
        return reject("Unconfirmed — malformed recovery request or service identity");
    const auto active = response.value("active").toObject();
    const auto requested = response.value("requested").toObject();
    // On restart, request 0 uses the manifest startup policy while the helper
    // retains a previous invocation's numeric selection for a future request.
    if (response.value("request_id") != "0" && requested.value("enabled").toBool() &&
        selected != requested.value("threshold"))
        return reject("Unconfirmed — selected/requested recovery threshold mismatch");
    if (!active.isEmpty() && (!validPolicy(active) || active.value("request_id").toString().isEmpty()))
        return reject("Unconfirmed — malformed active recovery readback");
    if (response.value("confirmation") == "confirmed")
    {
        if (active.isEmpty() || !response.value("live_api_supported").toBool())
            return reject("Unconfirmed — active recovery readback unavailable");
        if (active.value("request_id") != response.value("request_id") ||
            active.value("enabled") != response.value("requested").toObject().value("enabled") ||
            active.value("threshold") != response.value("requested").toObject().value("threshold"))
            return reject("Unconfirmed — recovery request/readback mismatch");
    }
    api = response;
    warning.clear();
    return true;
}

QString State::requested() const
{
    QString saved = "Persistent startup policy: ";
    const auto value = manifest.values.value("recovery_threshold");
    saved += supportsRecovery() && !value.isEmpty() ? "enabled; threshold " + value : "disabled";
    if (api.isEmpty())
        return saved + "; live request unavailable";
    return saved + "\nSelected recovery threshold (retained across restarts): " + number(api.value("selected_threshold")) +
           (api.value("request_id") == "0" ? "\nLive requested startup policy (no invocation override): "
                                          : "\nLive override for this service invocation: ") +
           policy(api.value("requested").toObject());
}
QString State::active() const
{
    if (api.isEmpty())
        return "Unknown / unconfirmed — deployed manifest and source JSON are saved settings";
    const auto active = api.value("active").toObject();
    return policy(active) + "; " + api.value("confirmation").toString("unconfirmed") +
           "; active request " + active.value("request_id").toString("unavailable") +
           "; request " + api.value("request_id").toString("unavailable");
}
QString State::auditPolicy() const
{
    QStringList changes;
    if (primaryEdited)
        changes << "primary settings requested in this session";
    if (sourceAvailable)
    {
        const auto primary = api.value("primary_policy").toObject();
        const auto actualPrimary = primary.value("threshold");
        const auto savedPrimary = source.value("manual_threshold").isDouble()
                                      ? source.value("manual_threshold")
                                      : source.value("threshold");
        const auto configuredPrimary = manifest.manual.value_or(manifest.automatic);
        if (savedPrimary.isDouble() &&
            (!thresholds::equal(savedPrimary.toDouble(), configuredPrimary) ||
             (actualPrimary.isDouble() &&
              !thresholds::equal(savedPrimary.toDouble(), actualPrimary.toDouble()))))
            changes << "primary threshold differs";
        if (source.value("threshold_calibration_early_emission").isBool() &&
            primary.value("early_emission").isBool() &&
            source.value("threshold_calibration_early_emission") != primary.value("early_emission"))
            changes << "early-emission policy differs from saved primary calibration mode";
        bool ok = false;
        const auto holdoff = manifest.values.value("holdoff_frames").toDouble(&ok);
        if (ok && source.value("holdoff_frames").isDouble() &&
            !thresholds::equal(holdoff, source.value("holdoff_frames").toDouble()))
            changes << "holdoff differs";
        QJsonObject requested = api.value("requested").toObject();
        if (requested.isEmpty())
        {
            const auto text = manifest.values.value("recovery_threshold");
            requested["enabled"] = supportsRecovery() && !text.isEmpty();
            requested["threshold"] = text.isEmpty() ? QJsonValue() : QJsonValue(text.toDouble());
        }
        if (requested.value("enabled").isBool() && requested.value("enabled").toBool() != sourceEnabled())
            changes << "recovery enabled policy differs";
        if (requested.value("threshold").isDouble() &&
            (!calibrated() || !thresholds::equal(requested.value("threshold").toDouble(), *calibrated())))
            changes << "recovery threshold differs";
        const auto selected = api.value("selected_threshold");
        if (selected.isDouble() && (!calibrated() || !thresholds::equal(selected.toDouble(), *calibrated())))
            changes << "selected recovery threshold customized";
    }
    return changes.isEmpty() ? "Calibration audit applicability unconfirmed — complete original runtime "
                               "policy is not established"
                             : "Policy customized — " + changes.join("; ") +
                                   ". Saved calibration audit may be inapplicable";
}
QString State::information() const
{
    if (manifest.contents.isEmpty())
        return "Refresh to read the remote deployment";
    const bool delayedPrimary = manifest.values.value("prediction_mode", "current") == "delayed_100hz";
    QStringList lines;
    lines << auditPolicy();
    lines << (supportsRecovery()
                  ? "Two outputs: shared LSTM; current frame and previous frame using current context"
                  : "Recovery unavailable: single-output model");
    bool a = false, b = false;
    const auto frame = manifest.values.value("frame_size_raw").toDouble(&a);
    const auto hz = manifest.values.value("raw_hz").toDouble(&b);
    if (delayedPrimary || supportsRecovery())
        lines << (a && b && std::isfinite(frame) && std::isfinite(hz) && frame > 0 && hz > 0
                      ? QString(delayedPrimary ? "Previous-frame primary: one-frame lookahead = %1 ms"
                                               : "Recovered events only: one-frame lookahead = %1 ms")
                            .arg(1000 * frame / hz, 0, 'g', 12)
                      : delayedPrimary ? "Primary lookahead duration unavailable"
                                       : "Recovery lookahead duration unavailable");
    else
        lines << "Current-frame primary: no configured lookahead";
    if (!sourceAvailable)
    {
        lines << "Source calibration/provenance unavailable";
        return lines.join('\n');
    }
    lines << (sourceUnchanged ? "Saved source snapshot from the deployed source directory; byte identity at "
                                "deployment is unverified (not runtime confirmation)"
                              : "Source metadata unconfirmed: it may have changed since deployment");
    lines << "Model kind: " + modelKind(source);
    if (supportsRecovery())
    {
        if (source.value("recovery_threshold").isNull())
            lines << "Recovery disabled by calibration";
        else
            lines << "Calibrated recovery threshold: " + number(source.value("recovery_threshold"));
        lines << "Source default enabled: " + QString(sourceEnabled() ? "yes" : "no");
    }
    if (!supportsRecovery())
        lines << (delayedPrimary ? "Training: single-output previous-frame primary"
                                 : "Training: single-output current-frame primary");
    else if (source.value("recovery_only").isBool())
        lines << (source.value("recovery_only").toBool()
                      ? "Training: recovery only; primary LSTM/current head declared frozen"
                      : "Training: joint; no equivalence guarantee to an independent single-output model");
    else
        lines << "Training mode: unknown";
    lines << "Primary weights declared frozen: " + boolean(source.value("primary_weights_frozen"));
    lines << "Primary weights verified unchanged against source: " +
                 boolean(source.value("primary_weights_verified_unchanged"));
    lines << "Primary threshold fixed: " + boolean(source.value("primary_threshold_fixed"));
    if (source.value("cv_method") == "nested_recovery_lopo")
    {
        lines << "Recovery-head nested LOPO: conditional on the frozen pretrained base";
        lines << "Outer patients excluded from recovery-head training/selection; base/preprocessing may have "
                 "seen them. Entire detector is not established as patient-unseen.";
    }
    lines << "CV-selected fixed epoch: " + number(source.value("selected_fixed_epoch"));
    lines << "Saved training length (not inferred best epoch): " + number(source.value("num_epochs"));
    if (source.contains("evaluation_scope"))
        lines << "Evaluation scope: " + source.value("evaluation_scope").toString();
    lines << "Base patient holdout verified: " + boolean(source.value("base_patient_holdout_verified"));
    for (const auto &key :
         {"recovery_selection_objective", "source_model_name", "source_config", "source_checkpoint"})
        if (source.contains(key))
            lines << QString(key) + ": " + source.value(key).toString();
    if (source.contains("source_checkpoint_sha256"))
        lines << "Frozen base checkpoint SHA256 (not deployed engine or recovery checkpoint): " +
                     source.value("source_checkpoint_sha256").toString();
    if (source.value("recovery_max_fp_increase").isDouble())
        lines << QString("Calibration FP allowance: %1% relative; not a live FP guarantee")
                     .arg(100 * source.value("recovery_max_fp_increase").toDouble(), 0, 'g', 8);
    else
        lines << "Calibration FP allowance: unavailable";
    const auto audit = source.value("recovery_validation_metrics").toObject();
    if (audit.isEmpty())
        lines << "Calibration audit unavailable";
    else
    {
        lines << "Saved calibration results (not outer-test or live performance):";
        lines << "Ordinary detector audit baseline FP: " + number(audit.value("baseline_fp")) +
                     "; baseline FN: " + number(audit.value("baseline_fn"));
        lines << QString::fromUtf8(QJsonDocument(audit).toJson(QJsonDocument::Indented));
        const auto early = audit.value("early_emission").toObject();
        if (early.isEmpty())
            lines << "Early-emission calibration audit unavailable";
        else
            lines << "Early-emission audit — emission threshold: " +
                         number(early.value("emission_threshold")) +
                         "; FP budget: " + number(early.value("fp_budget")) +
                         "; baseline FP: " + number(early.value("baseline_fp")) +
                         "; baseline FN: " + number(early.value("baseline_fn"));
    }
    lines << "Audit applicability unconfirmed / policy customized if primary threshold, early emission, "
             "holdoff or recovery threshold differs. Runtime changes do not recalibrate or enforce the FP "
             "allowance.";
    if (supportsRecovery())
        lines << "Starting current-only reproduces a frozen primary detector only with the same preprocessing "
                 "and runtime settings. Live disabling cannot undo emitted recovery events or shared holdoff.";
    return lines.join('\n');
}
QString telemetryDescription(const QJsonObject &e)
{
    const auto schema = e.value("schema").toString();
    if (schema == "dkfz-live-probability-v1")
    {
        QString text = (e.value("prediction_delay_frames").toInt() == 1
            ? "previous-frame primary probability=" : "current-head probability=") + number(e.value("probability"));
        if (e.value("recovery_prob").isDouble())
            text += "; previous-frame recovery probability=" + number(e.value("recovery_prob"));
        if (e.value("recovery_emitted_now").isBool())
            text += "; recovery emitted on this record=" + boolean(e.value("recovery_emitted_now"));
        if (e.value("prediction_delay_frames").toInt() == 1) {
            text += "; target frame=" + coordinate(e.value("frame_idx_50hz")) +
                "; observed frame=" + coordinate(e.value("observed_frame_idx")) +
                "; observed time ms=" + coordinate(e.value("observed_time_ms"));
        }
        return text;
    }
    if (schema == "dkfz-live-trigger-v1")
        return "accepted event score=" + number(e.value("probability")) +
               "; candidate frame=" + coordinate(e.value("peak_frame_idx_50hz")) +
               "; candidate raw sample=" + coordinate(e.value("peak_sample_idx_400hz")) +
               "; candidate time ms=" + coordinate(e.value("peak_time_ms")) +
               "; actual emission frame=" + coordinate(e.value("emitted_on_frame_idx_50hz")) +
               "; actual emission raw sample=" + coordinate(e.value("emitted_on_sample_idx_400hz")) +
               "; actual emission time ms=" + coordinate(e.value("emitted_on_time_ms")) +
               "; head origin unknown (requires explicit origin or matching probability record)";
    return {};
}
} // namespace recovery
