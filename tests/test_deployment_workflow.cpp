// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "main_window.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#ifndef Q_OS_WIN
#include <unistd.h>
#endif
namespace {
void write(const QString &path, const QByteArray &bytes, bool executable = false) {
  QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
  QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
  QCOMPARE(file.write(bytes), bytes.size()); file.close();
  if (executable) QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
}
QByteArray read(const QString &path) {
  QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
QPushButton *button(MainWindow &window, const QString &text) {
  for (auto *b : window.findChildren<QPushButton *>()) if (b->text() == text) return b;
  return nullptr;
}
}
class DeploymentWorkflowTest final : public QObject {
  Q_OBJECT
private slots:
  void observesWorkerSchedulingWithoutInferringRealtime() {
#ifdef Q_OS_WIN
    QSKIP("Linux fake SSH host");
#else
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto oldPath = qgetenv("PATH");
    struct Restore { QByteArray path; ~Restore() { qputenv("PATH", path); } } restore{oldPath};
    qputenv("PATH", dir.path().toUtf8() + "/bin:" + oldPath);
    write(dir.filePath("bin/ssh"), R"SH(#!/bin/bash
last=${@: -1}
if [[ $last == *__JCG_SERVICE__* ]]; then
 echo __JCG_SERVICE__=yes
 echo __JCG_SERVICE_STATE__=active
 echo __JCG_SERVICE_ACTIVE__=yes
 echo __JCG_SCHEDULING_PROFILE__=bounded_rr20
elif [[ $last == *journalctl* ]]; then
 echo '{"code":"worker_scheduling","thread":"dkfz-acquire","requested_realtime_priority":20,"policy":"rr","priority":20,"realtime_applied":true,"degraded":false,"error":0}'
 echo '{"code":"worker_scheduling","thread":"dkfz-infer","requested_realtime_priority":20,"policy":"other","priority":0,"realtime_applied":false,"degraded":true,"error":1,"prerequisite_error":"bounded limit missing"}'
 echo '{"code":"runtime_status","missed_frame_deadlines":6,"max_frame_end_to_completion_ns":23000000}'
fi
)SH", true);
    MainWindow window({.host="fake", .remoteDirectory=dir.path()});
    auto *check = button(window, "Check availability");
    auto *logs = button(window, "Follow log");
    check->click();
    QTRY_VERIFY_WITH_TIMEOUT(logs->isEnabled(), 5000);
    logs->click();
    auto *workers = window.findChild<QLabel *>("workerSchedulingStatus");
    auto *timing = window.findChild<QLabel *>("runtimeTimingStatus");
    QVERIFY(workers && timing);
    QTRY_VERIFY_WITH_TIMEOUT(workers->text().contains("dkfz-infer: actual other/0"), 5000);
    QVERIFY(workers->text().contains("dkfz-acquire: actual rr/20"));
    QVERIFY(workers->text().contains("degraded=yes"));
    QVERIFY(workers->text().contains("current invocation unconfirmed"));
    QTRY_VERIFY(timing->text().contains("missed frame deadlines=6; maximum frame delay=23 ms"));
    QVERIFY(timing->text().contains("Sustained performance unverified"));
    QTRY_VERIFY_WITH_TIMEOUT(logs->isEnabled(), 5000);
#endif
  }
  void installsSelectedModel_data() {
    QTest::addColumn<QString>("scenario");
    for (const auto &s : {"50hz", "100hz", "tuned", "recovery-off", "custom-recordings", "missing-recordings", "prepare-fails", "report-rejected", "readiness-fails", "wrong-selection", "stale-service", "stale-direct", "reinstall"})
      QTest::newRow(s) << QString(s);
  }
  void installsSelectedModel() {
#ifdef Q_OS_WIN
    QSKIP("Linux fake SSH host");
#else
    QFETCH(QString, scenario);
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto root = dir.path();
    const auto oldPath = qgetenv("PATH"), oldRuntime = qgetenv("XDG_RUNTIME_DIR");
    struct Restore {
      QByteArray path, runtime;
      ~Restore() { qputenv("PATH", path); if (runtime.isNull()) qunsetenv("XDG_RUNTIME_DIR"); else qputenv("XDG_RUNTIME_DIR", runtime); }
    } restore{oldPath, oldRuntime};
    qputenv("PATH", root.toUtf8() + "/bin:" + oldPath);
    qputenv("XDG_RUNTIME_DIR", root.toUtf8());
    write(root + "/scenario", scenario.toUtf8());
    write(root + "/service-state", "inactive\n");
    write(root + "/bin/ssh", R"SH(#!/bin/bash
last=${@: -1}
last=${last/exec bash -lc/exec bash -c}
# Emulate the service state directory without touching the host filesystem.
test() {
 if [[ $# == 2 && $1 == -d && $2 == /var/lib/dkfz-live ]]; then return 0; fi
 builtin test "$@"
}
export -f test
exec bash -c "$last"
)SH", true);
    write(root + "/bin/id", "#!/bin/bash\nif [[ $* == dkfz-live ]]; then exit 0; fi\nexec /usr/bin/id \"$@\"\n", true);
    write(root + "/bin/getent", "#!/bin/bash\nexit 0\n", true);
    write(root + "/bin/systemctl", R"SH(#!/bin/bash
case "$1" in
 is-active) cat service-state; [[ $(cat service-state) == active ]];;
 cat) cat deploy/dkfz-native-uart-live.service;;
 is-enabled) exit 1;;
 show)
  case "$*" in
   *FragmentPath*) echo "$PWD/deploy/dkfz-native-uart-live.service";;
   *NeedDaemonReload*) echo no;;
   *LoadState*) echo loaded;;
  esac;;
 *) echo "UNEXPECTED SERVICE MUTATION $*" >> actions; exit 1;;
