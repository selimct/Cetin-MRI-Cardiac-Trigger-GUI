// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#pragma once

#include "threshold_state.h"

namespace recovery
{
// Version 1 is also implemented by the optional native live-stack helper. Model
// capability and transport capability are deliberately independent.
inline constexpr auto schema = "dkfz-live-recovery-control-v1";
double threshold(const QString &text);
QString modelKind(const QJsonObject &source);
struct State
{
    thresholds::Manifest manifest;
    QJsonObject source, api;
    bool sourceAvailable{false}, sourceUnchanged{false}, primaryEdited{false};
    QString sourceRevision, warning;
    bool supportsRecovery() const;
    bool liveSupported() const;
    std::optional<double> calibrated() const;
    bool sourceEnabled() const;
    QString information() const;
    QString auditPolicy() const;
    QString requested() const;
    QString active() const;
    bool acceptApi(const QJsonObject &response);
};
// Head score and event origin have different meanings in the two telemetry
// schemas. Recovery scores refer to the previous frame, emissions to this one.
QString telemetryDescription(const QJsonObject &record);
} // namespace recovery
