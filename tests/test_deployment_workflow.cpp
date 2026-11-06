// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "main_window.h"
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>

#ifndef Q_MOC_RUN
namespace
{
void write(const QString &path, const QByteArray &bytes, bool executable = false)
{
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QCOMPARE(f.write(bytes), bytes.size());
    f.close();
    if (executable)
        QVERIFY(f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
}
QByteArray read(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}
QPushButton *button(MainWindow &w, const QString &text)
{
    for (auto *b : w.findChildren<QPushButton *>())
        if (b->text() == text)
            return b;
    return nullptr;
}
} // namespace
#endif
class DeploymentWorkflowTest final : public QObject
{
    Q_OBJECT
  private slots:
    void guidedDeployment_data();
    void guidedDeployment();
};
void DeploymentWorkflowTest::guidedDeployment_data()
{
    QTest::addColumn<QString>("scenario");
    for (const auto &name :
         {"success", "saved-recovery", "saved-disabled", "null-calibration", "same-model-restore",
          "preflight-fails", "prepare-fails", "validation-fails", "changed-manifest", "changed-config",
          "changed-runtime", "config-during-prepare", "runtime-during-prepare", "direct-started"})
        QTest::newRow(name) << QString(name);
}
void DeploymentWorkflowTest::guidedDeployment()
{
#ifdef Q_OS_WIN
    QSKIP("Linux fake SSH/systemd fixture");
#else
    QFETCH(QString, scenario);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto root = dir.path();
    const auto oldPath = qgetenv("PATH"), oldRuntime = qgetenv("XDG_RUNTIME_DIR");
    struct Restore
    {
        QByteArray path, runtime;
        ~Restore()
        {
            qputenv("PATH", path);
            if (runtime.isNull())
                qunsetenv("XDG_RUNTIME_DIR");
            else
                qputenv("XDG_RUNTIME_DIR", runtime);
        }
    } restore{oldPath, oldRuntime};
    qputenv("PATH", root.toUtf8() + "/bin:" + oldPath);
    qputenv("XDG_RUNTIME_DIR", root.toUtf8());
    write(root + "/scenario", scenario.toUtf8());
    write(root + "/service-state", "active\n");
    write(root + "/deploy/dkfz-native-uart-live.service",
          "ExecStart=/opt/dkfz-live/build_native/inference/native_uart_live_runtime --config "
          "/opt/dkfz-live/config/native_uart_live.conf\n");
    write(root + "/deploy/model/model.conf",
          "model_name=old\nmodel_variant=best\nthreshold=0.3\nsource_model_dir=finished_models/"
          "old\nengine_size_bytes=3\nengine_crc32=abcdef01\n");
    write(root + "/deploy/model/model.engine", "old");
    write(root + "/config/native_uart_live.conf",
          "manifest_path=deploy/model/model.conf\nearly_emission=false\n");
    write(root + "/build_native/inference/native_uart_live_runtime", "#!/bin/bash\nexit 0\n", true);
    const QByteArray sourceConfig =
        scenario == "saved-disabled" ? QByteArray("{\"modelname\":\"heart\",\"recovery_head\":true,"
                                                  "\"recovery_enabled\":false,\"recovery_threshold\":0.9}")
        : scenario == "null-calibration"
            ? QByteArray("{\"modelname\":\"heart\",\"recovery_head\":true,\"recovery_threshold\":null}")
            : QByteArray("{\"modelname\":\"heart\",\"recovery_head\":true,\"recovery_threshold\":0.9}");
    write(root + "/finished_models/heart_lopo_recovery/train_config.json", sourceConfig);
    write(root + "/finished_models/heart_lopo_recovery/best.pth", "published checkpoint");
    write(root + "/bin/sudo", "#!/bin/bash\nwhile [[ $1 != -- ]]; do shift; done\nshift\nexec \"$@\"\n",
          true);
    write(root + "/bin/python3", "#!/bin/bash\nexit 127\n", true);
    write(root + "/bin/systemctl", R"SH(#!/bin/bash
set -eu
if [[ $1 == show ]]; then
 case "$*" in
  *NeedDaemonReload*) echo no;;
  *FragmentPath*) echo "$PWD/deploy/dkfz-native-uart-live.service";;
  *WorkingDirectory*) echo /opt/dkfz-live;;
  *BindReadOnlyPaths*) echo "$PWD:/opt/dkfz-live";;
  *RootDirectory*) echo;;
  *InvocationID*) echo 11111111111111111111111111111111;;
 esac
