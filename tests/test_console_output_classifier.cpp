// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#include "console_output_classifier.h"

#include <QTest>

class ConsoleOutputClassifierTest final : public QObject {
  Q_OBJECT

private slots:
  void classifiesStructuredSeverities();
  void treatsInputPauseAsOperationalAttention();
  void trustsStructuredSeverityOverMessageText();
  void classifiesPlainTextConservatively();
  void classifiesRuntimeInputEvents();
  void understandsJournalPrefixedDiagnostics();
  void compactsRuntimeStatusForServiceLogs();
  void compactsOtherStructuredServiceLogs();
  void preservesUnstructuredServiceLogs();
  void displaysSchedulingAndDeadlines();
  void preservesCandidateAndEmissionCoordinates();
};

void ConsoleOutputClassifierTest::classifiesStructuredSeverities() {
  QCOMPARE(classifyConsoleLine(
               R"({"schema":"dkfz-live-diagnostic-v1","severity":"debug"})"),
           ConsoleLineSeverity::informational);
  QCOMPARE(classifyConsoleLine(
               R"({"schema":"dkfz-live-diagnostic-v1","severity":"info"})"),
           ConsoleLineSeverity::informational);
  QCOMPARE(classifyConsoleLine(R"({"severity":"warning"})"),
           ConsoleLineSeverity::elevated);
  QCOMPARE(classifyConsoleLine(R"({"severity":"error"})"),
           ConsoleLineSeverity::elevated);
  QCOMPARE(classifyConsoleLine(R"({"severity":"fatal"})"),
           ConsoleLineSeverity::elevated);
}

void ConsoleOutputClassifierTest::treatsInputPauseAsOperationalAttention() {
  QCOMPARE(
      classifyConsoleLine(
          R"({"schema":"dkfz-live-diagnostic-v1","severity":"warning","code":"input_paused"})"),
      ConsoleLineSeverity::attention);
  QCOMPARE(classifyConsoleLine(
               R"({"severity":"warning","code":"uart_resynchronized"})"),
           ConsoleLineSeverity::elevated);
}

void ConsoleOutputClassifierTest::trustsStructuredSeverityOverMessageText() {
  QCOMPARE(classifyConsoleLine(
               R"({"severity":"info","message":"No fatal error occurred"})"),
           ConsoleLineSeverity::informational);
}

void ConsoleOutputClassifierTest::classifiesPlainTextConservatively() {
  QCOMPARE(classifyConsoleLine("WARNING: engine fallback\n"),
           ConsoleLineSeverity::elevated);
  QCOMPARE(classifyConsoleLine("Runtime information\n"),
           ConsoleLineSeverity::unclassified);
  QCOMPARE(classifyConsoleLine("Step 2 of 4\n"),
           ConsoleLineSeverity::unclassified);
}

void ConsoleOutputClassifierTest::classifiesRuntimeInputEvents() {
  QCOMPARE(runtimeInputEventForConsoleLine(
               R"({"code":"input_paused","severity":"warning"})"),
           RuntimeInputEvent::paused);
  QCOMPARE(runtimeInputEventForConsoleLine(
               R"({"code":"input_signal_recovered","severity":"info"})"),
           RuntimeInputEvent::recovered);
  QCOMPARE(runtimeInputEventForConsoleLine(
               R"({"code":"runtime_status","input_paused":true})"),
           RuntimeInputEvent::paused);
  QCOMPARE(runtimeInputEventForConsoleLine(
               R"({"code":"runtime_status","input_paused":false})"),
           RuntimeInputEvent::running);
  QCOMPARE(runtimeInputEventForConsoleLine("not structured"),
           RuntimeInputEvent::none);
}

void ConsoleOutputClassifierTest::understandsJournalPrefixedDiagnostics() {
  const QByteArray line =
      R"(Sep 02 09:27:16 orin dkfz-native-live[625]: {"severity":"warning","code":"input_paused"})";
  QCOMPARE(classifyConsoleLine(line), ConsoleLineSeverity::attention);
  QCOMPARE(runtimeInputEventForConsoleLine(line), RuntimeInputEvent::paused);
}

void ConsoleOutputClassifierTest::compactsRuntimeStatusForServiceLogs() {
  const QByteArray line =
      "Sep 02 09:27:16 orin dkfz-native-live[625]: "
      R"({"schema":"dkfz-live-diagnostic-v1","severity":"info","code":"runtime_status","component":"runtime","monotonic_ns":2060330588441,"realtime_ns":1788334036026251097,"message":"Integrated runtime health status","bytes_read":0,"samples_decoded":0,"samples_consumed":0,"frames_inferred":0,"trigger_events":0,"invalid_samples":0,"input_state_resets":1,"input_paused":true,"queue_depth":0,"queue_high_water":0,"output_mode":"gpio","output_records":0,"gpio_pulses":0,"output_queue_depth":0,"output_queue_high_water":0,"malformed_groups":0,"bytes_discarded":0,"last_inference_duration_ns":1234567,"max_inference_duration_ns":2500000})"
      "\n";

  QCOMPARE(
      serviceLogDisplayLine(line),
      QByteArray("Sep 02 09:27:16 runtime: input=paused | bytes read=0 "
                 "B, decoded=0, consumed=0 | frames=0, triggers=0 | queues "
                 "in/out=0/0 (peak 0/0) | output=gpio (records=0, GPIO "
                 "pulses=0) | inference=1.235 ms (max 2.5 ms) | input "
                 "resets=1\n"));
}

