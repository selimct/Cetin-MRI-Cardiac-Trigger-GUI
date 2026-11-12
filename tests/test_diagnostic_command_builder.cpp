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
#include <QCryptographicHash>
#ifndef Q_OS_WIN
#include <unistd.h>
#endif

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
  void rejectsUnsafePreparationInputs();
  void buildsFingerprintedActivationCommand();
  void serviceSchedulingProfiles();
  void automaticInstallationKeepsValidationAndSeparateServiceStart();
  void directStartChecksServiceAndSerializesLaunch();
  void restartRejectsStaleDeployment();
  void recognizesInstalledSnapshotAfterReopening();
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
  config["prediction_delay_frames"] = 1;
  config["holdoff_frames"] = 4;
  models = parseFinishedModels(line(config));
  QVERIFY(models[0].predictionDescription().contains("holdoff 40 ms"));
  config["prediction_mode"] = "current"; config["prediction_delay_frames"] = 0;
  config["frame_size_raw"] = 8; config["window_size"] = 50;
  models = parseFinishedModels(line(config));
  QVERIFY(models[0].predictionDescription().contains("50 Hz; window 1 s; holdoff 80 ms"));
  config.remove("holdoff_frames"); config["holdoff_ms"] = 201;
  models = parseFinishedModels(line(config));
  QVERIFY(models[0].predictionDescription().contains("holdoff 220 ms"));
  config["frame_size_raw"] = 4;
  config["prediction_mode"] = "delayed_100hz"; config["prediction_delay_frames"] = 1;
  models = parseFinishedModels(line(config));
  QVERIFY(models[0].predictionDescription().contains("holdoff 210 ms"));
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
  options.bundleDirectory = "artifact/bundle 1";
  options.validationInputs = {"data/one.parquet", "data/two.parquet"};
  options.ecg3SourceConfirmed = true;
  const auto command = prepareModelCommand(options);
  QVERIFY(command.startsWith("bash ./prepare_model.sh 'target-r' --model-dir 'models_noCV/target-r' --variant 'tuned'"));
  QVERIFY(command.contains("--prepare-only --output-dir 'artifact/bundle 1'"));
  QVERIFY(command.contains("--ecg3-source ecg2_minus_ecg1"));
  QVERIFY(command.contains("--validation-input 'data/one.parquet' --validation-input 'data/two.parquet'"));
  QVERIFY(!command.contains("run/deploy_model.sh"));

  options.preparationMode = QStringLiteral("complete");
  QCOMPARE(prepareModelCommand(options), command);
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
  auto prepared = options;
  prepared.bundleDirectory = "artifact/quoted ' bundle";
  prepared.validationInputs = {"data/recording ' one.parquet", "data/two.parquet"};
  prepared.ecg3SourceConfirmed = true;
  const auto command = reexportModelCommand(prepared);
  QVERIFY(command.contains(QStringLiteral("./prepare_model.sh 'declared_name'")));
  QVERIFY(command.contains(QStringLiteral("--variant 'tuned'")));
  QVERIFY(command.contains(QStringLiteral("--prepare-only")));
  QVERIFY(command.contains(QStringLiteral("--max-stream-samples 0")));
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
      command.contains(QStringLiteral("models_noCV/target_r/best.pth")));
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
      .bundleDirectory = "artifact/quoted ' bundle",
      .validationInputs = {"data/one ' recording.parquet", "data/two.parquet"},
      .ecg3SourceConfirmed = true,
  };
  const QStringList commands{
      prepareModelCommand(options),       validateRuntimeCommand(options),
      startDirectRuntimeCommand(options), stopDirectRuntimeCommand(),
      serviceUnitRepairCommand(false),    serviceUnitRepairCommand(true),
      availabilityProbeCommand(options), rebuildRuntimeCommand(),
      finishedModelsCommand(), reexportModelCommand(options), prepareStoppedModelCommand(options),
      activatePreparedModelCommand(options),
      installModelCommand(options),
      deploymentStopCommand(false), deploymentStopCommand(true),
      deploymentStartCommand(false, QString(64,'a'), QString(64,'b'), QString(64,'c')),
      deploymentRestartCommand(false, QString(64,'a'), QString(64,'b'), QString(64,'c')),
      deploymentRestartCommand(true, QString(64,'a'), QString(64,'b'), QString(64,'c')),
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

void DiagnosticCommandBuilderTest::rejectsUnsafePreparationInputs() {
  DiagnosticRuntimeOptions options{.modelName="heart", .modelDirectory="finished_models/heart",
      .bundleDirectory="artifact/run", .validationInputs={"data/one.parquet"}, .ecg3SourceConfirmed=true};
  QVERIFY(validatePreparationOptions(options).isEmpty());
  options.ecg3SourceConfirmed = false;
  QVERIFY(!validatePreparationOptions(options).isEmpty());
  QVERIFY(!prepareModelCommand(options).contains("prepare_model.sh"));
  QVERIFY(!activatePreparedModelCommand(options).contains("deploy_model.sh"));
  options.ecg3SourceConfirmed = true;
  for (const auto &path : {"../outside", "/absolute", "deploy", "artifact/../deploy/run", ""}) {
    options.bundleDirectory = path;
    QVERIFY(!validatePreparationOptions(options).isEmpty());
  }
  options.bundleDirectory = "artifact/run";
  options.validationInputs = {"../outside.parquet"};
  QVERIFY(!validatePreparationOptions(options).isEmpty());
  options.validationInputs.clear();
  QVERIFY(!validatePreparationOptions(options).isEmpty());
}

void DiagnosticCommandBuilderTest::buildsFingerprintedActivationCommand() {
  DiagnosticRuntimeOptions options{.modelName="delayed", .modelDirectory="finished_models/heart/delayed_100hz",
      .variant="tuned", .forceRecoveryOff=true, .bundleDirectory="artifact/quoted ' bundle",
      .validationInputs={"data/one.parquet"}, .ecg3SourceConfirmed=true};
  const auto command = activatePreparedModelCommand(options);
  QVERIFY(command.contains("bash run/deploy_model.sh 'delayed' --variant 'tuned'"));
  for (const auto &path : {"/model/train_config.json", "/model/best.pth", "/model/model.engine", "/validation_report.json"})
    QVERIFY(command.contains(posixShellQuote(options.bundleDirectory + path)));
  QVERIFY(command.contains("--validated-report"));
  QVERIFY(command.contains("--no-recovery"));
  QVERIFY(command.contains("systemctl is-active"));
  QVERIFY(!command.contains("systemctl start"));
  QVERIFY(!command.contains("systemctl stop"));
}

void DiagnosticCommandBuilderTest::serviceSchedulingProfiles() {
#ifdef Q_OS_WIN
  QSKIP("Linux fake host filesystem fixture");
#else
  for (const auto &profile : {"legacy", "bounded", "unbounded", "whole-process-rt", "unexpected-flags", "old-runtime"}) {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto write = [&](const QString &path, const QByteArray &bytes, bool executable = false) {
      const auto full = dir.filePath(path);
      QVERIFY(QDir().mkpath(QFileInfo(full).absolutePath()));
      QFile file(full); QVERIFY(file.open(QIODevice::WriteOnly));
      QCOMPARE(file.write(bytes), bytes.size()); file.close();
      if (executable) QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    };
    QByteArray unit = "ExecStart=/opt/dkfz-live/build_native/inference/native_uart_live_runtime --config /opt/dkfz-live/config/native_uart_live.conf";
    if (QString(profile) != "legacy") unit += " --realtime-priority 20 --require-realtime";
    if (QString(profile) == "unexpected-flags") unit += " --other-flag";
    write("deploy/dkfz-native-uart-live.service", unit + '\n');
    write("profile", profile);
    write("build_native/inference/native_uart_live_runtime",
          QString(profile) == "old-runtime" ? "#!/bin/bash\necho old runtime\n" :
          "#!/bin/bash\necho --realtime-priority --require-realtime\n", true);
    write("bin/systemctl", R"SH(#!/bin/bash
if [[ $1 == is-active ]]; then echo inactive; exit 3; fi
if [[ $1 != show ]]; then exit 1; fi
case "$*" in
 *LimitRTPRIO*) echo 20;;
 *LimitRTTIME*) if [[ $(cat profile) == unbounded ]]; then echo infinity; else echo 200000; fi;;
 *CPUSchedulingPolicy*) if [[ $(cat profile) == whole-process-rt ]]; then echo 2; else echo 0; fi;;
 *CPUWeight*|*TimerSlackNSec*) echo 1000;;