elif [[ $1 == stop ]]; then echo stop >> actions; echo inactive > service-state
elif [[ $1 == start ]]; then echo start >> actions; echo active > service-state
elif [[ $1 == is-active ]]; then
 if [[ $* != *--quiet* ]]; then cat service-state; fi
 [[ $(cat service-state) == active ]]
else exit 1; fi
)SH",
          true);
    write(root + "/probe", R"PROBE(__JCG_SERVICE__=yes
__JCG_SERVICE_UNIT__=yes
__JCG_SERVICE_LINKED__=yes
__JCG_SERVICE_CONTRACT__=yes
__JCG_RUNTIME__=yes
__JCG_SERVICE_CONFIG__=yes
__JCG_CONFIG__=yes
__JCG_MANIFEST__=yes
__JCG_ENGINE__=yes
__JCG_SLOT_METADATA__=yes
__JCG_PREPARE__=yes
__JCG_PREPARE_INPUTS__=no
__JCG_REEXPORT_INPUTS__=yes
__JCG_DIRECT_ACTIVE__=no
)PROBE");
    write(root + "/bin/ssh",
          QString(R"SH(#!/bin/bash
set -eu
last=${@: -1}
last=${last/exec bash -lc/exec bash -c}
cd -- %1
printf 'ssh\n' >> ssh.calls
if test -e offline; then echo 'Host unavailable' >&2; exit 255; fi
if [[ $last == *__JCG_SERVICE__* ]]; then
 if [[ $(cat scenario) == preflight-fails ]]; then sed 's/__JCG_SERVICE_CONTRACT__=yes/__JCG_SERVICE_CONTRACT__=no/' probe; else cat probe; fi
 state=$(cat service-state)
 printf '__JCG_SERVICE_STATE__=%%s\n' "$state"
 if [[ $state == active ]]; then echo __JCG_SERVICE_ACTIVE__=yes; else echo __JCG_SERVICE_ACTIVE__=no; fi
 if grep -Fqx model_name=heart deploy/model/model.conf; then
  echo __JCG_ACTIVE_SELECTION__=yes
  if [[ $state == inactive ]]; then echo validate >> actions; else echo final-check >> actions; fi
  if [[ $(cat scenario) == validation-fails ]]; then echo __JCG_SERVICE_READY__=no; else echo __JCG_SERVICE_READY__=yes; fi
 else echo __JCG_ACTIVE_SELECTION__=no; echo __JCG_SERVICE_READY__=yes; fi
 for entry in MANIFEST:deploy/model/model.conf CONFIG:config/native_uart_live.conf RUNTIME:build_native/inference/native_uart_live_runtime; do
  printf '__JCG_DEPLOY_%%s_REVISION__=' "${entry%%:*}"; sha256sum "${entry#*:}" | cut -d ' ' -f1
 done
 exit 0
fi
if [[ $last == *DKFZ_DEPLOY_START* ]]; then
 case $(cat scenario) in
  changed-manifest) echo '# changed after validation' >> deploy/model/model.conf;;
  changed-config) echo '# changed after validation' >> config/native_uart_live.conf;;
  changed-runtime) echo '# changed after validation' >> build_native/inference/native_uart_live_runtime;;
  direct-started) mkdir -p "${XDG_RUNTIME_DIR}/dkfz-jetson-control-${UID}"; echo %2 > "${XDG_RUNTIME_DIR}/dkfz-jetson-control-${UID}/native_uart_live_runtime.pid";;
 esac
fi
exec bash -c "$last"
)SH")
              .arg(posixShellQuote(root), QString::number(QCoreApplication::applicationPid()))
              .replace("%%", "%")
              .toUtf8(),
          true);
    write(root + "/prepare_model.sh", R"SH(#!/bin/bash
set -eu
[[ $(cat service-state) == inactive ]]
if flock -n .jetson-control-deployment.lock true; then exit 90; fi
echo prepare >> actions
printf '%s\n' "$@" > prepare.args
[[ $(cat scenario) != prepare-fails ]]
case $(cat scenario) in
 config-during-prepare) echo '# changed during preparation' >> config/native_uart_live.conf;;
 runtime-during-prepare) echo '# changed during preparation' >> build_native/inference/native_uart_live_runtime;;
