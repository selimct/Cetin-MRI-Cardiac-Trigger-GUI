// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#include "threshold_controller.h"
#include "threshold_panel.h"
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QDateTime>

namespace
{
    void writeFile(const QString &path, const QByteArray &bytes, bool executable = false)
    {
        QFile file(path);
        QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(file.errorString()));
        QCOMPARE(file.write(bytes), bytes.size());
        if (executable)
            QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    }
    QByteArray readFile(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return file.readAll();
    }
}
class ThresholdControllerTest : public QObject
{
    Q_OBJECT
private slots:
    void sshPanelAndJournalLifecycle()
    {
#ifdef Q_OS_WIN
        QSKIP("Fake Linux SSH host (portable parsing/reconciliation tested separately)");
#else
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto bin = dir.filePath("bin");
        QVERIFY(QDir().mkpath(bin));
        const auto oldPath = qgetenv("PATH");
        const auto oldData = qgetenv("XDG_DATA_HOME");
        qputenv("PATH", bin.toUtf8() + ':' + oldPath);
        qputenv("XDG_DATA_HOME", dir.path().toUtf8());
        struct RestoreEnvironment
        {
            QByteArray path, data;
            ~RestoreEnvironment()
            {
                qputenv("PATH", path);
                qputenv("XDG_DATA_HOME", data);
            }
        } restore{oldPath, oldData};
        const auto config = dir.filePath("runtime.conf");
        const auto manifest = dir.filePath("model.conf");
        const auto policy = dir.filePath("policy.conf");
        const QByteArray original = "model_name=heart\nmodel_variant=best\nengine_size_bytes=123\nengine_crc32=abcdef01\n"
                                    "threshold=0.246\nfp_threshold=0.40812345678901234\nder_threshold=0.246\nmanual_threshold=\n";
        writeFile(manifest, original);
        writeFile(config, "manifest_path=" + manifest.toUtf8() + "\nthreshold_override_path=" + policy.toUtf8() +
                              "\nearly_emission=true\nstatus_interval_ms=5000\npaused_status_interval_ms=60000\n");
        QProcess runtime;
        runtime.start("bash", {"-c", "while :; do sleep 1; done", "fake-native", "--config", config});
        QVERIFY(runtime.waitForStarted());
        struct StopRuntime
        {
            QProcess &process;
            ~StopRuntime()
            {
                process.terminate();
                process.waitForFinished();
            }
        } stop{runtime};
        writeFile(dir.filePath("invocation"), "11111111111111111111111111111111\n");
        writeFile(dir.filePath("properties"), "WorkingDirectory=" + dir.path().toUtf8() + "\nBindReadOnlyPaths=\nRootDirectory=\n"
                                                                                          "InvocationID=11111111111111111111111111111111\nMainPID=" +
                                                  QByteArray::number(runtime.processId()) +
                                                  "\nUser=" + qgetenv("USER") + "\nGroup=" + qgetenv("USER") + "\nLogNamespace=test\nNeedDaemonReload=no\n");
        // The test transport runs the GUI's exact commands against this isolated directory.
        // Remove only the remote login-shell behavior so the fake host tools stay on PATH.
        writeFile(bin + "/ssh", "#!/bin/bash\nlast=${@: -1}\nlast=${last/exec bash -lc/exec bash -c}\nexec bash -c \"$last\"\n", true);
        writeFile(bin + "/sudo", "#!/bin/bash\nwhile [[ $1 != -- ]]; do shift; done\nshift\nexec \"$@\"\n", true);
        writeFile(bin + "/systemctl", "#!/bin/bash\nif [[ $* == *--value* ]]; then\n"
                                      "if [[ $* == *InvocationID* ]]; then cat invocation; else echo " +
                                          QByteArray::number(runtime.processId()) + "; fi\nelse\ncat properties\nfi\n",
                  true);
        writeFile(bin + "/journalctl", "#!/bin/bash\nif [[ -e journal_error ]]; then exit 1; fi\ncat journal\n", true);
        writeFile(bin + "/date", "#!/bin/bash\nif [[ -e clock_override ]]; then cat clock_override; else exec /usr/bin/date \"$@\"; fi\n", true);
        writeFile(dir.filePath("journal"), {});
        int sequence = 0;
        auto event = [&](const QString &code, QJsonObject fields, QString invocation = "11111111111111111111111111111111", qint64 time = 0)
        {
            QTest::qWait(3); // strictly after the file operation's microsecond boundary
            fields.insert("schema", "dkfz-live-diagnostic-v1");
            fields.insert("code", code);
            const QJsonObject envelope{{"_SYSTEMD_INVOCATION_ID", invocation},
                                       {"__REALTIME_TIMESTAMP", QString::number(time ? time : QDateTime::currentMSecsSinceEpoch() * 1000)},
                                       {"__CURSOR", "cursor-" + QString::number(++sequence)},
                                       {"MESSAGE", QString::fromUtf8(QJsonDocument(fields).toJson(QJsonDocument::Compact))}};
            QFile journal(dir.filePath("journal"));
            QVERIFY(journal.open(QIODevice::Append));
            journal.write(QJsonDocument(envelope).toJson(QJsonDocument::Compact) + '\n');
        };
        auto status = [&](double active, bool manual, double configured, bool early, bool paused = false)
        {
            event("runtime_status", {{"threshold", active}, {"manual_override_configured", manual}, {"configured_threshold", configured}, {"early_emission", early}, {"emission_threshold", early ? std::max(active, 0.40812345678901234) : active}, {"input_paused", paused}});
        };
        const SshConnectionOptions connection{.host = "fake", .remoteDirectory = dir.path()};
        ThresholdController controller;
        controller.configure(connection, {});
        ThresholdPanel panel(&controller);
        auto *input = panel.findChild<QLineEdit *>("customThreshold");
        auto *apply = panel.findChild<QPushButton *>("applyThreshold");
        auto *checkbox = panel.findChild<QCheckBox *>("earlyEmission");
        QVERIFY(input);
        QVERIFY(apply);
        QVERIFY(checkbox);
        status(0.246, false, 0.246, true);
        controller.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(controller.ready(), 5000);
        QCOMPARE(*controller.readback().active, 0.246);
        input->setText("0.1"); // No write on editing.
        QCOMPARE(readFile(manifest), original);
        input->setText("NaN");
        apply->click();
        QCOMPARE(readFile(manifest), original);
        QVERIFY(controller.status().contains("finite"));
        // Same-valued DER is explicitly saved and needs fresh status, without a change event.
        controller.apply("der_preset");
        QTRY_VERIFY_WITH_TIMEOUT(readFile(manifest).contains("manual_threshold=0.246"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(controller.status() == "Saved/Pending", 5000);
        QVERIFY(controller.blocking());
        event("manual_threshold_configured", {{"manual_override_configured", true}, {"configured_threshold", 0.246}});
        controller.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(controller.status() == "Saved/Pending", 5000);
        QVERIFY(controller.status() != "Applied");
        status(0.246, true, 0.246, true);
        controller.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(controller.status(), QString("Applied"), 5000);
        QCOMPARE(controller.savedSource(), QString("Manual (DER preset)"));
        const auto records = QJsonDocument::fromJson(readFile(controller.auditPath())).array();
        QVERIFY(records.size() >= 3);
        QCOMPARE(records.last().toObject().value("result").toString(), QString("Applied"));
        // Identical repeat: historical/wrong-invocation status cannot acknowledge it.
        controller.apply("der_preset");
        QTRY_COMPARE_WITH_TIMEOUT(controller.status(), QString("Saved/Pending"), 5000);
        event("runtime_status", {{"threshold", 0.246}, {"manual_override_configured", true}, {"configured_threshold", 0.246}, {"early_emission", true}}, "22222222222222222222222222222222");
        controller.refresh();
        QTest::qWait(200);
        QVERIFY(controller.status() != "Applied");
        status(0.246, true, 0.246, true);
        controller.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(controller.status(), QString("Applied"), 5000);
        // Checkbox-only operation writes a six-field policy, retaining manual contents.
        const auto beforeCheckbox = readFile(manifest);
        checkbox->click();
        QTRY_VERIFY_WITH_TIMEOUT(readFile(policy).contains("early_emission=false"), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(controller.status(), QString("Saved/Pending"), 5000);
        QCOMPARE(readFile(manifest), beforeCheckbox);
        QCOMPARE(thresholds::settings(readFile(policy)).size(), 6);
        status(0.246, true, 0.246, false);
        controller.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(controller.status(), QString("Applied"), 5000);
        // Reset a stale fallback first, then clear manual; preserve the saved checkbox choice.
        auto stalePolicy = readFile(policy);
        stalePolicy.replace("threshold=0.246", "threshold=0.9");
        writeFile(policy, stalePolicy);
        controller.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(controller.savedPolicy().contains("0.9"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(controller.ready(), 5000);
        controller.restoreAutomatic();
        QTRY_COMPARE_WITH_TIMEOUT(controller.status(), QString("Saved/Pending"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.manifest().manual, 5000);
        QVERIFY(readFile(policy).contains("threshold=0.246\n"));
        QVERIFY(readFile(policy).contains("early_emission=false\n"));
        status(0.246, false, 0.246, false);
        controller.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(controller.status(), QString("Applied"), 5000);
        // An earlier equal-valued preset record cannot restore provenance across a reset.
        writeFile(manifest, thresholds::Manifest::parse(readFile(manifest)).patchManual("0.246"));
        controller.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(controller.manifest().manual.has_value(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(controller.ready(), 5000);
        QCOMPARE(controller.savedSource(), QString("Manual (source unknown)"));
        // Paused input leaves the previous active policy visible.
        input->setText("0.1");
        apply->click();
        QTRY_COMPARE_WITH_TIMEOUT(controller.status(), QString("Saved/Pending"), 5000);
        status(0.246, true, 0.1, false, true);
        controller.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(controller.status().startsWith("Waiting for input"), 5000);
        QCOMPARE(*controller.readback().active, 0.246);
        event("manual_threshold_rejected", {{"message", "Only manual_threshold may change"}});
        controller.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(controller.status().startsWith("Rejected"), 5000);
        QCOMPARE(*controller.manifest().manual, 0.1);
        QCOMPARE(*controller.readback().active, 0.246);
        // Reconnect restores source only from the matching durable GUI audit.
        ThresholdController reconnected;
        reconnected.configure(connection, {});
        reconnected.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(reconnected.ready(), 5000);
        QCOMPARE(reconnected.savedSource(), QString("Manual (custom)"));
        writeFile(manifest, thresholds::Manifest::parse(readFile(manifest)).patchManual("0.37"));
        reconnected.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(reconnected.manifest().manual && *reconnected.manifest().manual == 0.37, 5000);
        QCOMPARE(reconnected.savedSource(), QString("Manual (source unknown)"));
        // Missing status times out honestly and remains distinguishable from rejection.
        reconnected.apply("fp_preset");
        QTRY_COMPARE_WITH_TIMEOUT(reconnected.status(), QString("Saved/Pending"), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(reconnected.manifest().values.value("manual_threshold"), QString("0.40812345678901234"), 5000);
        writeFile(dir.filePath("clock_override"), QByteArray::number(QDateTime::currentMSecsSinceEpoch() * 1000 + 200000000));
        reconnected.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(reconnected.status().startsWith("Unconfirmed/stale"), 5000);
        QVERIFY(!reconnected.blocking());
        QFile::remove(dir.filePath("clock_override"));
        // A service restart cancels a pending operation and discards active confirmation.
        reconnected.apply("custom", "0.2");
        QTRY_COMPARE_WITH_TIMEOUT(reconnected.status(), QString("Saved/Pending"), 5000);
        auto props = readFile(dir.filePath("properties"));
        props.replace("11111111111111111111111111111111", "22222222222222222222222222222222");
        writeFile(dir.filePath("properties"), props);
        writeFile(dir.filePath("invocation"), "22222222222222222222222222222222\n");
        reconnected.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!reconnected.blocking(), 5000);
        QVERIFY(!reconnected.readback().active);
        QVERIFY(reconnected.status().startsWith("Unconfirmed/stale"));
        // Deploying a new model never copies the previous manual selection.
        auto newModel = original;
        newModel.replace("model_name=heart", "model_name=new-heart");
        writeFile(manifest, newModel);
        reconnected.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(reconnected.manifest().values.value("model_name"), QString("new-heart"), 5000);
        QVERIFY(!reconnected.manifest().manual);
        writeFile(dir.filePath("journal_error"), "fail");
        reconnected.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(reconnected.status().contains("status reporting unavailable"), 5000);
#endif
    }
};
QTEST_MAIN(ThresholdControllerTest)
#include "test_threshold_controller.moc"