esac
)SH", true);
    DiagnosticRuntimeOptions options{.modelName="heart", .modelDirectory="finished_models/heart"};
    QProcess process; auto env=QProcessEnvironment::systemEnvironment();
    env.insert("PATH", dir.filePath("bin") + ':' + env.value("PATH")); process.setProcessEnvironment(env);
    process.setWorkingDirectory(dir.path()); process.start("bash", {"-c", availabilityProbeCommand(options)});
    QVERIFY(process.waitForStarted()); QVERIFY(process.waitForFinished(10000));
    QCOMPARE(process.exitCode(), 0);
    const auto expected = QString(profile) == "legacy" ? "legacy" : QString(profile) == "bounded" ? "bounded_rr20" : "unknown";
    QVERIFY(process.readAllStandardOutput().contains(QByteArray("__JCG_SCHEDULING_PROFILE__=") + expected));
  }
#endif
}

void DiagnosticCommandBuilderTest::automaticInstallationKeepsValidationAndSeparateServiceStart() {
  DiagnosticRuntimeOptions options{.modelName="heart", .modelDirectory="finished_models/heart",
      .bundleDirectory="build_native/prepared_models/gui-run", .ecg3SourceConfirmed=true,
      .autoValidationInputs=true};
  QVERIFY(validatePreparationOptions(options).isEmpty());
  const auto command = installModelCommand(options);
  QVERIFY(command.contains("data/live/p12/p12_out.parquet"));
  QVERIFY(command.contains("data/live/p13/p13_out.parquet"));
  QVERIFY(command.contains("No standard parity recordings found"));
  QVERIFY(command.contains("--prepare-only"));
  QVERIFY(command.contains("--validated-report"));
  QVERIFY(command.contains("|| exit $?"));
  QVERIFY(command.indexOf("__JCG_INSTALL_STAGE__=preparation") < command.indexOf("__JCG_INSTALL_STAGE__=activation"));
  QVERIFY(command.indexOf("__JCG_INSTALL_STAGE__=activation") < command.indexOf("__JCG_INSTALL_STAGE__=verification"));
  QVERIFY(!command.contains("systemctl start"));
  QVERIFY(!command.contains("systemctl stop"));
  const auto bash = QStandardPaths::findExecutable("bash");
  if (!bash.isEmpty()) {
    QProcess process;
    process.start(bash, {"-n"}); QVERIFY(process.waitForStarted());
    process.write(command.toUtf8()); process.closeWriteChannel();
    QVERIFY(process.waitForFinished()); QCOMPARE(process.exitCode(), 0);
  }
}