esac
recovery=0.9
case $(cat scenario) in saved-disabled|null-calibration) recovery=;; esac
if [[ " $* " == *" --no-recovery "* ]]; then recovery=; fi
printf 'model_name=heart\nmodel_variant=best\nthreshold=0.3\nrecovery_head=true\nrecovery_threshold=%s\nsource_model_dir=finished_models/heart_lopo_recovery\nengine_size_bytes=3\nengine_crc32=abcdef01\nframe_size_raw=5\nraw_hz=500\n' "$recovery" > deploy/model/model.conf
)SH",
          true);
    MainWindow w({.host = "fake", .remoteDirectory = root});
    auto *models = w.findChild<QComboBox *>("finishedModelSelection");
    auto *refresh = w.findChild<QPushButton *>("refreshFinishedModels");
    auto *deploy = w.findChild<QPushButton *>("deployAndStart");
    auto *safety = w.findChild<QCheckBox *>("serviceHardwareReview");
    auto *policy = w.findChild<QComboBox *>("recoveryDeploymentPolicy");
    auto *check = button(w, "Check availability");
    QVERIFY(models && refresh && deploy && safety && policy && check);
    QVERIFY(!w.findChild<QPushButton *>("installRecoverySupport"));
    QVERIFY(!w.findChild<QPushButton *>("chooseRuntimePackage"));
    const auto originalEngine = read(root + "/deploy/model/model.engine");
    const auto originalRuntime = read(root + "/build_native/inference/native_uart_live_runtime");
    QVERIFY(read(root + "/ssh.calls").isEmpty());
    refresh->click();
    QTRY_COMPARE_WITH_TIMEOUT(models->count(), 2, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(refresh->isEnabled(), 5000);
    QVERIFY(models->itemText(1).contains("Recovery supported"));
    models->setCurrentIndex(1);
    QVERIFY(QMetaObject::invokeMethod(models, "activated", Q_ARG(int, 1)));
    const bool savedPolicy =
        scenario == "saved-recovery" || scenario == "saved-disabled" || scenario == "null-calibration";
    policy->setCurrentIndex(savedPolicy ? 0 : 1);
    QVERIFY(!deploy->isEnabled());
    QVERIFY(read(root + "/actions").isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(safety->isEnabled(), 5000);
    QVERIFY(!deploy->isEnabled());
    safety->setChecked(true);
    QTRY_VERIFY_WITH_TIMEOUT(deploy->isEnabled(), 5000);
    deploy->click();
    QTRY_VERIFY_WITH_TIMEOUT(check->isEnabled(), 10000);
    QTest::qWait(100);
    QTRY_VERIFY_WITH_TIMEOUT(check->isEnabled(), 5000);
    const auto actions = read(root + "/actions");
    if (scenario == "preflight-fails")
    {
        QVERIFY(actions.isEmpty());
        QCOMPARE(read(root + "/service-state"), QByteArray("active\n"));
        return;
    }
    QVERIFY(actions.startsWith("stop\nprepare\n"));
    const auto args = read(root + "/prepare.args");
    QVERIFY(args.contains("--variant\nbest\n"));
    QCOMPARE(args.contains("--no-recovery\n"), !savedPolicy);
    QVERIFY(!args.contains("--enable-recovery"));
    QVERIFY(!args.contains("--recovery\n"));
    QVERIFY(!args.contains("--jetson-only"));
    if (scenario == "success" || savedPolicy || scenario == "same-model-restore")
    {
        QCOMPARE(actions, QByteArray("stop\nprepare\nvalidate\nstart\n"));
        QCOMPARE(read(root + "/service-state"), QByteArray("active\n"));
        QVERIFY(button(w, "Stop")->isEnabled());
        QVERIFY(!button(w, "Prepare + activate model")->isEnabled());
        const bool recoveryOn = scenario == "saved-recovery";
        QVERIFY(read(root + "/deploy/model/model.conf")
                    .contains(recoveryOn ? "recovery_threshold=0.9\n" : "recovery_threshold=\n"));
        QVERIFY(read(root + "/deploy/model/model.conf").contains("recovery_head=true\n"));
        const auto afterStart = read(root + "/ssh.calls");
        QTest::qWait(150);
        QCOMPARE(read(root + "/ssh.calls"), afterStart); // Start does not refresh status.
        QCOMPARE(read(root + "/finished_models/heart_lopo_recovery/train_config.json"), sourceConfig);
        QCOMPARE(read(root + "/finished_models/heart_lopo_recovery/best.pth"),
                 QByteArray("published checkpoint"));
        QCOMPARE(read(root + "/deploy/model/model.engine"), originalEngine);
        QCOMPARE(read(root + "/build_native/inference/native_uart_live_runtime"), originalRuntime);
        if (scenario == "same-model-restore")
        {
            policy->setCurrentIndex(0); // Keep the exact same model and variant.
            QTest::qWait(100);
            QCOMPARE(read(root + "/ssh.calls"), afterStart);
            deploy->click();
            QTRY_VERIFY_WITH_TIMEOUT(check->isEnabled(), 10000);
            QVERIFY(read(root + "/prepare.args").contains("--variant\nbest\n"));
            QVERIFY(!read(root + "/prepare.args").contains("--no-recovery"));
            QVERIFY(read(root + "/deploy/model/model.conf")
                        .contains("recovery_head=true\nrecovery_threshold=0.9\n"));
            QCOMPARE(read(root + "/actions").count("stop\n"), 2);
            QCOMPARE(read(root + "/actions").count("start\n"), 2);
            QCOMPARE(read(root + "/finished_models/heart_lopo_recovery/train_config.json"), sourceConfig);
            QCOMPARE(read(root + "/deploy/model/model.engine"), originalEngine);
        }
        if (scenario == "success")
        {
            // Enter is the same explicit read request as Check, and performs no
            // model/service mutation. The command console is outside this scope.
            w.show();
            w.activateWindow();
            QVERIFY(QTest::qWaitForWindowActive(&w));
            policy->setFocus();
            QTest::keyClick(policy, Qt::Key_Return);
            QTRY_VERIFY_WITH_TIMEOUT(read(root + "/ssh.calls").size() > afterStart.size(), 5000);
            QTRY_VERIFY_WITH_TIMEOUT(check->isEnabled(), 5000);
            QCOMPARE(read(root + "/actions").count("stop\n"), 1);
            QCOMPARE(read(root + "/actions").count("start\n"), 1);
            const auto afterEnter = read(root + "/ssh.calls");
            QTest::qWait(3200); // No recurring status checks after explicit refresh.
            QCOMPARE(read(root + "/ssh.calls"), afterEnter);
            policy->setFocus();
            QTest::keyClick(policy, Qt::Key_Enter);
            QTRY_VERIFY_WITH_TIMEOUT(read(root + "/ssh.calls").size() > afterEnter.size(), 5000);
            QTRY_VERIFY_WITH_TIMEOUT(check->isEnabled(), 5000);
            QCOMPARE(read(root + "/deploy/model/model.engine"), originalEngine);
            write(root + "/offline", "offline");
            check->click();
            QTRY_VERIFY_WITH_TIMEOUT(check->isEnabled(), 5000);
            QVERIFY(!button(w, "Stop")->isEnabled());
            QVERIFY(!button(w, "Start")->isEnabled());
            QVERIFY(!safety->isChecked());
            QVERIFY(!w.findChild<QLabel *>("recoverySummary")->text().contains("Saved startup:"));
            QFile::remove(root + "/offline");
            check->click();
            QTRY_VERIFY_WITH_TIMEOUT(check->isEnabled(), 5000);
            QVERIFY(button(w, "Stop")->isEnabled());
            const auto transport = read(root + "/bin/ssh");
            QVERIFY(QFile::remove(root + "/bin/ssh"));
            check->click();
            QTRY_VERIFY_WITH_TIMEOUT(check->isEnabled(), 5000);
            QVERIFY(!button(w, "Stop")->isEnabled());
            QVERIFY(!safety->isChecked());
            write(root + "/bin/ssh", transport, true);
        }
    }
    else
    {
        QVERIFY(!actions.contains("start\n"));
        QCOMPARE(read(root + "/service-state"), QByteArray("inactive\n"));
        QVERIFY(!deploy->isEnabled());
    }
#endif
}
QTEST_MAIN(DeploymentWorkflowTest)
#include "test_deployment_workflow.moc"
