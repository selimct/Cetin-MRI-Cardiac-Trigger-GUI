// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#include "main_window.h"
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>

namespace {
void writeFile(const QString &path, const QByteArray &bytes, bool executable = false) {
  QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
  QFile file(path);
  QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(file.errorString()));
  QCOMPARE(file.write(bytes), bytes.size());
  if (executable)
    QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
}
QByteArray readFile(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  return file.readAll();
}
QPushButton *button(MainWindow &window, const QString &text) {
  for (auto *candidate : window.findChildren<QPushButton *>())
    if (candidate->text() == text) return candidate;
  return nullptr;
}
}

class ModelDeploymentTest final : public QObject {
  Q_OBJECT
private slots:
  void selectsAndReexportsOnlyOnExplicitActions() {
#ifdef Q_OS_WIN
    QSKIP("Fake Linux SSH host requires bash and python3");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto oldPath = qgetenv("PATH");
    const auto oldRuntime = qgetenv("XDG_RUNTIME_DIR");
    struct RestoreEnvironment {
      QByteArray path, runtime;
      ~RestoreEnvironment() {
        qputenv("PATH", path);
        if (runtime.isNull()) qunsetenv("XDG_RUNTIME_DIR");
        else qputenv("XDG_RUNTIME_DIR", runtime);
      }
    } restore{oldPath, oldRuntime};
    const auto bin = dir.filePath("bin");
    qputenv("PATH", bin.toUtf8() + ':' + oldPath);
    qputenv("XDG_RUNTIME_DIR", dir.path().toUtf8());
    writeFile(bin + "/ssh", "#!/bin/bash\nprintf 'ssh\\n' >> " + posixShellQuote(dir.filePath("ssh.calls")).toUtf8() + "\nlast=${@: -1}\nlast=${last/exec bash -lc/exec bash -c}\nexec bash -c \"$last\"\n", true);
    writeFile(bin + "/systemctl", "#!/bin/bash\nif [[ $1 == is-active ]]; then cat service-state 2>/dev/null || echo inactive; fi\nexit 1\n", true);
    QTemporaryDir outside;
    QVERIFY(outside.isValid());
    writeFile(outside.filePath("train_config.json"), "{\"modelname\":\"outside_model\"}");
    writeFile(dir.filePath("finished_models/external/recovery/best.pth"), "weights");
    QVERIFY(QFile::link(outside.filePath("train_config.json"), dir.filePath("finished_models/external/recovery/train_config.json")));
    const QString source = "finished_models/folder with ' quote/recovery";
    writeFile(dir.filePath(source + "/train_config.json"), "{\"modelname\":\"declared_name\",\"recovery_head\":true,\"recovery_only\":false}");
    writeFile(dir.filePath(source + "/best.pth"), "saved weights");
    writeFile(dir.filePath(source + "/tuning/tuned_config.json"), "{\"modelname\":\"declared_name\",\"recovery_head\":true,\"recovery_only\":false}");
    writeFile(dir.filePath(source + "/best_tuned.pth"), "saved tuned weights");
    // Published recovery workflows both publish best; intermediate tuning/fold
    // checkpoints are never catalogued, and names do not imply capability.
    writeFile(dir.filePath("finished_models/heart/recovery/train_config.json"), "{\"modelname\":\"heart_recovery\",\"recovery_head\":true,\"recovery_only\":false}");
    writeFile(dir.filePath("finished_models/heart/recovery/best.pth"), "published");
    writeFile(dir.filePath("finished_models/heart/lopo_recovery/train_config.json"), "{\"modelname\":\"heart_lopo_recovery\",\"recovery_head\":true,\"recovery_only\":true,\"cv_method\":\"nested_recovery_lopo\"}");
    writeFile(dir.filePath("finished_models/heart/lopo_recovery/best.pth"), "published");
    writeFile(dir.filePath("finished_models/heart/frozen_recovery/train_config.json"), "{\"modelname\":\"heart_frozen_recovery\",\"recovery_head\":true,\"recovery_only\":true}");
    writeFile(dir.filePath("finished_models/heart/frozen_recovery/best.pth"), "published");
    writeFile(dir.filePath("finished_models/heart_legacy_recovery/train_config.json"), "{\"modelname\":\"heart_legacy_recovery\",\"recovery_head\":true}");
    writeFile(dir.filePath("finished_models/heart_legacy_recovery/best.pth"), "published");
    writeFile(dir.filePath("finished_models/z_normal/train_config.json"), "{\"modelname\":\"misleading_recovery\"}");
    writeFile(dir.filePath("finished_models/z_normal/best.pth"), "published");
    writeFile(dir.filePath("finished_models/heart/internal/outer/train_config.json"), "{\"modelname\":\"internal\"}");
    writeFile(dir.filePath("finished_models/heart/internal/outer/best.pth"), "intermediate");
    writeFile(dir.filePath("models/heart_lopo_recovery_cv/nested_recovery_lopo/best_tuned.pth"), "intermediate");
    writeFile(dir.filePath("finished_models/incomplete_recovery/train_config.json"), "{\"modelname\":\"incomplete_recovery\"}");
    writeFile(dir.filePath("finished_models/broken/train_config.json"), "not json");
    writeFile(dir.filePath("finished_models/broken/best.pth"), "weights");
    writeFile(dir.filePath("run/deploy_model.sh"), "#!/bin/bash\nexit 0\n", true);
    writeFile(dir.filePath("build_native/inference/native_uart_live_runtime"),
              "#!/bin/bash\nprintf '%s\\n' \"$*\" >> validations\nexit 0\n", true);
    writeFile(dir.filePath("config/native_uart_live.conf"),
              "input_reset_enabled=true\necg_baseline_reset_ms=100\nstuck_signal_reset_ms=100\n"
              "input_recovery_ms=100\nwatchdog_enabled=true\nuart_timeout_ms=100\nsample_timeout_ms=100\n");
    writeFile(dir.filePath("prepare_model.sh"), R"SH(#!/bin/bash
set -eu
# The GUI must hold the shared lock for the entire export pipeline.
if flock -n .jetson-control-deployment.lock true; then exit 90; fi
printf '%s\n' "$@" > prepare.args
if test -e fail-export; then exit 7; fi
mkdir -p deploy/model
printf 'engine' > deploy/model/model.engine
printf 'model_name=%s\nsource_model_dir=%s\nmodel_variant=%s\n' "$1" "$3" "$5" > deploy/model/model.conf
)SH", true);

    MainWindow window({.host = "fake", .remoteDirectory = dir.path()});
    auto *models = window.findChild<QComboBox *>("finishedModelSelection");
    auto *refresh = window.findChild<QPushButton *>("refreshFinishedModels");
    auto *reexport = window.findChild<QPushButton *>("reexportModel");
    auto *name = window.findChild<QLineEdit *>("modelName");
    auto *directory = window.findChild<QLineEdit *>("modelDirectory");
    auto *variant = window.findChild<QComboBox *>("modelVariant");
    auto *start = button(window, "Start direct inference");
    auto *prepare = button(window, "Prepare + activate model");
    auto *check = button(window, "Check availability");
    QVERIFY(models && refresh && reexport && name && directory && variant && start && prepare && check);
    auto *recoveryPolicy=window.findChild<QComboBox *>("recoveryDeploymentPolicy");
    QVERIFY(recoveryPolicy);
    QVERIFY(readFile(dir.filePath("ssh.calls")).isEmpty());
    refresh->click();
    QTRY_COMPARE_WITH_TIMEOUT(models->count(), 8, 5000); // Placeholder, best/tuned, three nested recovery kinds, legacy flat recovery, normal.
    QTRY_VERIFY_WITH_TIMEOUT(refresh->isEnabled(), 5000);
    for (int index = 3; index < 7; ++index) {
      QVERIFY(models->itemText(index).contains("_recovery"));
      QVERIFY(models->itemText(index).contains("Recovery supported"));
      QVERIFY(!models->itemText(index).contains("Fine-tuned"));
    }
    QVERIFY(models->itemText(3).contains("Joint-recovery"));
    QVERIFY(models->itemText(4).contains("Frozen-recovery"));
    QVERIFY(models->itemText(5).contains("LOPO-recovery"));
    QVERIFY(models->itemText(6).contains("training mode unknown"));
    QVERIFY(models->itemText(7).contains("Normal (single output)"));
    const auto beforeSelection = readFile(dir.filePath("ssh.calls"));
    models->setCurrentIndex(7);
    QVERIFY(QMetaObject::invokeMethod(models, "activated", Q_ARG(int, 7)));
    QVERIFY(!recoveryPolicy->isEnabled());
    models->setCurrentIndex(2);
    QVERIFY(QMetaObject::invokeMethod(models, "activated", Q_ARG(int, 2)));
    QCOMPARE(name->text(), QString("declared_name"));
    QCOMPARE(directory->text(), source);
    QCOMPARE(variant->currentText(), QString("tuned"));
    QVERIFY(recoveryPolicy->isEnabled());
    QTest::qWait(200);
    QCOMPARE(readFile(dir.filePath("ssh.calls")), beforeSelection);
    QVERIFY(!reexport->isEnabled());
    check->click();
    QTRY_VERIFY_WITH_TIMEOUT(reexport->isEnabled(), 5000);
    QVERIFY(!prepare->isEnabled()); // Existing-ONNX mode cannot work yet.
    QVERIFY(!start->isEnabled());
    recoveryPolicy->setCurrentIndex(1); // Explicit deployment draft only.
    reexport->click();
    QTRY_VERIFY_WITH_TIMEOUT(refresh->isEnabled(), 5000);
    QVERIFY(!start->isEnabled());
    const auto afterReexport = readFile(dir.filePath("ssh.calls"));
    QTest::qWait(150);
    QCOMPARE(readFile(dir.filePath("ssh.calls")), afterReexport);
    check->click();
    QTRY_VERIFY_WITH_TIMEOUT(start->isEnabled(), 5000); // Only the explicit Check refreshes readiness.
    QCOMPARE(readFile(dir.filePath("prepare.args")),
             "declared_name\n--model-dir\n" + source.toUtf8() + "\n--variant\ntuned\n--no-recovery\n");
    QVERIFY(readFile(dir.filePath("validations")).contains("--check-engine"));
    QCOMPARE(models->currentIndex(), 2);

    // Existing-ONNX preparation forwards the same policy without a Python
    // discovery dependency. Complete preparation was exercised above.
    writeFile(dir.filePath(source + "/declared_name_best_tuned.onnx"), "onnx");
    check->click();
    QTRY_VERIFY_WITH_TIMEOUT(prepare->isEnabled(), 5000);
    prepare->click();
    QTRY_VERIFY_WITH_TIMEOUT(refresh->isEnabled(), 5000);
    QCOMPARE(readFile(dir.filePath("prepare.args")),
             "declared_name\n--model-dir\n" + source.toUtf8() + "\n--variant\ntuned\n--jetson-only\n--no-recovery\n");
    check->click();
    QTRY_VERIFY_WITH_TIMEOUT(reexport->isEnabled(), 5000);

    // A running service prevents export, even after a stale successful check.
    writeFile(dir.filePath("service-state"), "active\n");
    QFile::remove(dir.filePath("prepare.args"));
    reexport->click();
    QTRY_VERIFY_WITH_TIMEOUT(refresh->isEnabled(), 5000);
    QVERIFY(!QFile::exists(dir.filePath("prepare.args")));
    QVERIFY(!start->isEnabled());

    // Failed export leaves readiness invalid and never queues a follow-up check.
    writeFile(dir.filePath("service-state"), "inactive\n");
    check->click();
    QTRY_VERIFY_WITH_TIMEOUT(reexport->isEnabled(), 5000);
    writeFile(dir.filePath("fail-export"), "fail");
    reexport->click();
    QTRY_VERIFY_WITH_TIMEOUT(refresh->isEnabled(), 5000);
    QVERIFY(!start->isEnabled());
    QVERIFY(!reexport->isEnabled());
#endif
  }
};

QTEST_MAIN(ModelDeploymentTest)
#include "test_model_deployment.moc"