namespace {
void writeFixture(const QString &path, const QByteArray &bytes) {
  QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
  QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
  QCOMPARE(file.write(bytes), bytes.size()); file.close();
  QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
}
QProcessEnvironment fixtureEnvironment(const QTemporaryDir &dir) {
  auto env = QProcessEnvironment::systemEnvironment();
  env.insert("PATH", dir.filePath("bin") + ':' + env.value("PATH"));
  env.insert("XDG_RUNTIME_DIR", dir.path());
  return env;
}
}

void DiagnosticCommandBuilderTest::directStartChecksServiceAndSerializesLaunch() {
#ifdef Q_OS_WIN
  QSKIP("Linux fake host process fixture");
#else
  QTemporaryDir dir; QVERIFY(dir.isValid());
  writeFixture(dir.filePath("bin/systemctl"), R"SH(#!/bin/bash
case "$1" in
 is-active) cat service-state; [[ $(cat service-state) == active ]];;
 show) echo loaded;;
esac
)SH");
  writeFixture(dir.filePath("build_native/inference/native_uart_live_runtime"), R"SH(#!/bin/bash
echo launched >> launches
while ! test -f finish; do sleep 0.02; done
)SH");
  DiagnosticRuntimeOptions options{.modelName="heart", .modelDirectory="finished_models/heart"};
  const auto run = [&](QProcess &process) {
    process.setProcessEnvironment(fixtureEnvironment(dir));
    process.setWorkingDirectory(dir.path());
    process.start("bash", {"-c", startDirectRuntimeCommand(options)});
    QVERIFY(process.waitForStarted());
  };
  for (const auto &state : {"active", "activating", "reloading", "deactivating", "unknown"}) {
    writeFixture(dir.filePath("service-state"), state);
    QProcess process; run(process); QVERIFY(process.waitForFinished(5000));
    QCOMPARE(process.exitCode(), 3);
    QVERIFY(!QFile::exists(dir.filePath("launches")));
  }
  writeFixture(dir.filePath("service-state"), "inactive");
  // A deployment already holding the lock must finish before launch can proceed.
  QProcess holder;
  holder.setWorkingDirectory(dir.path());
  holder.start("bash", {"-c", "exec 9>.jetson-control-deployment.lock; flock -x 9; echo locked; read -r release"});
  QVERIFY(holder.waitForReadyRead()); QVERIFY(holder.readAllStandardOutput().contains("locked"));
  QProcess first; run(first);
  QTest::qWait(100); QVERIFY(!QFile::exists(dir.filePath("launches")));
  holder.write("release\n"); QVERIFY(holder.waitForFinished(5000));
  QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(dir.filePath("launches")), 5000);
  // Lock was released after PID publication, but that live PID excludes a
  // second client until the first runtime exits.
  QProcess second; run(second); QVERIFY(second.waitForFinished(5000));
  QCOMPARE(second.exitCode(), 73);
  QFile launches(dir.filePath("launches")); QVERIFY(launches.open(QIODevice::ReadOnly));
  QCOMPARE(launches.readAll(), QByteArray("launched\n"));
  writeFixture(dir.filePath("finish"), "done");
  QVERIFY(first.waitForFinished(5000)); QCOMPARE(first.exitCode(), 0);
  QVERIFY(!QFile::exists(dir.filePath("dkfz-jetson-control-" + QString::number(getuid()) + "/native_uart_live_runtime.pid")));
