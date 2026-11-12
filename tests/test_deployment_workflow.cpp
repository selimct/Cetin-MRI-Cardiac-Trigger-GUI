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
#include <QShortcut>
#include <QTextDocument>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>
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
  void reusesSavedEngine_data() {
    QTest::addColumn<QString>("scenario");
    for (const auto *scenario : {"50hz", "100hz", "tuned", "recovery-off", "changed-config", "changed-weights",
         "changed-engine", "changed-report", "report-rejected", "stale-service", "stale-direct", "readiness-fails"})
      QTest::newRow(scenario) << QString(scenario);
  }
  void reusesSavedEngine() {
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
    write(root + "/deploy/model/model.engine", "previous engine");
    write(root + "/deploy/model/model.conf", "model_name=previous\nmodel_variant=best\nsource_model_dir=finished_models/previous\nthreshold=0.3\n");
    write(root + "/bin/ssh", R"SH(#!/bin/bash
last=${@: -1}
last=${last/exec bash -lc/exec bash -c}
test() {
 if [[ $# == 2 && $1 == -d && $2 == /var/lib/dkfz-live ]]; then return 0; fi
 builtin test "$@"
}
export -f test
echo ssh >> ssh.calls
exec bash -c "$last"
)SH", true);
    write(root + "/bin/id", "#!/bin/bash\nexit 0\n", true);
    write(root + "/bin/getent", "#!/bin/bash\nexit 0\n", true);
    write(root + "/bin/systemctl", R"SH(#!/bin/bash
case "$1" in
 is-active) cat service-state; [[ $(cat service-state) == active ]];;
 cat) cat deploy/dkfz-native-uart-live.service;;
 is-enabled) exit 1;;
 show) case "$*" in
  *FragmentPath*) echo "$PWD/deploy/dkfz-native-uart-live.service";;
  *NeedDaemonReload*) echo no;;
  *LoadState*) echo loaded;;
 esac;;
 *) echo "UNEXPECTED SERVICE MUTATION $*" >> actions; exit 1;;