void ConsoleOutputClassifierTest::compactsOtherStructuredServiceLogs() {
  const QByteArray line =
      R"(Sep 02 09:28:16 orin unit[625]: {"schema":"dkfz-live-diagnostic-v1","severity":"error","code":"uart_failed","component":"input","message":"UART read failed","device":"/dev/ttyTHS1","attempt":3})"
      "\n";
  const QByteArray display = serviceLogDisplayLine(line);

  QVERIFY(display.startsWith(
      "Sep 02 09:28:16 [error] input/uart_failed: UART read failed | "));
  QVERIFY(display.contains("attempt=3"));
  QVERIFY(display.contains("device=/dev/ttyTHS1"));
  QVERIFY(!display.contains("monotonic_ns"));
  QVERIFY(!display.contains("dkfz-live-diagnostic-v1"));
}

void ConsoleOutputClassifierTest::preservesUnstructuredServiceLogs() {
  const QByteArray reboot = "-- Boot 1a2b3c4d --\n";
  QCOMPARE(serviceLogDisplayLine(reboot), reboot);
}

void ConsoleOutputClassifierTest::displaysSchedulingAndDeadlines() {
  const QByteArray worker = R"({"severity":"warning","code":"worker_scheduling","thread":"dkfz-inference","requested_realtime_priority":20,"policy":"other","priority":0,"realtime_applied":false,"degraded":true,"error":1,"prerequisite_error":"Missing bounded RT limit"})";
  const auto text = serviceLogDisplayLine(worker);
  for (const auto &part : {"thread=dkfz-inference", "policy=other", "priority=0", "requested_realtime_priority=20", "degraded=true", "realtime_applied=false", "error=1"})
    QVERIFY(text.contains(part));
  QCOMPARE(classifyConsoleLine(worker), ConsoleLineSeverity::elevated);
  const QByteArray health = R"({"severity":"info","code":"runtime_status","missed_frame_deadlines":6,"max_frame_end_to_completion_ns":23789000})";
  QVERIFY(serviceLogDisplayLine(health).contains("missed frame deadlines=6"));
  QVERIFY(serviceLogDisplayLine(health).contains("max frame delay=23.789 ms (host decode to completion)"));
  QCOMPARE(classifyConsoleLine(health), ConsoleLineSeverity::elevated);
  const auto zero = serviceLogDisplayLine(R"({"code":"runtime_status","missed_frame_deadlines":0,"max_frame_end_to_completion_ns":0})");
  QVERIFY(zero.contains("missed frame deadlines=0"));
  QVERIFY(zero.contains("max frame delay=0 ms"));
  QVERIFY(!serviceLogDisplayLine(R"({"code":"runtime_status"})").contains("deadlines=0"));
}

void ConsoleOutputClassifierTest::preservesCandidateAndEmissionCoordinates() {
  const QByteArray trigger = R"({"schema":"dkfz-live-trigger-v1","peak_frame_idx_50hz":99,"peak_sample_idx_400hz":396,"peak_time_ms":990,"emitted_on_frame_idx_50hz":101,"emitted_on_sample_idx_400hz":407,"emitted_on_time_ms":1017.5,"decision_monotonic_ns":12345,"gpio_asserted_monotonic_ns":null,"probability":0.9})";
  const auto text = serviceLogDisplayLine(trigger + '\n');
  QVERIFY(text.startsWith(trigger + " | ")); // Original coordinates and clocks survive byte for byte.
  QVERIFY(text.contains("candidate frame=99"));
  QVERIFY(text.contains("candidate time ms=990"));
  QVERIFY(text.contains("actual emission frame=101"));
  QVERIFY(text.contains("actual emission time ms=1017.5"));
  const QByteArray probability = R"({"schema":"dkfz-live-probability-v1","probability":0.9,"prediction_delay_frames":1,"frame_idx_50hz":99,"observed_frame_idx":100,"observed_time_ms":1000,"time_ms":990})";
  const auto delayed = serviceLogDisplayLine(probability);
  QVERIFY(delayed.startsWith(probability));
  QVERIFY(delayed.contains("previous-frame primary probability"));
  QVERIFY(delayed.contains("target frame=99; observed frame=100"));
  QVERIFY(delayed.contains("observed time ms=1000"));
}

QTEST_APPLESS_MAIN(ConsoleOutputClassifierTest)

#include "test_console_output_classifier.moc"