#endif
}

void DiagnosticCommandBuilderTest::restartRejectsStaleDeployment() {
#ifdef Q_OS_WIN
  QSKIP("Linux fake host filesystem fixture");
#else
  for (const auto &scenario : {"valid", "manifest", "config", "runtime", "stale-unit", "wrong-bind", "direct"}) {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    writeFixture(dir.filePath("scenario"), scenario);
    writeFixture(dir.filePath("bin/sudo"), "#!/bin/bash\nshift 2\nexec \"$@\"\n");
    writeFixture(dir.filePath("bin/systemctl"), R"SH(#!/bin/bash
case "$*" in
 *NeedDaemonReload*) if [[ $(cat scenario) == stale-unit ]]; then echo yes; else echo no; fi;;
 *FragmentPath*) echo "$PWD/deploy/dkfz-native-uart-live.service";;
 *WorkingDirectory*) echo /opt/dkfz-live;;
 *RootDirectory*) echo;;
 *BindReadOnlyPaths*) if [[ $(cat scenario) == wrong-bind ]]; then echo /wrong:/opt/dkfz-live; else echo "$PWD:/opt/dkfz-live"; fi;;
 'restart dkfz-native-uart-live.service')
  if flock -n .jetson-control-deployment.lock true; then exit 90; fi
  echo restarted > restarted;;
 'is-active --quiet dkfz-native-uart-live.service') exit 0;;
 *) exit 1;;
