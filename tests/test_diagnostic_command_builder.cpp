// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#include "diagnostic_command_builder.h"
#include "ssh_command_builder.h"

#include <QProcess>
#include <QStandardPaths>
#include <QTest>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

class DiagnosticCommandBuilderTest final : public QObject {
  Q_OBJECT

private slots:
  void delayedPredictionDiscovery();
  void validatesParameters();
  void resolvesActiveManifest();
  void buildsServiceUnitRepairCommand();
  void buildsPreparationCommand();
  void buildsReexportCommand();
  void buildsSafeDirectRuntimeCommand();
  void validatesInputRecoveryContract();
  void buildsAvailabilityProbe();
  void buildsNativeRuntimeRebuildCommand();
  void generatedCommandsParseAsBash();
  void rejectsFilesChangedDuringValidation();
  void preparationChecksTheCurrentServiceState();
};

void DiagnosticCommandBuilderTest::delayedPredictionDiscovery() {
  QJsonObject config{{"modelname","delayed"},{"raw_hz",400},{"frame_size_raw",4},{"window_size",100},
      {"prediction_mode","delayed_100hz"},{"prediction_delay_frames",1},{"recovery_threshold",QJsonValue::Null}};
  const auto line = [&](const QJsonObject &value) {
    return "__JCG_FINISHED_MODEL__=" + QByteArray("finished_models/ecg3/delayed_100hz").toBase64() +
        ":best:" + QJsonDocument(value).toJson(QJsonDocument::Compact).toBase64() + "\n";
  };
  auto models = parseFinishedModels(line(config));
  QCOMPARE(models.size(),1);
  QCOMPARE(models[0].directory,QString("finished_models/ecg3/delayed_100hz"));
  QCOMPARE(models[0].frameHz,100.0);
  QCOMPARE(models[0].predictionDelayFrames,1);
  QVERIFY(models[0].predictionDescription().contains("Previous-frame primary"));
  QVERIFY(models[0].predictionDescription().contains("window 1 s"));
  config["frame_size_raw"] = 8;
  QVERIFY(parseFinishedModels(line(config)).isEmpty());
  config["frame_size_raw"] = 4; config["recovery_head"] = true;
  QVERIFY(parseFinishedModels(line(config)).isEmpty());
  config["recovery_head"] = false; config["prediction_delay_frames"] = 0;
  QVERIFY(parseFinishedModels(line(config)).isEmpty());
}

void DiagnosticCommandBuilderTest::validatesParameters() {
  DiagnosticRuntimeOptions options{
      .modelName = QStringLiteral("target_r"),
      .modelDirectory = QStringLiteral("models/target_r"),
  };
  QVERIFY(validateDiagnosticOptions(options).isEmpty());

  options.modelName = QStringLiteral("not safe; reboot");
  QVERIFY(!validateDiagnosticOptions(options).isEmpty());
  options.modelName = QStringLiteral("target_r");

  options.preparationMode = QStringLiteral("somewhere-else");
  QVERIFY(!validateDiagnosticOptions(options).isEmpty());
  options.preparationMode = QStringLiteral("jetson-only");

  options.modelDirectory = QStringLiteral("../models/target_r");
  QVERIFY(!validateDiagnosticOptions(options).isEmpty());
  options.modelDirectory = QStringLiteral("models/target_r");

  options.sessionLogPath = QStringLiteral("../outside.log");
  QVERIFY(!validateDiagnosticOptions(options).isEmpty());
  options.sessionLogPath = QStringLiteral("output/session.log");
  QVERIFY(validateDiagnosticOptions(options).isEmpty());

  options.sessionLogPath = QStringLiteral(".");
  QVERIFY(!validateDiagnosticOptions(options).isEmpty());
  options.sessionLogPath.clear();

  options.outputMode = QStringLiteral("gpio");
  options.emitProbabilities = true;
  QVERIFY(!validateDiagnosticOptions(options).isEmpty());
}

void DiagnosticCommandBuilderTest::validatesInputRecoveryContract() {
  const DiagnosticRuntimeOptions options{
      .modelName = QStringLiteral("target_r"),
      .modelDirectory = QStringLiteral("models/target_r"),
  };
  const QString command = validateRuntimeCommand(options);
  QVERIFY(command.contains(QStringLiteral("input_reset_enabled")));
  QVERIFY(command.contains(QStringLiteral("ecg_baseline_reset_ms")));
  QVERIFY(command.contains(QStringLiteral("stuck_signal_reset_ms")));
  QVERIFY(command.contains(QStringLiteral("input_recovery_ms")));
  QVERIFY(command.contains(QStringLiteral("watchdog_enabled")));
  QVERIFY(command.contains(QStringLiteral("uart_timeout_ms")));
  QVERIFY(command.contains(QStringLiteral("sample_timeout_ms")));
}