esac
)SH", true);
    const bool tuned = scenario == "tuned", recoveryOff = scenario == "recovery-off", delayed = scenario == "100hz";
    const QString source = "finished_models/heart ' source";
    const QString bundle = "build_native/prepared_models/gui-heart-20261113-quoted ' bundle";
    const QString configPath = source + (tuned ? "/tuning/tuned_config.json" : "/train_config.json");
    const QString weightsPath = source + (tuned ? "/best_tuned.pth" : "/best.pth");
    QJsonObject config{{"modelname", "heart"}, {"recovery_head", !delayed}, {"raw_hz",400}, {"frame_size_raw",delayed ? 4 : 8}, {"window_size",delayed ? 100 : 50}};
    if (delayed) { config["prediction_mode"] = "delayed_100hz"; config["prediction_delay_frames"] = 1; }
    const auto configBytes = QJsonDocument(config).toJson();
    write(root + '/' + configPath, configBytes);
    write(root + '/' + weightsPath, "saved weights");
    write(root + '/' + bundle + "/model/train_config.json", configBytes);
    write(root + '/' + bundle + "/model/best.pth", "saved weights");
    write(root + '/' + bundle + "/model/model.engine", "saved engine");
    if (tuned) {
      QVERIFY(QDir().mkpath(root + '/' + bundle + "/model/tuning"));
      QVERIFY(QFile::link(root + '/' + bundle + "/model/train_config.json", root + '/' + bundle + "/model/tuning/tuned_config.json"));
    }
    const auto hash = [](const QByteArray &bytes) {
      return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    };
    QJsonObject report{{"schema","dkfz-prepared-model-v1"}, {"modelname","heart"}, {"model_variant",tuned ? "tuned" : "best"},
        {"validation_passed",true}, {"source_unchanged",true}, {"disable_recovery",recoveryOff},
        {"ecg3_source","ecg2_minus_ecg1"},
        {"stream_validation",QJsonObject{{"passed",true},{"recordings",QJsonArray{QJsonObject{{"input","recording"}}}}}},
        {"sha256",QJsonObject{{"config",hash(configBytes)},{"checkpoint",hash("saved weights")},{"engine",hash("saved engine")}}}};
    write(root + '/' + bundle + "/validation_report.json", QJsonDocument(report).toJson());
    // Other identities/policies and failed preparations must not be offered.
    auto addReport = [&](const QString &name, QJsonObject other) {
      const auto path = root + "/build_native/prepared_models/" + name;
      write(path + "/validation_report.json", QJsonDocument(other).toJson());
      for (const auto *file : {"train_config.json", "best.pth", "model.engine"}) write(path + "/model/" + file, "other");
    };
    auto other = report; other["modelname"] = "other"; addReport("other-model", other);
    other = report; other["model_variant"] = tuned ? "best" : "tuned"; addReport("other-variant", other);
    other = report; other["disable_recovery"] = !recoveryOff; addReport("other-policy", other);
    other = report; other["validation_passed"] = false; addReport("failed", other);
    write(root + "/bin/check-report", R"SH(#!/bin/bash
set -eu
echo validate >> actions
printf '%s\n' "$@" > validation.args
if flock -n .jetson-control-deployment.lock true; then exit 90; fi
engine=
while (( $# )); do if [[ $1 == --engine ]]; then engine=$2; fi; shift; done
[[ $(cat scenario) != report-rejected ]]
test "$(cat "$engine")" = 'saved engine'
)SH", true);
    write(root + "/run/deploy_model.sh", R"SH(#!/bin/bash
set -eu
if flock -n .jetson-control-deployment.lock true; then exit 90; fi
printf '%s\n' "$@" > activation.args
name=$1; shift
model=; variant=
while (( $# )); do
 case $1 in --model-dir) model=$2;; --variant) variant=$2;; esac
 shift
done
echo activate >> actions
mkdir -p deploy/model
cp -- "$model/model.engine" deploy/model/model.engine
printf 'model_name=%s\nmodel_variant=%s\nsource_model_dir=%s\nthreshold=0.3\n' "$name" "$variant" "$model" > deploy/model/model.conf
)SH", true);
    write(root + "/build_native/inference/native_uart_live_runtime", "#!/bin/bash\n[[ $(cat scenario) != readiness-fails ]]\n", true);
    write(root + "/config/native_uart_live.conf", "manifest_path=deploy/model/model.conf\ninput_reset_enabled=true\necg_baseline_reset_ms=100\nstuck_signal_reset_ms=100\ninput_recovery_ms=100\nwatchdog_enabled=true\nuart_timeout_ms=100\nsample_timeout_ms=100\n");
    write(root + "/deploy/dkfz-native-uart-live.service", "WorkingDirectory=/opt/dkfz-live\nBindReadOnlyPaths=" + root.toUtf8() + ":/opt/dkfz-live\nReadWritePaths=/var/lib/dkfz-live\nExecStart=/opt/dkfz-live/build_native/inference/native_uart_live_runtime --config /opt/dkfz-live/config/native_uart_live.conf\n");
    MainWindow window({.host="fake", .remoteDirectory=root});
    window.pythonExecutableEdit_->setText(root + "/bin/check-report");
    window.refreshModelsButton_->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window.runner_.isRunning(), 5000);
    QCOMPARE(window.finishedModelCombo_->count(), 2);
    const auto beforeSelection = read(root + "/ssh.calls");
    window.finishedModelCombo_->setCurrentIndex(1);
    QVERIFY(QMetaObject::invokeMethod(window.finishedModelCombo_, "activated", Q_ARG(int,1)));
    if (recoveryOff) window.findChild<QComboBox *>("recoveryDeploymentPolicy")->setCurrentIndex(1);
    QCOMPARE(read(root + "/ssh.calls"), beforeSelection); // Selection stays local.
    QCOMPARE(window.savedBundleCombo_->count(), 1);
    QCOMPARE(window.savedBundleCombo_->currentData().toString(), bundle);
    QVERIFY(window.reuseBundleButton_->isEnabled());
    if (scenario == "50hz") {
      window.refreshBundlesButton_->click();
      QTRY_VERIFY_WITH_TIMEOUT(!window.runner_.isRunning(), 5000);
      QCOMPARE(window.savedBundleCombo_->currentData().toString(), bundle);
      QVERIFY(window.reuseBundleButton_->isEnabled());
    }
    QVERIFY(!window.ecg3SourceCheck_->isChecked()); // Prior report carries the confirmation.
    if (scenario == "changed-config") write(root + '/' + configPath, "changed config");
    if (scenario == "changed-weights") write(root + '/' + weightsPath, "changed weights");
    if (scenario == "changed-engine") write(root + '/' + bundle + "/model/model.engine", "changed engine");
    if (scenario == "changed-report") write(root + '/' + bundle + "/validation_report.json", "changed report");
    if (scenario == "stale-service") write(root + "/service-state", "active\n");
    if (scenario == "stale-direct") write(root + "/dkfz-jetson-control-" + QString::number(getuid()) + "/native_uart_live_runtime.pid", QByteArray::number(QCoreApplication::applicationPid()));
    window.reuseBundleButton_->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window.runner_.isRunning(), 10000);
    const bool validationAttempted = !QStringList{"changed-config", "changed-weights", "changed-report", "stale-service", "stale-direct"}.contains(scenario);
    const bool activated = validationAttempted && scenario != "changed-engine" && scenario != "report-rejected";
    QCOMPARE(read(root + "/actions"), activated ? QByteArray("validate\nactivate\n") : validationAttempted ? QByteArray("validate\n") : QByteArray{});
    QVERIFY(!QFile::exists(root + "/prepare_model.sh")); // Reuse cannot export or build.
    QCOMPARE(read(root + "/service-state"), QByteArray(scenario == "stale-service" ? "active\n" : "inactive\n"));
    if (activated) {
      QCOMPARE(read(root + "/deploy/model/model.engine"), QByteArray("saved engine"));
      const auto args = read(root + "/activation.args");
      QVERIFY(args.contains("--validated-report\n" + bundle.toUtf8() + "/validation_report.json\n"));
      QCOMPARE(args.contains("--no-recovery"), recoveryOff);
      QCOMPARE(window.capabilities_.serviceReady, scenario != "readiness-fails");
      if (scenario != "readiness-fails") {
        window.serviceSafetyCheck_->setChecked(true);
        QVERIFY(window.serviceStartButton_->isEnabled());
      }
    } else {
      QVERIFY(!window.serviceStartButton_->isEnabled());
      QCOMPARE(read(root + "/deploy/model/model.engine"), QByteArray("previous engine"));
    }
