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
  void isolatedDeployment_data() {
    QTest::addColumn<QString>("scenario");
    for (const auto &s : {"50hz", "100hz", "tuned", "recovery-off", "prepare-fails", "report-rejected", "stale-service", "stale-direct", "changed-input", "changed-policy", "changed-model"})
      QTest::newRow(s) << QString(s);
  }
  void isolatedDeployment() {
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
    write(root + "/service-state", "inactive\n");
    write(root + "/bin/ssh", "#!/bin/bash\nlast=${@: -1}\nlast=${last/exec bash -lc/exec bash -c}\nprintf 'ssh\\n' >> " + posixShellQuote(root + "/ssh.calls").toUtf8() + "\nexec bash -c \"$last\"\n", true);
    write(root + "/bin/systemctl", "#!/bin/bash\nif [[ $1 == is-active ]]; then cat service-state; exit 0; fi\nif [[ $1 == show ]]; then echo not-found; exit 0; fi\ncase $1 in start|stop|restart) printf 'UNEXPECTED SERVICE MUTATION\\n' >> actions;; esac\nexit 1\n", true);
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
    write(root + "/config/native_uart_live.conf", "input_reset_enabled=true\necg_baseline_reset_ms=100\nstuck_signal_reset_ms=100\ninput_recovery_ms=100\nwatchdog_enabled=true\nuart_timeout_ms=100\nsample_timeout_ms=100\n");
    write(root + "/build_native/inference/native_uart_live_runtime", "#!/bin/bash\nexit 0\n", true);
    write(root + "/deploy/model/model.conf", "model_name=old\nmodel_variant=best\nsource_model_dir=finished_models/old\nthreshold=0.3\n");
    write(root + "/deploy/model/model.engine", "old engine");
    write(root + "/prepare_model.sh", R"SH(#!/bin/bash
set -eu
if flock -n .jetson-control-deployment.lock true; then exit 90; fi
echo prepare >> actions
printf '%s\n' "$@" > prepare.args
test ! -e fail-prepare
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
report=
while (( $# )); do if [[ $1 == --validated-report ]]; then report=$2; fi; shift; done
test -r "$report"
test ! -e reject-report
echo activate >> actions
)SH", true);
    MainWindow window({.host="fake", .remoteDirectory=root});
    auto *models = window.findChild<QComboBox *>("finishedModelSelection");
    auto *refresh = window.findChild<QPushButton *>("refreshFinishedModels");
    auto *prepare = window.findChild<QPushButton *>("reexportModel");
    auto *activate = window.findChild<QPushButton *>("activatePreparedBundle");
    auto *bundle = window.findChild<QLineEdit *>("bundleDirectory");
    auto *inputs = window.findChild<QPlainTextEdit *>("validationInputs");
    auto *ecg = window.findChild<QCheckBox *>("ecg3SourceConfirmed");
    auto *qualification = window.findChild<QCheckBox *>("bundleQualificationAccepted");
    auto *prepState = window.findChild<QLabel *>("preparationState");
    auto *activationState = window.findChild<QLabel *>("activationState");
    auto *check = button(window, "Check availability");
    QVERIFY(models && refresh && prepare && activate && bundle && inputs && ecg && qualification && prepState && activationState && check);
    QVERIFY(read(root + "/ssh.calls").isEmpty());
    refresh->click();
    QTRY_COMPARE_WITH_TIMEOUT(models->count(), scenario == "tuned" ? 3 : 2, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(refresh->isEnabled(), 5000);
    const int index = scenario == "tuned" ? 2 : 1;
    models->setCurrentIndex(index);
    QVERIFY(QMetaObject::invokeMethod(models, "activated", Q_ARG(int, index)));
    bundle->setText("artifact/quoted ' bundle");
    inputs->setPlainText("data/recording ' one.parquet\ndata/two.parquet");
    check->click();
    QTRY_VERIFY_WITH_TIMEOUT(check->isEnabled(), 5000);
    QVERIFY(!prepare->isEnabled());
    ecg->setChecked(true);
    QTRY_VERIFY_WITH_TIMEOUT(prepare->isEnabled(), 5000);
    QVERIFY(!activate->isEnabled());
    auto *policy = window.findChild<QComboBox *>("recoveryDeploymentPolicy");
    if (scenario == "recovery-off") policy->setCurrentIndex(1);
    if (scenario == "prepare-fails") write(root + "/fail-prepare", "fail");
    prepare->click();
    QTRY_VERIFY_WITH_TIMEOUT(refresh->isEnabled(), 5000);
    QCOMPARE(read(root + "/actions"), QByteArray("prepare\n"));
    QVERIFY(!activate->isEnabled());
    const auto args = read(root + "/prepare.args");
    QVERIFY(args.contains("--prepare-only\n--output-dir\nartifact/quoted ' bundle\n"));
    QVERIFY(args.contains("--ecg3-source\necg2_minus_ecg1\n"));
    QVERIFY(args.contains("--max-stream-samples\n0\n"));
    QVERIFY(args.contains("--validation-input\ndata/recording ' one.parquet\n--validation-input\ndata/two.parquet\n"));
    QCOMPARE(args.contains("--no-recovery"), scenario == "recovery-off");
    QCOMPARE(args.contains("--variant\ntuned\n"), scenario == "tuned");
    QCOMPARE(read(root + "/deploy/model/model.engine"), QByteArray("old engine"));
    if (scenario == "prepare-fails") {
      QVERIFY(prepState->text().contains("failed"));
      QVERIFY(!qualification->isEnabled());
      return;
    }
    QVERIFY(prepState->text().contains("not activated"));
    QVERIFY(prepState->text().contains("unverified"));
    if (scenario == "tuned") {
      const auto alias = root + "/artifact/quoted ' bundle/model/tuning/tuned_config.json";
      QVERIFY(QFileInfo(alias).isSymLink());
      QCOMPARE(read(alias), read(root + "/artifact/quoted ' bundle/model/train_config.json"));
      QCOMPARE(read(alias), QByteArray("unchanged-config-copy\n"));
    }
    qualification->setChecked(true);
    QVERIFY(!activate->isEnabled());
    check->click();
    QTRY_VERIFY_WITH_TIMEOUT(activate->isEnabled(), 5000);
    if (scenario == "changed-input" || scenario == "changed-policy" || scenario == "changed-model") {
      if (scenario == "changed-input") inputs->appendPlainText("data/three.parquet");
      if (scenario == "changed-policy") policy->setCurrentIndex(1);
      if (scenario == "changed-model") window.findChild<QLineEdit *>("modelName")->setText("other");
      QVERIFY(!activate->isEnabled());
      QVERIFY(!qualification->isChecked());
      if (scenario == "changed-model") QVERIFY(!ecg->isChecked());
      return;
    }
    if (scenario == "report-rejected") write(root + "/reject-report", "rejected");
    if (scenario == "stale-service") write(root + "/service-state", "active\n");
    if (scenario == "stale-direct") write(root + "/dkfz-jetson-control-" + QString::number(getuid()) + "/native_uart_live_runtime.pid", QByteArray::number(QCoreApplication::applicationPid()));
    activate->click();
    QTRY_VERIFY_WITH_TIMEOUT(refresh->isEnabled(), 5000);
    const bool success = scenario != "report-rejected" && scenario != "stale-service" && scenario != "stale-direct";
    QCOMPARE(read(root + "/actions"), success ? QByteArray("prepare\nactivate\n") : QByteArray("prepare\n"));
    QVERIFY(activationState->text().contains(success ? "service not started" : "unconfirmed"));
    if (scenario != "stale-service" && scenario != "stale-direct") {
      const auto activation = read(root + "/activate.args");
      QVERIFY(activation.contains("--validated-report\nartifact/quoted ' bundle/validation_report.json\n"));
      QVERIFY(activation.contains("--config\nartifact/quoted ' bundle/model/train_config.json\n"));
      QVERIFY(activation.contains("--checkpoint\nartifact/quoted ' bundle/model/best.pth\n"));
      QVERIFY(activation.contains("--engine\nartifact/quoted ' bundle/model/model.engine\n"));
      QCOMPARE(activation.contains("--no-recovery"), scenario == "recovery-off");
    }
    QCOMPARE(read(root + '/' + source + "/train_config.json"), config);
    QCOMPARE(read(root + '/' + source + "/best.pth"), QByteArray("unchanged checkpoint"));
    const auto calls = read(root + "/ssh.calls");
    QTest::qWait(200);
    QCOMPARE(read(root + "/ssh.calls"), calls);
#endif
  }
};
QTEST_MAIN(DeploymentWorkflowTest)
#include "test_deployment_workflow.moc"