esac
)SH");
    writeFixture(dir.filePath("deploy/dkfz-native-uart-live.service"), "ExecStart=/opt/dkfz-live/build_native/inference/native_uart_live_runtime --config /opt/dkfz-live/config/native_uart_live.conf\n");
    writeFixture(dir.filePath("deploy/model/model.conf"), "manifest");
    writeFixture(dir.filePath("config/native_uart_live.conf"), "config");
    writeFixture(dir.filePath("build_native/inference/native_uart_live_runtime"), "runtime");
    const auto hash = [](const QByteArray &bytes) {
      return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    };
    const QMap<QString, QString> paths{{"manifest", "deploy/model/model.conf"}, {"config", "config/native_uart_live.conf"}, {"runtime", "build_native/inference/native_uart_live_runtime"}};
    if (paths.contains(scenario)) writeFixture(dir.filePath(paths.value(scenario)), "changed");
    if (QString(scenario) == "direct")
      writeFixture(dir.filePath("dkfz-jetson-control-" + QString::number(getuid()) + "/native_uart_live_runtime.pid"), QByteArray::number(QCoreApplication::applicationPid()));
    QProcess process; process.setWorkingDirectory(dir.path()); process.setProcessEnvironment(fixtureEnvironment(dir));
    process.start("bash", {"-c", deploymentRestartCommand(false, hash("manifest"), hash("config"), hash("runtime"))});
    QVERIFY(process.waitForFinished(5000));
    QCOMPARE(process.exitCode() == 0, QString(scenario) == "valid");
    QCOMPARE(QFile::exists(dir.filePath("restarted")), QString(scenario) == "valid");
  }
#endif
}

void DiagnosticCommandBuilderTest::recognizesInstalledSnapshotAfterReopening() {
#ifdef Q_OS_WIN
  QSKIP("Linux fake host filesystem fixture");
#else
  for (const auto &variant : {"best", "tuned"}) {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString published = "finished_models/heart ' source";
    const QString snapshot = "build_native/prepared_models/gui-old-session/model";
    const QString config = published + (QString(variant) == "tuned" ? "/tuning/tuned_config.json" : "/train_config.json");
    const QString weights = published + (QString(variant) == "tuned" ? "/best_tuned.pth" : "/best.pth");
    writeFixture(dir.filePath(config), "saved config");
    writeFixture(dir.filePath(weights), "saved weights");
    writeFixture(dir.filePath(snapshot + "/train_config.json"), "saved config");
    writeFixture(dir.filePath(snapshot + "/best.pth"), "saved weights");
    writeFixture(dir.filePath("bin/systemctl"), "#!/bin/bash\nif [[ $1 == is-active ]]; then echo inactive; fi\nexit 1\n");
    writeFixture(dir.filePath("deploy/model/model.conf"), "model_name=heart\nmodel_variant=" + QByteArray(variant) + "\nsource_model_dir=" + snapshot.toUtf8() + '\n');
    DiagnosticRuntimeOptions options{.modelName="heart", .modelDirectory=published, .variant=variant};
    const auto matches = [&] {
      QProcess process; process.setWorkingDirectory(dir.path()); process.setProcessEnvironment(fixtureEnvironment(dir));
      process.start("bash", {"-c", availabilityProbeCommand(options)});
      if (!process.waitForFinished(5000) || process.exitCode() != 0) return false;
      return process.readAllStandardOutput().contains("__JCG_ACTIVE_SELECTION__=yes\n");
    };
    QVERIFY(matches()); // No in-memory bundle directory from the previous GUI.
    writeFixture(dir.filePath(weights), "different weights"); QVERIFY(!matches());
    writeFixture(dir.filePath(weights), "saved weights");
    writeFixture(dir.filePath(config), "different config"); QVERIFY(!matches());
    writeFixture(dir.filePath(config), "saved config");
    QTemporaryDir outside; QVERIFY(outside.isValid());
    writeFixture(outside.filePath("weights"), "saved weights");
    QVERIFY(QFile::remove(dir.filePath(snapshot + "/best.pth")));
    QVERIFY(QFile::link(outside.filePath("weights"), dir.filePath(snapshot + "/best.pth")));
    QVERIFY(!matches());
  }
#endif
}

QTEST_APPLESS_MAIN(DiagnosticCommandBuilderTest)

#include "test_diagnostic_command_builder.moc"