void DiagnosticCommandBuilderTest::resolvesActiveManifest() {
  QCOMPARE(diagnosticManifestPath(), QStringLiteral("deploy/model/model.conf"));
}

void DiagnosticCommandBuilderTest::buildsServiceUnitRepairCommand() {
  const QString nonInteractive = serviceUnitRepairCommand(false);
  QVERIFY(nonInteractive.contains(QStringLiteral("sudo -n -- bash -c")));
  QVERIFY(nonInteractive.contains(
      QStringLiteral("deploy/dkfz-native-uart-live.service")));
  QVERIFY(nonInteractive.contains(QStringLiteral(
      "/etc/systemd/system/dkfz-native-uart-live.service")));
  QVERIFY(nonInteractive.contains(QStringLiteral("ln -sfnT")));
  QVERIFY(nonInteractive.contains(QStringLiteral("systemctl daemon-reload")));
  QVERIFY(nonInteractive.contains(QStringLiteral("Refusing to replace")));

  const QString withPassword = serviceUnitRepairCommand(true);
  QVERIFY(withPassword.contains(QStringLiteral("sudo -S -p '' -- bash -c")));
  QVERIFY(!withPassword.contains(QStringLiteral("sudo -n")));
}

void DiagnosticCommandBuilderTest::buildsPreparationCommand() {
  DiagnosticRuntimeOptions options{
      .modelName = QStringLiteral("target-r"),
      .modelDirectory = QStringLiteral("models_noCV/target-r"),
      .variant = QStringLiteral("tuned"),
  };
  QCOMPARE(
      prepareModelCommand(options),
      QStringLiteral("./prepare_model.sh 'target-r' --model-dir "
                     "'models_noCV/target-r' --variant 'tuned' --jetson-only"));

  options.preparationMode = QStringLiteral("complete");
  QCOMPARE(prepareModelCommand(options),
           QStringLiteral("./prepare_model.sh 'target-r' --model-dir "
                          "'models_noCV/target-r' --variant 'tuned'"));
}

void DiagnosticCommandBuilderTest::buildsSafeDirectRuntimeCommand() {
  const DiagnosticRuntimeOptions options{
      .modelName = QStringLiteral("target_r"),
      .modelDirectory = QStringLiteral("models/target_r"),
      .variant = QStringLiteral("best"),
      .outputMode = QStringLiteral("jsonl"),
      .sessionLogPath = QStringLiteral("output/live session.log"),
      .rawCapturePath = QStringLiteral("output/session.bin"),
      .emitProbabilities = true,
  };
  const QString command = startDirectRuntimeCommand(options);

  QVERIFY(command.contains(
      QStringLiteral("build_native/inference/native_uart_live_runtime")));
  QVERIFY(!command.contains(QStringLiteral("build_data_ingest")));
  QVERIFY(command.contains(QStringLiteral("deploy/model/model.conf")));
  QVERIFY(command.contains(QStringLiteral("--trigger-output 'jsonl'")));
  QVERIFY(command.contains(QStringLiteral("--emit-probabilities")));
  QVERIFY(command.contains(
      QStringLiteral("--capture-byte-file 'output/session.bin'")));
  QVERIFY(command.contains(
      QStringLiteral("session_log='output/live session.log'")));
  QVERIFY(command.contains(QStringLiteral("native_uart_live_runtime.pid")));
}

void DiagnosticCommandBuilderTest::buildsReexportCommand() {
  const DiagnosticRuntimeOptions options{
      .modelName = QStringLiteral("declared_name"),
      .modelDirectory = QStringLiteral("finished_models/folder with ' quote"),
      .variant = QStringLiteral("tuned"),
  };
  const auto command = reexportModelCommand(options);
  QVERIFY(command.contains(QStringLiteral("./prepare_model.sh 'declared_name'")));
  QVERIFY(command.contains(QStringLiteral("--variant 'tuned'")));
  QVERIFY(!command.contains(QStringLiteral("--jetson-only")));
  QVERIFY(!command.contains(QStringLiteral("--engine")));
  QVERIFY(command.contains(QStringLiteral("systemctl is-active")));
  QVERIFY(command.contains(QStringLiteral("native_uart_live_runtime.pid")));
}

