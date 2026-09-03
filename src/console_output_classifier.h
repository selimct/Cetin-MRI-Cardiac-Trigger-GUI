// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#pragma once

#include <QByteArray>

enum class ConsoleLineSeverity {
  informational,
  attention,
  elevated,
  unclassified,
};

enum class RuntimeInputEvent {
  none,
  paused,
  recovered,
  running,
};

[[nodiscard]] ConsoleLineSeverity classifyConsoleLine(const QByteArray &line);
[[nodiscard]] RuntimeInputEvent
runtimeInputEventForConsoleLine(const QByteArray &line);
[[nodiscard]] QByteArray serviceLogDisplayLine(const QByteArray &line);