#endif
  }
  void allChecksReadReadinessAndModels() {
#ifdef Q_OS_WIN
    QSKIP("Linux fake SSH host");
#else
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto oldPath = qgetenv("PATH");
    struct Restore { QByteArray path; ~Restore() { qputenv("PATH", path); } } restore{oldPath};
    qputenv("PATH", dir.filePath("bin").toUtf8() + ':' + oldPath);
    write(dir.filePath("bin/ssh"), "#!/bin/bash\nlast=${@: -1}\nif [[ $last == *__JCG_SERVICE__* ]]; then cat " +
          posixShellQuote(dir.filePath("response")).toUtf8() + "; else exit 1; fi\n", true);
    // The fake SSH response is independent of real systemd or sudo access.
    MainWindow window({.host="fake", .remoteDirectory=dir.path()});
    const auto name = window.modelNameEdit_->text().toUtf8();
    const auto source = window.modelDirectoryEdit_->text().toUtf8();
    const auto catalog = "__JCG_FINISHED_MODEL__=" + source.toBase64() + ":best:" +
        ("{\"modelname\":\"" + name + "\"}").toBase64() + '\n';
    const auto response = "__JCG_SERVICE__=yes\n__JCG_SERVICE_READY__=yes\n__JCG_SERVICE_STATE__=inactive\n"
        "__JCG_RUNTIME__=yes\n__JCG_PREPARE__=yes\n__JCG_PREPARE_INPUTS__=yes\n"
        "__JCG_CONFIG__=yes\n__JCG_MANIFEST__=yes\n__JCG_ENGINE__=yes\n__JCG_SLOT_METADATA__=yes\n"
        "__JCG_ACTIVE_SELECTION__=yes\n__JCG_DIRECT_READY__=yes\n__JCG_DEPLOY_MANIFEST_REVISION__=" +
        QByteArray(64, 'a') + "\n__JCG_DEPLOY_CONFIG_REVISION__=" + QByteArray(64, 'b') +
        "\n__JCG_DEPLOY_RUNTIME_REVISION__=" + QByteArray(64, 'c') + '\n' + catalog;
    write(dir.filePath("response"), response);
    for (auto *check : {window.probeButton_, window.connectionCheckButton_, window.diagnosticCheckButton_}) {
      check->click();
      QTRY_VERIFY_WITH_TIMEOUT(!window.runner_.isRunning() && !window.recovery_.blocking() && !window.thresholds_.blocking(), 5000);
      QCOMPARE(window.observedManifestRevision_, QString(64, 'a'));
      QCOMPARE(window.finishedModelCombo_->count(), 2);
      window.serviceSafetyCheck_->setChecked(true);
      QVERIFY(window.serviceStartButton_->isEnabled());
      QVERIFY(window.startInferenceButton_->isEnabled());
    }
    // Return and keypad Enter use the same Check action from both scopes.
    int shortcuts = 0;
    for (auto *shortcut : window.findChildren<QShortcut *>()) {
      if (shortcut->key() != QKeySequence(Qt::Key_Return) && shortcut->key() != QKeySequence(Qt::Key_Enter)) continue;
      window.observedManifestRevision_.clear();
      QVERIFY(QMetaObject::invokeMethod(shortcut, "activated"));
      QTRY_VERIFY_WITH_TIMEOUT(!window.runner_.isRunning() && !window.recovery_.blocking() && !window.thresholds_.blocking(), 5000);
      QCOMPARE(window.observedManifestRevision_, QString(64, 'a'));
      QVERIFY(window.serviceStartButton_->isEnabled());
      ++shortcuts;
    }
    QCOMPARE(shortcuts, 4);
#endif
  }
  void streamCaptureStaysBounded() {
    MainWindow window;
    window.console_->document()->setMaximumBlockCount(100);
    QByteArray lines;
    for (int i = 0; i < 2000; ++i) lines += QByteArray(80, 'x') + '\n';
    for (const auto task : {MainWindow::TaskKind::serviceLogs, MainWindow::TaskKind::directInference, MainWindow::TaskKind::ordinary}) {
      window.activeTask_ = task;
      window.activeStandardOutput_.clear();
      window.activeStandardError_.clear();
      for (int i = 0; i < 3; ++i) {
        window.runner_.standardOutputReceived(lines);
        window.runner_.standardErrorReceived(lines);
        QVERIFY(window.activeStandardOutput_.size() <= 64 * 1024);
        QVERIFY(window.activeStandardError_.size() <= 64 * 1024);
      }
      QCOMPARE(window.activeStandardOutput_, lines.right(64 * 1024));
      QVERIFY(window.console_->document()->blockCount() <= 100);
    }
    // Newline-free producers also have bounded pending and display buffers.
    window.console_->clear();
    window.runner_.standardOutputReceived(QByteArray(256 * 1024, 'x'));
    QVERIFY(window.mainStandardOutputDisplayBuffer_.size() < 64 * 1024);
    QVERIFY(window.console_->document()->blockCount() > 1);
    // Finite machine-readable responses must remain complete beyond the tail limit.
    window.activeTask_ = MainWindow::TaskKind::availabilityProbe;
    window.activeStandardOutput_.clear();
    window.runner_.standardOutputReceived(lines);
    QCOMPARE(window.activeStandardOutput_, lines);
  }
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