void DiagnosticCommandBuilderTest::buildsAvailabilityProbe() {
  DiagnosticRuntimeOptions options{
      .modelName = QStringLiteral("target_r"),
      .modelDirectory = QStringLiteral("models_noCV/target_r"),
  };
  const QString command = availabilityProbeCommand(options);
  QVERIFY(command.contains(QStringLiteral("__JCG_SERVICE__")));
  QVERIFY(command.contains(QStringLiteral("__JCG_SERVICE_ACTIVE__")));
  QVERIFY(command.contains(QStringLiteral("__JCG_SERVICE_INPUT__")));
  QVERIFY(command.contains(QStringLiteral("__JCG_SERVICE_READY__")));
  QVERIFY(command.contains(QStringLiteral("__JCG_SERVICE_UNIT__")));
  QVERIFY(command.contains(QStringLiteral("__JCG_SERVICE_LINKED__")));
  QVERIFY(command.contains(QStringLiteral("__JCG_SERVICE_CONTRACT__")));
  QVERIFY(command.contains(QStringLiteral("__JCG_RUNTIME__")));
  QVERIFY(command.contains(QStringLiteral("__JCG_MANIFEST__")));
  QVERIFY(command.contains(QStringLiteral("__JCG_ENGINE__")));
  QVERIFY(command.contains(QStringLiteral("__JCG_SLOT_METADATA__")));
  QVERIFY(command.contains(QStringLiteral("__JCG_ACTIVE_SELECTION__")));
  QVERIFY(command.contains(QStringLiteral("__JCG_DIRECT_READY__")));
  QVERIFY(command.contains(QStringLiteral("__JCG_PREPARE_INPUTS__")));
  QVERIFY(command.contains(QStringLiteral("__JCG_REEXPORT_INPUTS__")));
  QVERIFY(command.contains(QStringLiteral("__JCG_FINISHED_MODEL__")));
  QVERIFY(command.contains(QStringLiteral("deploy/model/model.conf")));
  QVERIFY(
      command.contains(QStringLiteral("models_noCV/target_r/target_r.onnx")));
  QVERIFY(
      command.contains(QStringLiteral("deploy/dkfz-native-uart-live.service")));
  QVERIFY(command.contains(QStringLiteral("config/native_uart_live.conf")));
  QVERIFY(command.contains(QStringLiteral("ecg_baseline_reset_ms")));
  QVERIFY(command.contains(QStringLiteral("stuck_signal_reset_ms")));
  QVERIFY(command.contains(QStringLiteral("input_recovery_ms")));
  QVERIFY(command.contains(QStringLiteral("StatusText")));
  QVERIFY(command.contains(QStringLiteral(
      "ExecStart=/opt/dkfz-live/build_native/inference/"
      "native_uart_live_runtime")));
  QVERIFY(command.contains(QStringLiteral("run/build_native.sh")));
  QVERIFY(!command.contains(QStringLiteral("build_data_ingest")));
  QVERIFY(!command.contains(QStringLiteral("/etc/dkfz-live")));

  options.preparationMode = QStringLiteral("complete");
  const QString completeCommand = availabilityProbeCommand(options);
  QVERIFY(!completeCommand.contains(
      QStringLiteral("models_noCV/target_r/target_r.onnx")));
}

void DiagnosticCommandBuilderTest::buildsNativeRuntimeRebuildCommand() {
  const QString command = rebuildRuntimeCommand();
  QVERIFY(command.contains(QStringLiteral("build_native/CMakeCache.txt")));
  QVERIFY(command.contains(QStringLiteral("run/rebuild_native.sh")));
  QVERIFY(command.contains(QStringLiteral("run/build_native.sh")));
  QVERIFY(!command.contains(QStringLiteral("build_data_ingest")));
}

void DiagnosticCommandBuilderTest::generatedCommandsParseAsBash() {
  const QString bash = QStandardPaths::findExecutable(QStringLiteral("bash"));
  if (bash.isEmpty()) {
    QSKIP("bash is not available on this build host");
  }

  const DiagnosticRuntimeOptions options{
      .modelName = QStringLiteral("target_r"),
      .modelDirectory = QStringLiteral("models/target_r"),
      .outputMode = QStringLiteral("jsonl"),
      .sessionLogPath = QStringLiteral("output/live session.log"),
      .emitProbabilities = true,
  };
  const QStringList commands{
      prepareModelCommand(options),       validateRuntimeCommand(options),
      startDirectRuntimeCommand(options), stopDirectRuntimeCommand(),
      serviceUnitRepairCommand(false),    serviceUnitRepairCommand(true),
      availabilityProbeCommand(options), rebuildRuntimeCommand(),
      finishedModelsCommand(), reexportModelCommand(options), prepareStoppedModelCommand(options),
      deploymentStopCommand(false), deploymentStopCommand(true),
      deploymentStartCommand(false, QString(64,'a'), QString(64,'b'), QString(64,'c')),
  };

  for (const QString &command : commands) {
    QProcess process;
    process.start(bash, {QStringLiteral("-n")});
    QVERIFY2(process.waitForStarted(5000), qPrintable(process.errorString()));

    const QByteArray script = command.toUtf8();
    QCOMPARE(process.write(script), static_cast<qint64>(script.size()));
    process.closeWriteChannel();

    QVERIFY2(process.waitForFinished(5000), qPrintable(process.errorString()));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QVERIFY2(process.exitCode() == 0,
             qPrintable(QString::fromUtf8(process.readAllStandardError())));
  }
}

