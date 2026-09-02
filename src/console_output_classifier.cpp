#include "console_output_classifier.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

namespace {
QJsonObject structuredObject(const QByteArray &line) {
  const qsizetype objectStart = line.indexOf('{');
  if (objectStart < 0) {
    return {};
  }

  QJsonParseError parseError;
  const QJsonDocument document =
      QJsonDocument::fromJson(line.mid(objectStart).trimmed(), &parseError);
  if (parseError.error == QJsonParseError::NoError && document.isObject()) {
    return document.object();
  }
  return {};
}

QString displayTimestamp(const QByteArray &line, const QJsonObject &object) {
  const QString text = QString::fromUtf8(line);
  static const QRegularExpression journalTimestamp(
      QStringLiteral("^([A-Z][a-z]{2}\\s+\\d{1,2}\\s+\\d{2}:\\d{2}:\\d{2})"));
  const QRegularExpressionMatch match = journalTimestamp.match(text);
  if (match.hasMatch()) {
    return match.captured(1);
  }

  const QJsonValue realtime = object.value(QStringLiteral("realtime_ns"));
  if (!realtime.isDouble()) {
    return {};
  }
  const qint64 milliseconds = realtime.toInteger() / 1'000'000;
  return QDateTime::fromMSecsSinceEpoch(milliseconds)
      .toLocalTime()
      .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

QString integerValue(const QJsonObject &object, const char *name) {
  const QJsonValue value = object.value(QLatin1String(name));
  return value.isDouble() ? QString::number(value.toInteger()) : QString{};
}

QString jsonValueText(const QJsonValue &value) {
  if (value.isString()) {
    return value.toString();
  }
  if (value.isBool()) {
    return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
  }
  if (value.isDouble()) {
    const double number = value.toDouble();
    const qint64 integer = value.toInteger();
    if (number == static_cast<double>(integer)) {
      return QString::number(integer);
    }
    return QString::number(number, 'g', 12);
  }
  if (value.isNull()) {
    return QStringLiteral("null");
  }
  if (value.isArray()) {
    return QString::fromUtf8(
        QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
  }
  if (value.isObject()) {
    return QString::fromUtf8(
        QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
  }
  return {};
}

QString durationMilliseconds(const QString &nanoseconds) {
  bool ok = false;
  const qulonglong value = nanoseconds.toULongLong(&ok);
  if (!ok || value == 0) {
    return QStringLiteral("0");
  }
  QString milliseconds =
      QString::number(static_cast<double>(value) / 1e6, 'f', 3);
  while (milliseconds.endsWith(QLatin1Char('0'))) {
    milliseconds.chop(1);
  }
  if (milliseconds.endsWith(QLatin1Char('.'))) {
    milliseconds.chop(1);
  }
  return milliseconds;
}

QString runtimeStatusText(const QJsonObject &object) {
  QStringList sections;

  if (object.contains(QStringLiteral("input_paused"))) {
    sections.append(
        QStringLiteral("input=%1")
            .arg(object.value(QStringLiteral("input_paused")).toBool()
                     ? QStringLiteral("paused")
                     : QStringLiteral("running")));
  }

  const QString bytesRead = integerValue(object, "bytes_read");
  const QString decoded = integerValue(object, "samples_decoded");
  const QString consumed = integerValue(object, "samples_consumed");
  QStringList input;
  if (!bytesRead.isEmpty()) {
    input.append(QStringLiteral("bytes read=%1 B").arg(bytesRead));
  }
  if (!decoded.isEmpty()) {
    input.append(QStringLiteral("decoded=%1").arg(decoded));
  }
  if (!consumed.isEmpty()) {
    input.append(QStringLiteral("consumed=%1").arg(consumed));
  }
  if (!input.isEmpty()) {
    sections.append(input.join(QStringLiteral(", ")));
  }

  const QString frames = integerValue(object, "frames_inferred");
  const QString triggers = integerValue(object, "trigger_events");
  QStringList processing;
  if (!frames.isEmpty()) {
    processing.append(QStringLiteral("frames=%1").arg(frames));
  }
  if (!triggers.isEmpty()) {
    processing.append(QStringLiteral("triggers=%1").arg(triggers));
  }
  if (!processing.isEmpty()) {
    sections.append(processing.join(QStringLiteral(", ")));
  }

  const QString inputQueue = integerValue(object, "queue_depth");
  const QString outputQueue = integerValue(object, "output_queue_depth");
  const QString inputPeak = integerValue(object, "queue_high_water");
  const QString outputPeak = integerValue(object, "output_queue_high_water");
  if (!inputQueue.isEmpty() && !outputQueue.isEmpty()) {
    QString queues =
        QStringLiteral("queues in/out=%1/%2").arg(inputQueue, outputQueue);
    if (!inputPeak.isEmpty() && !outputPeak.isEmpty()) {
      queues += QStringLiteral(" (peak %1/%2)").arg(inputPeak, outputPeak);
    }
    sections.append(queues);
  }

  const QString outputMode =
      object.value(QStringLiteral("output_mode")).toString();
  const QString records = integerValue(object, "output_records");
  const QString pulses = integerValue(object, "gpio_pulses");
  QStringList outputCounts;
  if (!records.isEmpty()) {
    outputCounts.append(QStringLiteral("records=%1").arg(records));
  }
  if (!pulses.isEmpty()) {
    outputCounts.append(QStringLiteral("GPIO pulses=%1").arg(pulses));
  }
  if (!outputMode.isEmpty() || !outputCounts.isEmpty()) {
    QString output =
        QStringLiteral("output=%1")
            .arg(outputMode.isEmpty() ? QStringLiteral("unknown") : outputMode);
    if (!outputCounts.isEmpty()) {
      output +=
          QStringLiteral(" (%1)").arg(outputCounts.join(QStringLiteral(", ")));
    }
    sections.append(output);
  }

  const QString lastDuration =
      integerValue(object, "last_inference_duration_ns");
  const QString maxDuration = integerValue(object, "max_inference_duration_ns");
  if (!lastDuration.isEmpty()) {
    QString inference = QStringLiteral("inference=%1 ms")
                            .arg(durationMilliseconds(lastDuration));
    if (!maxDuration.isEmpty()) {
      inference +=
          QStringLiteral(" (max %1 ms)").arg(durationMilliseconds(maxDuration));
    }
    sections.append(inference);
  }

  const QString resets = integerValue(object, "input_state_resets");
  if (!resets.isEmpty() && resets != QStringLiteral("0")) {
    sections.append(QStringLiteral("input resets=%1").arg(resets));
  }

  QStringList problems;
  const auto addNonZero = [&object, &problems](const char *field,
                                               const QString &label) {
    const QString value = integerValue(object, field);
    if (!value.isEmpty() && value != QStringLiteral("0")) {
      problems.append(QStringLiteral("%1=%2").arg(label, value));
    }
  };
  addNonZero("invalid_samples", QStringLiteral("invalid samples"));
  addNonZero("malformed_groups", QStringLiteral("malformed groups"));
  addNonZero("bytes_discarded", QStringLiteral("discarded bytes"));
  if (!problems.isEmpty()) {
    sections.append(problems.join(QStringLiteral(", ")));
  }

  return sections.join(QStringLiteral(" | "));
}
} // namespace

ConsoleLineSeverity classifyConsoleLine(const QByteArray &line) {
  const QJsonObject object = structuredObject(line);
  if (!object.isEmpty()) {
    const QString code =
        object.value(QStringLiteral("code")).toString().toLower();
    if (code == QStringLiteral("input_paused")) {
      return ConsoleLineSeverity::attention;
    }

    const QString severity =
        object.value(QStringLiteral("severity")).toString().toLower();
    if (severity == QStringLiteral("debug") ||
        severity == QStringLiteral("info")) {
      return ConsoleLineSeverity::informational;
    }
    if (severity == QStringLiteral("warning") ||
        severity == QStringLiteral("error") ||
        severity == QStringLiteral("fatal")) {
      return ConsoleLineSeverity::elevated;
    }
  }

  const QString text = QString::fromUtf8(line);
  static const QRegularExpression elevatedPattern(
      QStringLiteral(
          "\\b(?:warn(?:ing)?|error|fatal|critical|failed|failure)\\b"),
      QRegularExpression::CaseInsensitiveOption);
  if (text.contains(elevatedPattern)) {
    return ConsoleLineSeverity::elevated;
  }

  static const QRegularExpression informationalPattern(
      QStringLiteral("\\b(?:debug|info)\\b"),
      QRegularExpression::CaseInsensitiveOption);
  if (text.contains(informationalPattern)) {
    return ConsoleLineSeverity::informational;
  }
  return ConsoleLineSeverity::unclassified;
}

RuntimeInputEvent runtimeInputEventForConsoleLine(const QByteArray &line) {
  const QJsonObject object = structuredObject(line);
  if (object.isEmpty()) {
    return RuntimeInputEvent::none;
  }

  const QString code =
      object.value(QStringLiteral("code")).toString().toLower();
  if (code == QStringLiteral("input_paused")) {
    return RuntimeInputEvent::paused;
  }
  if (code == QStringLiteral("input_signal_recovered")) {
    return RuntimeInputEvent::recovered;
  }
  if (code == QStringLiteral("runtime_status") &&
      object.contains(QStringLiteral("input_paused"))) {
    return object.value(QStringLiteral("input_paused")).toBool()
               ? RuntimeInputEvent::paused
               : RuntimeInputEvent::running;
  }
  return RuntimeInputEvent::none;
}

QByteArray serviceLogDisplayLine(const QByteArray &line) {
  const QJsonObject object = structuredObject(line);
  if (object.isEmpty()) {
    return line;
  }

  const QString schema = object.value(QStringLiteral("schema")).toString();
  const QString code = object.value(QStringLiteral("code")).toString();
  if (schema != QStringLiteral("dkfz-live-diagnostic-v1") && code.isEmpty()) {
    return line;
  }

  QString heading = displayTimestamp(line, object);
  if (!heading.isEmpty()) {
    heading += QLatin1Char(' ');
  }

  QString display;
  if (code.compare(QStringLiteral("runtime_status"), Qt::CaseInsensitive) ==
      0) {
    display = heading + QStringLiteral("runtime: ") + runtimeStatusText(object);
  } else {
    const QString severity =
        object.value(QStringLiteral("severity")).toString().toLower();
    const QString component =
        object.value(QStringLiteral("component")).toString();
    const QString message = object.value(QStringLiteral("message")).toString();

    display = heading;
    if (!severity.isEmpty()) {
      display += QStringLiteral("[%1] ").arg(severity);
    }
    if (!component.isEmpty()) {
      display += component + QLatin1Char('/');
    }
    display += code.isEmpty() ? QStringLiteral("diagnostic") : code;
    if (!message.isEmpty()) {
      display += QStringLiteral(": ") + message;
    }

    static const QSet<QString> metadata{
        QStringLiteral("schema"),       QStringLiteral("severity"),
        QStringLiteral("code"),         QStringLiteral("component"),
        QStringLiteral("monotonic_ns"), QStringLiteral("realtime_ns"),
        QStringLiteral("message")};
    QStringList context;
    for (auto iterator = object.constBegin(); iterator != object.constEnd();
         ++iterator) {
      if (!metadata.contains(iterator.key())) {
        context.append(QStringLiteral("%1=%2").arg(
            iterator.key(), jsonValueText(iterator.value())));
      }
    }
    if (!context.isEmpty()) {
      display += QStringLiteral(" | ") + context.join(QStringLiteral(", "));
    }
  }

  if (line.endsWith('\n')) {
    display += QLatin1Char('\n');
  }
  return display.toUtf8();
}