esac
)SH", true);
    const bool delayed = scenario == "100hz";
    const QString source = delayed ? "finished_models/heart/delayed_100hz" : "finished_models/heart/recovery";
    const QByteArray config = delayed
        ? QByteArray("{\"modelname\":\"heart_delayed_100hz\",\"prediction_mode\":\"delayed_100hz\",\"prediction_delay_frames\":1,\"frame_size_raw\":4,\"raw_hz\":400,\"window_size\":100}")
        : QByteArray("{\"modelname\":\"heart\",\"recovery_head\":true,\"frame_size_raw\":8,\"raw_hz\":400,\"window_size\":50}");
    write(root + '/' + source + "/train_config.json", config);
    write(root + '/' + source + "/best.pth", "unchanged checkpoint");
    if (scenario == "tuned") {
      write(root + '/' + source + "/tuning/tuned_config.json", config);
      write(root + '/' + source + "/best_tuned.pth", "unchanged tuned checkpoint");
    }
    if (scenario != "missing-recordings")
      write(root + "/data/live/p12/p12_out.parquet", "raw ECG fixture");
    write(root + "/config/native_uart_live.conf", "manifest_path=deploy/model/model.conf\ninput_reset_enabled=true\necg_baseline_reset_ms=100\nstuck_signal_reset_ms=100\ninput_recovery_ms=100\nwatchdog_enabled=true\nuart_timeout_ms=100\nsample_timeout_ms=100\n");
    write(root + "/deploy/dkfz-native-uart-live.service", "WorkingDirectory=/opt/dkfz-live\nBindReadOnlyPaths=" + root.toUtf8() + ":/opt/dkfz-live\nReadWritePaths=/var/lib/dkfz-live\nExecStart=/opt/dkfz-live/build_native/inference/native_uart_live_runtime --config /opt/dkfz-live/config/native_uart_live.conf\n");
    write(root + "/build_native/inference/native_uart_live_runtime", "#!/bin/bash\nif [[ $(cat scenario) == readiness-fails ]] && test -e activated; then exit 1; fi\nexit 0\n", true);
    write(root + "/deploy/model/model.conf", "model_name=old\nmodel_variant=best\nsource_model_dir=finished_models/old\nthreshold=0.3\n");
    write(root + "/deploy/model/model.engine", "old engine");
    write(root + "/prepare_model.sh", R"SH(#!/bin/bash
set -eu
if flock -n .jetson-control-deployment.lock true; then exit 90; fi
echo prepare >> actions
printf '%s\n' "$@" > prepare.args
[[ $(cat scenario) != prepare-fails ]]
bundle=
while (( $# )); do if [[ $1 == --output-dir ]]; then bundle=$2; fi; shift; done
test -n "$bundle"
mkdir -p "$bundle/model"
echo unchanged-config-copy > "$bundle/model/train_config.json"
echo report > "$bundle/validation_report.json"
)SH", true);
    write(root + "/run/deploy_model.sh", R"SH(#!/bin/bash
set -eu
if flock -n .jetson-control-deployment.lock true; then exit 90; fi
printf '%s\n' "$@" > activate.args
name=$1; shift
report=; model=; variant=
while (( $# )); do
 case $1 in --validated-report) report=$2;; --model-dir) model=$2;; --variant) variant=$2;; esac
 shift
done
test -r "$report"
[[ $(cat scenario) != report-rejected ]]
echo activate >> actions
if [[ $(cat scenario) == wrong-selection ]]; then name=other; fi
printf 'model_name=%s\nmodel_variant=%s\nsource_model_dir=%s\nthreshold=0.3\n' "$name" "$variant" "$model" > deploy/model/model.conf
echo new-engine > deploy/model/model.engine
touch activated
)SH", true);
    MainWindow window({.host="fake", .remoteDirectory=root});
    auto *models = window.findChild<QComboBox *>("finishedModelSelection");
    auto *refresh = window.findChild<QPushButton *>("refreshFinishedModels");
    auto *install = window.findChild<QPushButton *>("installSelectedModel");
    auto *inputs = window.findChild<QPlainTextEdit *>("validationInputs");
    auto *ecg = window.findChild<QCheckBox *>("ecg3SourceConfirmed");
    auto *prepState = window.findChild<QLabel *>("preparationState");
    auto *activationState = window.findChild<QLabel *>("activationState");
    auto *installationState = window.findChild<QLabel *>("installationState");
    auto *advanced = window.findChild<QWidget *>("advancedModelInstallation");
    QVERIFY(models && refresh && install && inputs && ecg && prepState && activationState && installationState && advanced);
    QVERIFY(advanced->isHidden());
    QVERIFY(!window.findChild<QLineEdit *>("bundleDirectory"));
    QVERIFY(!window.findChild<QCheckBox *>("bundleQualificationAccepted"));
    refresh->click();
    QTRY_COMPARE_WITH_TIMEOUT(models->count(), scenario == "tuned" ? 3 : 2, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(refresh->isEnabled(), 5000);
    const int index = scenario == "tuned" ? 2 : 1;
    models->setCurrentIndex(index);
    QVERIFY(QMetaObject::invokeMethod(models, "activated", Q_ARG(int, index)));
    QVERIFY(!install->isEnabled());
    ecg->setChecked(true);
    QTRY_VERIFY_WITH_TIMEOUT(install->isEnabled(), 5000); // No extra Check or preparation fields.
    if (scenario == "custom-recordings") inputs->setPlainText("data/recording ' one.parquet\ndata/two.parquet");
    auto *policy = window.findChild<QComboBox *>("recoveryDeploymentPolicy");
    if (scenario == "recovery-off") policy->setCurrentIndex(1);
    if (scenario == "stale-service") write(root + "/service-state", "active\n");
    if (scenario == "stale-direct") write(root + "/dkfz-jetson-control-" + QString::number(getuid()) + "/native_uart_live_runtime.pid", QByteArray::number(QCoreApplication::applicationPid()));
    install->click();
    QTRY_VERIFY_WITH_TIMEOUT(refresh->isEnabled(), 10000);
    const bool prepared = scenario != "prepare-fails" && scenario != "missing-recordings" && scenario != "stale-service" && scenario != "stale-direct";
    const bool activated = prepared && scenario != "report-rejected";
    const bool ready = activated && scenario != "readiness-fails" && scenario != "wrong-selection";
    const auto actions = read(root + "/actions");
    if (scenario == "missing-recordings" || scenario == "stale-service" || scenario == "stale-direct") {
      QVERIFY(actions.isEmpty());
    } else {
      QCOMPARE(actions, activated ? QByteArray("prepare\nactivate\n") : QByteArray("prepare\n"));
      const auto args = read(root + "/prepare.args");
      QVERIFY(args.contains("--prepare-only\n--output-dir\nbuild_native/prepared_models/gui-"));
      QVERIFY(args.contains("--ecg3-source\necg2_minus_ecg1\n"));
      QVERIFY(args.contains("--max-stream-samples\n0\n"));
      QVERIFY(args.contains(scenario == "custom-recordings"
          ? "--validation-input\ndata/recording ' one.parquet\n--validation-input\ndata/two.parquet\n"
          : "--validation-input\ndata/live/p12/p12_out.parquet\n"));
      QCOMPARE(args.contains("--no-recovery"), scenario == "recovery-off");
      QCOMPARE(args.contains("--variant\ntuned\n"), scenario == "tuned");
      const auto argList = args.split('\n');
      const auto bundle = QString::fromUtf8(argList.at(argList.indexOf("--output-dir") + 1));
      if (prepared) {
        const auto activation = read(root + "/activate.args");
        QVERIFY(activation.contains("--validated-report\n" + bundle.toUtf8() + "/validation_report.json\n"));
        QVERIFY(activation.contains("--config\n" + bundle.toUtf8() + "/model/train_config.json\n"));
        QVERIFY(activation.contains("--checkpoint\n" + bundle.toUtf8() + "/model/best.pth\n"));
        QVERIFY(activation.contains("--engine\n" + bundle.toUtf8() + "/model/model.engine\n"));
        QCOMPARE(activation.contains("--no-recovery"), scenario == "recovery-off");
      }
      if (scenario == "tuned") {
        const auto alias = root + '/' + bundle + "/model/tuning/tuned_config.json";
        QVERIFY(QFileInfo(alias).isSymLink());
        QCOMPARE(read(alias), QByteArray("unchanged-config-copy\n"));
      }
    }
    auto *start = button(window, "Start");
    QVERIFY(start);
    auto *hardware = window.findChild<QCheckBox *>("serviceHardwareReview");
    if (ready) {
      QVERIFY(installationState->text().contains("Installed — ready to start"));
      QVERIFY(prepState->text().contains("unverified"));
      QVERIFY(activationState->text().contains("service not started"));
      QVERIFY(window.findChild<QLabel *>("serviceState")->text().contains("inactive"));
      hardware->setChecked(true);
      QVERIFY(start->isEnabled()); // Installation includes readiness verification; no manual Check.
    } else {
      QVERIFY(!installationState->text().contains("ready to start"));
      hardware->setChecked(true);
      QVERIFY(!start->isEnabled());
    }
    if (!activated) QCOMPARE(read(root + "/deploy/model/model.engine"), QByteArray("old engine"));
    QCOMPARE(read(root + '/' + source + "/train_config.json"), config);
    QCOMPARE(read(root + '/' + source + "/best.pth"), QByteArray("unchanged checkpoint"));
    QCOMPARE(read(root + "/service-state"), scenario == "stale-service" ? QByteArray("active\n") : QByteArray("inactive\n"));
    QVERIFY(!actions.contains("UNEXPECTED SERVICE MUTATION"));
    if (scenario == "reinstall") {
      const auto firstArgs = read(root + "/prepare.args").split('\n');
      const auto firstBundle = firstArgs.at(firstArgs.indexOf("--output-dir") + 1);
      install->click();
      QTRY_VERIFY_WITH_TIMEOUT(refresh->isEnabled(), 10000);
      QVERIFY(installationState->text().contains("Installed — ready to start"));
      const auto secondArgs = read(root + "/prepare.args").split('\n');
      const auto secondBundle = secondArgs.at(secondArgs.indexOf("--output-dir") + 1);
      QVERIFY(firstBundle != secondBundle);
      QVERIFY(QFile::exists(root + '/' + QString::fromUtf8(firstBundle) + "/validation_report.json"));
      QVERIFY(QFile::exists(root + '/' + QString::fromUtf8(secondBundle) + "/validation_report.json"));
      QCOMPARE(read(root + "/actions"), QByteArray("prepare\nactivate\nprepare\nactivate\n"));
      QCOMPARE(read(root + "/service-state"), QByteArray("inactive\n"));
    }
#endif
  }
};
QTEST_MAIN(DeploymentWorkflowTest)
#include "test_deployment_workflow.moc"