void DiagnosticCommandBuilderTest::rejectsFilesChangedDuringValidation() {
#ifdef Q_OS_WIN
  QSKIP("Linux fake host filesystem fixture");
#else
  for (const auto &changed : {"config/native_uart_live.conf", "deploy/model/model.conf", "build_native/inference/native_uart_live_runtime"}) {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto write = [&](const QString &path, const QByteArray &bytes, bool executable = false) {
      QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath())); QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
      QCOMPARE(file.write(bytes), bytes.size()); file.close();
      if (executable) QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    };
    write(dir.filePath("config/native_uart_live.conf"),
      "input_reset_enabled=true\necg_baseline_reset_ms=100\nstuck_signal_reset_ms=100\ninput_recovery_ms=100\nwatchdog_enabled=true\nuart_timeout_ms=100\nsample_timeout_ms=100\n");
    write(dir.filePath("deploy/model/model.conf"), "model_name=heart\nmodel_variant=best\nsource_model_dir=finished_models/heart\n");
    write(dir.filePath("deploy/model/model.engine"), "engine");
    write(dir.filePath("bin/systemctl"), "#!/bin/bash\nif [[ $1 == is-active ]]; then echo inactive; fi\nexit 1\n", true);
    write(dir.filePath("build_native/inference/native_uart_live_runtime"),
      "#!/bin/bash\nprintf '# changed while validating\\n' >> " + posixShellQuote(changed).toUtf8() + "\nexit 0\n", true);
    DiagnosticRuntimeOptions options{.modelName="heart",.modelDirectory="finished_models/heart"};
    QProcess process; auto env = QProcessEnvironment::systemEnvironment();
    env.insert("PATH", dir.filePath("bin") + ':' + env.value("PATH")); process.setProcessEnvironment(env);
    process.setWorkingDirectory(dir.path()); process.start("bash", {"-c", availabilityProbeCommand(options)});
    QVERIFY(process.waitForStarted()); QVERIFY(process.waitForFinished(10000));
    QVERIFY(process.exitCode() != 0);
    QVERIFY(process.readAllStandardError().contains("Deployment changed during validation"));
  }
#endif
}

void DiagnosticCommandBuilderTest::preparationChecksTheCurrentServiceState() {
#ifdef Q_OS_WIN
  QSKIP("Linux fake host process fixture");
#else
  QTemporaryDir dir; QVERIFY(dir.isValid());
  QVERIFY(QDir().mkpath(dir.filePath("bin")));
  QFile systemctl(dir.filePath("bin/systemctl")); QVERIFY(systemctl.open(QIODevice::WriteOnly));
  systemctl.write("#!/bin/bash\necho active\n"); systemctl.close();
  QVERIFY(systemctl.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
  QFile prepare(dir.filePath("prepare_model.sh")); QVERIFY(prepare.open(QIODevice::WriteOnly));
  prepare.write("#!/bin/bash\ntouch preparation-was-run\n"); prepare.close();
  QVERIFY(prepare.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
  DiagnosticRuntimeOptions options{.modelName="heart", .modelDirectory="finished_models/heart"};
  QProcess process; auto env=QProcessEnvironment::systemEnvironment(); env.insert("PATH",dir.filePath("bin")+':'+env.value("PATH"));
  process.setProcessEnvironment(env); process.setWorkingDirectory(dir.path());
  process.start("bash", {"-c", prepareStoppedModelCommand(options)});
  QVERIFY(process.waitForStarted()); QVERIFY(process.waitForFinished());
  QCOMPARE(process.exitCode(),3); QVERIFY(!QFile::exists(dir.filePath("preparation-was-run")));
#endif
}

QTEST_APPLESS_MAIN(DiagnosticCommandBuilderTest)

#include "test_diagnostic_command_builder.moc"
