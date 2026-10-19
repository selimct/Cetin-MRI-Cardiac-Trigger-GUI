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
    const QString source = "finished_models/folder with ' quote";
    writeFile(dir.filePath(source + "/train_config.json"), "{\"modelname\":\"declared_name\"}");
    writeFile(dir.filePath(source + "/best.pth"), "saved weights");
    writeFile(dir.filePath(source + "/tuning/tuned_config.json"), "{\"modelname\":\"declared_name\"}");
    writeFile(dir.filePath(source + "/best_tuned.pth"), "saved tuned weights");
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
    QVERIFY(readFile(dir.filePath("ssh.calls")).isEmpty());
    refresh->click();
    QTRY_COMPARE_WITH_TIMEOUT(models->count(), 3, 5000); // Placeholder + best + tuned.
    QTRY_VERIFY_WITH_TIMEOUT(refresh->isEnabled(), 5000);
    const auto beforeSelection = readFile(dir.filePath("ssh.calls"));
    models->setCurrentIndex(2);
    QVERIFY(QMetaObject::invokeMethod(models, "activated", Q_ARG(int, 2)));
    QCOMPARE(name->text(), QString("declared_name"));
    QCOMPARE(directory->text(), source);
    QCOMPARE(variant->currentText(), QString("tuned"));
    QTest::qWait(200);
    QCOMPARE(readFile(dir.filePath("ssh.calls")), beforeSelection);
    QVERIFY(!reexport->isEnabled());
    check->click();
    QTRY_VERIFY_WITH_TIMEOUT(reexport->isEnabled(), 5000);
    QVERIFY(!prepare->isEnabled()); // Existing-ONNX mode cannot work yet.
    QVERIFY(!start->isEnabled());
    reexport->click();
    QTRY_VERIFY_WITH_TIMEOUT(refresh->isEnabled(), 5000);
    QVERIFY(!start->isEnabled());
    QCOMPARE(readFile(dir.filePath("ssh.calls")).count('\n'), beforeSelection.count('\n') + 2);
    check->click();
    QTRY_VERIFY_WITH_TIMEOUT(start->isEnabled(), 5000); // Only the explicit Check refreshes readiness.
    QCOMPARE(readFile(dir.filePath("prepare.args")),
             "declared_name\n--model-dir\n" + source.toUtf8() + "\n--variant\ntuned\n");
    QVERIFY(readFile(dir.filePath("validations")).contains("--check-engine"));
    QCOMPARE(models->currentIndex(), 2);

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
