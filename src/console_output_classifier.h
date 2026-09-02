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
