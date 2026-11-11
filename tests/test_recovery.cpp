// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#include "console_output_classifier.h"
#include "diagnostic_command_builder.h"
#include "recovery_controller.h"
#include "recovery_panel.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>

namespace
{
QByteArray manifest(bool dual = true)
{
    return "model_name=heart\nmodel_variant=best\nengine_size_bytes=123\nengine_crc32=abcdef01\nthreshold=0."
           "3\nmanual_threshold=\n"
           "source_model_dir=finished_models/heart/recovery\nengine_path=custom.engine\nframe_size_raw=5\nraw_hz=500\nrecovery_head=" +
           QByteArray(dual ? "true" : "false") + "\nrecovery_threshold=\n";
}
QJsonObject source()
{
    return {{"modelname", "heart"},
            {"recovery_head", true},
            {"recovery_threshold", 0.92},
            {"recovery_only", true},
            {"primary_weights_frozen", true},
            {"primary_weights_verified_unchanged", false},
            {"cv_method", "nested_recovery_lopo"},
            {"num_epochs", 35},
            {"selected_fixed_epoch", 17},
            {"recovery_max_fp_increase", 0.1},
            {"base_patient_holdout_verified", false},
            {"evaluation_scope", "recovery_head_conditional_on_frozen_pretrained_base"},
            {"source_checkpoint_sha256", "base-sha256"},
            {"recovery_validation_metrics",
             QJsonObject{{"early_emission", QJsonObject{{"emission_threshold", 0.4},
                                                        {"fp_budget", 3},
                                                        {"baseline_fp", 2},
                                                        {"baseline_fn", 9},
                                                        {"selected", QJsonObject{{"fp", 3}, {"fn", 8}}}}}}}};
}
void write(const QString &path, const QByteArray &bytes, bool executable = false)
{
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(file.errorString()));
    QCOMPARE(file.write(bytes), bytes.size());
    file.close();
    if (executable)
        QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
}
QByteArray read(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}
QJsonObject api(const recovery::State &s, bool enabled = false, QJsonValue active = QJsonObject{},
                QString invocation = "11111111111111111111111111111111")
{
    return {{"schema", recovery::schema},
            {"api_version", 1},
            {"identity", s.manifest.identity()},
            {"model_supports_recovery", s.supportsRecovery()},
            {"live_api_supported", true},
            {"revision", "revision-1"},
            {"service_invocation_id", invocation},
            {"source_revision", s.sourceRevision},
            {"source_metadata_available", true},
            {"source_predates_engine", true},
            {"selected_threshold", 0.97},
            {"requested",
             QJsonObject{{"enabled", enabled}, {"threshold", enabled ? QJsonValue(0.97) : QJsonValue()}}},
            {"active", active},
            {"confirmation", "pending"},
            {"request_id", "request-1"}};
}
} // namespace
class RecoveryTest final : public QObject
{
    Q_OBJECT
  private slots:
    void metadataAndProvenance()
    {
        recovery::State s;
        s.manifest = thresholds::Manifest::parse(manifest());
        s.source = source();
        s.sourceAvailable = true;
        QVERIFY(s.supportsRecovery());
        QVERIFY(!s.liveSupported());
        auto info = s.information();
        QVERIFY(s.auditPolicy().contains("Policy customized"));
        s.primaryEdited = true;
        QVERIFY(s.auditPolicy().contains("primary settings requested"));
        s.primaryEdited = false;
        QVERIFY(info.contains("10 ms"));
        QVERIFY(!info.contains("20 ms"));
        QVERIFY(info.contains("declared frozen"));
        QVERIFY(info.contains("verified unchanged against source: no"));
        QVERIFY(info.contains("conditional on the frozen pretrained base"));
        QVERIFY(info.contains("not established as patient-unseen"));
        QVERIFY(info.contains("CV-selected fixed epoch: 17"));
        QVERIFY(info.contains("Saved training length (not inferred best epoch): 35"));
        QVERIFY(info.contains("baseline FP: 2"));
        QVERIFY(info.contains("baseline FN: 9"));
        QVERIFY(info.contains("FP allowance: 10%"));
        QVERIFY(info.contains("not a live FP guarantee"));
        QVERIFY(info.contains("not deployed engine or recovery checkpoint"));
        QVERIFY(info.contains("policy customized"));
        s.source["primary_weights_verified_unchanged"] = true;
        QVERIFY(s.information().contains("verified unchanged against source: yes"));
        s.source.remove("primary_weights_verified_unchanged");
        s.source.remove("recovery_only");
        s.source.remove("selected_fixed_epoch");
        QVERIFY(s.information().contains("Training mode: unknown"));
        QVERIFY(s.information().contains("CV-selected fixed epoch: Unavailable"));
        QVERIFY(s.information().contains("verified unchanged against source: Unknown"));
        s.source["recovery_only"] = false;
        QVERIFY(s.information().contains("Training: joint"));
        s.source["recovery_enabled"] = false;
        QVERIFY(!s.sourceEnabled());
        QVERIFY(s.calibrated());
        s.source["recovery_threshold"] = QJsonValue();
        QVERIFY(!s.calibrated());
        QVERIFY(s.information().contains("Recovery disabled by calibration"));
        s.source["recovery_validation_metrics"] =
            QJsonObject{{"early_emission", QJsonObject{{"fp_budget", 3}}}};
        QVERIFY(s.information().contains("baseline FP: Unavailable; baseline FN: Unavailable"));
        s.manifest = thresholds::Manifest::parse(manifest(false));
        QVERIFY(s.information().contains("Recovery unavailable: single-output model"));
    }
    void modelKindsComeFromMetadata()
    {
        QCOMPARE(recovery::modelKind({{"modelname", "looks_like_lopo_recovery"}}),
                 QString("Normal (single output)"));
        QJsonObject model{{"recovery_head", true}, {"recovery_only", false}};
        QCOMPARE(recovery::modelKind(model), QString("Joint-recovery"));
        model["recovery_only"] = true;
        QCOMPARE(recovery::modelKind(model), QString("Frozen-recovery"));
        model["cv_method"] = "nested_recovery_lopo";
        QCOMPARE(recovery::modelKind(model), QString("LOPO-recovery"));
        model.remove("cv_method");
        model.remove("recovery_only");
        QCOMPARE(recovery::modelKind(model), QString("Recovery (training mode unknown)"));
    }
    void informationFollowsPrimaryPredictionTarget()
    {
        recovery::State s;
        auto delayed = manifest(false);
        delayed.replace("frame_size_raw=5", "frame_size_raw=4");
        delayed.replace("raw_hz=500", "raw_hz=400");
        delayed += "schema=dkfz-live-deployment-v2\nwindow_size=100\n"
                   "prediction_mode=delayed_100hz\nprediction_delay_frames=1\n";
        s.manifest = thresholds::Manifest::parse(delayed);
        // Deployed timing remains visible even when source metadata is unavailable.
        auto info = s.information();
        QVERIFY(info.contains("Previous-frame primary: one-frame lookahead = 10 ms"));
        QVERIFY(info.contains("Source calibration/provenance unavailable"));
        QVERIFY(!info.contains("Recovered events only"));

        s.source = {{"modelname", "heart"}, {"recovery_head", false}, {"recovery_only", false}};
        s.sourceAvailable = true;
        info = s.information();
        QVERIFY(info.contains("Recovery unavailable: single-output model"));
        QVERIFY(info.contains("Training: single-output previous-frame primary"));
        QVERIFY(!info.contains("Training: joint"));
        QVERIFY(!info.contains("Starting current-only reproduces a frozen primary detector"));

        s.manifest = thresholds::Manifest::parse(manifest(false));
        info = s.information();
        QVERIFY(info.contains("Current-frame primary: no configured lookahead"));
        QVERIFY(info.contains("Training: single-output current-frame primary"));
        QVERIFY(!info.contains("one-frame lookahead"));
        QVERIFY(!info.contains("Training: joint"));

        // Two-output models retain recovery-specific timing and training descriptions.
        s.manifest = thresholds::Manifest::parse(manifest());
        s.source = source();
        info = s.information();
        QVERIFY(info.contains("Recovered events only: one-frame lookahead = 10 ms"));
        QVERIFY(info.contains("Training: recovery only"));
        s.source["recovery_only"] = false;
        QVERIFY(s.information().contains("Training: joint"));
        auto missingTiming = manifest();
        missingTiming.replace("raw_hz=500\n", "");
        s.manifest = thresholds::Manifest::parse(missingTiming);
        QVERIFY(s.information().contains("Recovery lookahead duration unavailable"));
    }
    void thresholdValidationAndApiIdentity()
    {
        QCOMPARE(recovery::threshold("1"), 1.);
        QCOMPARE(recovery::threshold("0.92"), 0.92);
        for (const auto &invalid : {"0", "-0.1", "nan", "inf", "1.01", "", "0.3junk"})
        {
            bool rejected = false;
            try
            {
                recovery::threshold(invalid);
            }
            catch (const std::exception &)
            {
                rejected = true;
            }
            QVERIFY2(rejected, invalid);
        }
        recovery::State s;
        s.manifest = thresholds::Manifest::parse(manifest());
        auto response = api(s);
        QVERIFY(s.acceptApi(response));
        QVERIFY(s.liveSupported());
        QVERIFY(s.requested().contains("0.97"));
        QVERIFY(s.active().contains("Unknown"));
        response["active"] =
            QJsonObject{{"enabled", false}, {"threshold", QJsonValue()}, {"request_id", "request-1"}};
        QVERIFY(s.acceptApi(response));
        QVERIFY(s.active().contains("disabled"));
        QVERIFY(s.requested().contains("0.97")); // active null never erases saved selection
        response["api_version"] = 2;
        QVERIFY(!s.acceptApi(response));
        QVERIFY(!s.liveSupported());
        response = api(s);
        response["identity"] = QJsonObject{{"model_name", "other"}};
        QVERIFY(!s.acceptApi(response));
        response = api(s);
        response["live_api_supported"] = false;
        QVERIFY(s.acceptApi(response));
        QVERIFY(!s.liveSupported());
    }
    void rejectsMalformedApiReadback()
    {
        recovery::State state;
        state.manifest = thresholds::Manifest::parse(manifest());
        const auto valid =
            api(state, false,
                QJsonObject{{"enabled", false}, {"threshold", QJsonValue()}, {"request_id", "request-1"}});
        const QList<QJsonObject> invalid = {
            QJsonObject{{"service_invocation_id", "wrong"}},
            QJsonObject{{"revision", QJsonValue()}},
            QJsonObject{{"request_id", QJsonValue()}},
            QJsonObject{{"live_api_supported", "true"}},
            QJsonObject{{"api_version", 1.5}},
            QJsonObject{{"selected_threshold", 0}},
            QJsonObject{{"selected_threshold", 1.1}},
            QJsonObject{{"selected_threshold", "0.9"}},
            QJsonObject{{"requested", QJsonObject{{"enabled", true}, {"threshold", QJsonValue()}}}},
            QJsonObject{{"requested", QJsonObject{{"enabled", false}, {"threshold", 0.9}}}},
            QJsonObject{{"requested", QJsonObject{{"enabled", true}, {"threshold", 0.91}}}},
            QJsonObject{{"active", QJsonValue()}},
            QJsonObject{{"confirmation", QJsonValue()}},
            QJsonObject{{"confirmation", "confirmed"}, {"live_api_supported", false}}};
        for (const auto &patch : invalid)
        {
            auto response = valid;
            for (auto it = patch.begin(); it != patch.end(); ++it)
                response[it.key()] = it.value();
            QVERIFY(state.acceptApi(valid));
            QVERIFY(!state.acceptApi(response));
            QVERIFY(state.api.isEmpty());
            QVERIFY(!state.liveSupported());
            QVERIFY(state.active().startsWith("Unknown"));
        }
    }
    void restartKeepsSelectionSeparateFromStartupAndActive()
    {
        recovery::State state;
        auto deployed = manifest();
        deployed.replace("recovery_threshold=\n", "recovery_threshold=0.92\n");
        state.manifest = thresholds::Manifest::parse(deployed);
        state.source = source();
        state.sourceAvailable = state.sourceUnchanged = true;
        auto response = api(state, true);
        response["requested"] = QJsonObject{{"enabled", true}, {"threshold", 0.92}};
        response["request_id"] = "0";
        response["active"] = QJsonObject{{"enabled", true}, {"threshold", 0.92}, {"request_id", "0"}};
        response["confirmation"] = "confirmed";
        QVERIFY(state.acceptApi(response));
        QCOMPARE(state.calibrated().value(), 0.92);
        QVERIFY(state.requested().contains("Persistent startup policy: enabled; threshold 0.92"));
        QVERIFY(state.requested().contains("Selected recovery threshold (retained across restarts): 0.97"));
        QVERIFY(state.requested().contains("no invocation override"));
        QVERIFY(state.active().contains("effective threshold 0.92; confirmed"));
    }
    void preparationAndTelemetrySemantics()
    {
        DiagnosticRuntimeOptions options{
            .modelName = "heart", .modelDirectory = "finished_models/heart/recovery", .forceRecoveryOff = true,
            .bundleDirectory = "artifact/test-bundle", .validationInputs = {"data/one.parquet"}, .ecg3SourceConfirmed = true};
        QVERIFY(prepareModelCommand(options).endsWith("--no-recovery"));
        QVERIFY(prepareModelCommand(options).contains("--prepare-only"));
        options.preparationMode = "complete";
        QVERIFY(prepareModelCommand(options).endsWith("--no-recovery"));
        QVERIFY(reexportModelCommand(options).contains("--no-recovery"));
        options.forceRecoveryOff = false;
        QVERIFY(!prepareModelCommand(options).contains("--no-recovery"));
        const QJsonObject delayed{{"schema", "dkfz-live-probability-v1"},
                                  {"probability", 0.9}, {"prediction_delay_frames", 1}};
        QVERIFY(recovery::telemetryDescription(delayed).contains("previous-frame primary"));
        QVERIFY(!recovery::telemetryDescription(delayed).contains("current-head"));
        QVERIFY(!finishedModelsCommand().contains("python"));
        auto probability = QJsonObject{{"schema", "dkfz-live-probability-v1"},
                                       {"probability", 0.2},
                                       {"recovery_prob", 0.95},
                                       {"trigger_emitted_now", true},
                                       {"recovery_emitted_now", true}};
        auto description = recovery::telemetryDescription(probability);
        QVERIFY(description.contains("current-head probability=0.2"));
        QVERIFY(description.contains("previous-frame recovery probability=0.95"));
        QVERIFY(description.contains("emitted on this record=yes"));
        auto event = QJsonObject{
            {"schema", "dkfz-live-trigger-v1"}, {"probability", 0.95}, {"emitted_on_time_ms", 100}};
        description = recovery::telemetryDescription(event);
        QVERIFY(description.contains("accepted event score=0.95"));
        QVERIFY(description.contains("head origin unknown"));
        auto line = QJsonDocument(event).toJson(QJsonDocument::Compact) + '\n';
        auto display = serviceLogDisplayLine(line);
        QVERIFY(display.contains("emitted_on_time_ms\":100"));
        QVERIFY(display.contains("origin unknown"));
        QVERIFY(recovery::telemetryDescription({{"code", "input_resumed"}}).isEmpty());
        QCOMPARE(runtimeInputEventForConsoleLine(QJsonDocument(probability).toJson(QJsonDocument::Compact)),
                 RuntimeInputEvent::none);
    }
    void sshLifecycleAndPanel()
    {
#ifdef Q_OS_WIN
        QSKIP("Fake Linux SSH transport requires bash/coreutils");
#else
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto oldPath = qgetenv("PATH");
        struct Restore
        {
            QByteArray path;
            ~Restore() { qputenv("PATH", path); }
        } restore{oldPath};
        const auto bin = dir.filePath("bin");
        qputenv("PATH", bin.toUtf8() + ':' + oldPath);
        write(bin + "/ssh",
              "#!/bin/bash\nprintf 'ssh\\n' >> " + posixShellQuote(dir.filePath("ssh.calls")).toUtf8() +
                  "\nif test -e " + posixShellQuote(dir.filePath("read-failure")).toUtf8() +
                  "; then echo 'Host unavailable' >&2; exit 7; fi\nlast=${@: -1}\nlast=${last/exec bash "
                  "-lc/exec bash -c}\nexec bash -c \"$last\"\n",
              true);
        write(bin + "/sudo", "#!/bin/bash\nwhile [[ $1 != -- ]]; do shift; done\nshift\nexec \"$@\"\n", true);
        // Fail if runtime discovery or metadata accidentally acquires a Python dependency.
        write(bin + "/python3", "#!/bin/bash\necho 'Python unavailable on target' >&2\nexit 127\n", true);
        write(dir.filePath("deploy/model/model.conf"), manifest());
        write(dir.filePath("finished_models/heart/recovery/train_config.json"), QJsonDocument(source()).toJson());
        write(dir.filePath("deploy/model/custom.engine"), "engine");
        RecoveryController c;
        c.configure({.host = "fake", .remoteDirectory = dir.path()}, {});
        RecoveryPanel p(&c);
        const auto refresh = [&]
        {
            c.refresh();
            QTRY_VERIFY_WITH_TIMEOUT(!c.blocking(), 5000);
        };
        QVERIFY(read(dir.filePath("ssh.calls")).isEmpty());
        refresh();
        QVERIFY(c.state().supportsRecovery());
        QVERIFY(!c.ready());
        QVERIFY(c.status().contains("API v1 is absent"));
        QVERIFY(c.state().sourceAvailable);
        QVERIFY(c.state().sourceUnchanged); // engine_path is relative to the manifest, not the checkout.
        QVERIFY(c.state().information().contains("10 ms"));
        auto *deployment = p.findChild<QComboBox *>("recoveryDeploymentPolicy");
        const auto calls = read(dir.filePath("ssh.calls"));
        deployment->setCurrentIndex(1);
        QVERIFY(p.forceRecoveryOff());
        QCOMPARE(read(dir.filePath("ssh.calls")), calls);
        p.findChild<QPushButton *>("restoreRecoveryDeployment")->click();
        QVERIFY(!p.forceRecoveryOff());
        auto *apply = p.findChild<QPushButton *>("applyRecovery");
        QVERIFY(!apply->isEnabled());
        write(dir.filePath("build_native/inference/native_live_recovery_control"), R"SH(#!/bin/bash
if [[ $* == *--status* ]]; then cat api.json
else printf '%s\n' "$@" > mutation.args; cat mutation.json; fi
)SH",
              true);
        auto response =
            api(c.state(), false,
                QJsonObject{{"enabled", false}, {"threshold", QJsonValue()}, {"request_id", "request-1"}});
        response["confirmation"] = "confirmed";
        write(dir.filePath("api.json"), QJsonDocument(response).toJson());
        write(dir.filePath("mutation.json"), QJsonDocument(response).toJson());
        refresh();
        QVERIFY(c.ready());
        QVERIFY(apply->isEnabled());
        auto *custom = p.findChild<QLineEdit *>("customRecoveryThreshold");
        auto *mode = p.findChild<QComboBox *>("recoveryThresholdMode");
        auto *enabled = p.findChild<QCheckBox *>("recoveryEnabled");
        QCOMPARE(mode->currentIndex(), 2); // Enable/disable preserves the backend's last selection.
        QCOMPARE(custom->text(), QString::number(0.97, 'g', 17));
        QVERIFY(!enabled->isChecked());
        enabled->setChecked(true);
        apply->click();
        QTRY_VERIFY_WITH_TIMEOUT(!c.blocking(), 5000);
        QVERIFY(read(dir.filePath("mutation.args")).startsWith("--enable\n--json\n"));
        QVERIFY(!read(dir.filePath("mutation.args")).contains("--threshold"));
        enabled->setChecked(false);
        apply->click();
        QTRY_VERIFY_WITH_TIMEOUT(!c.blocking(), 5000);
        QVERIFY(read(dir.filePath("mutation.args")).startsWith("--disable\n--json\n"));
        QVERIFY(!read(dir.filePath("mutation.args")).contains("--threshold"));
        QFile::remove(dir.filePath("mutation.args"));
        mode->setCurrentIndex(1);
        custom->setText("0");
        apply->click();
        QVERIFY(!QFile::exists(dir.filePath("mutation.args")));
        QVERIFY(c.status().contains("(0, 1]"));
        custom->setText("0.96");
        enabled->setChecked(true);
        const auto original = read(dir.filePath("deploy/model/model.conf"));
        apply->click();
        QTRY_VERIFY_WITH_TIMEOUT(!c.blocking(), 5000);
        QVERIFY(c.status().contains("pending"));
        QVERIFY(c.state().api.value("confirmation") != "confirmed");
        QVERIFY(c.state().active().contains("Unknown"));
        QVERIFY(!c.state().active().contains("; confirmed;"));
        QVERIFY(read(dir.filePath("mutation.args")).contains("--enable\n--threshold\n0.95999999999999996"));
        QVERIFY(read(dir.filePath("mutation.args")).contains("--expected-revision\nrevision-1"));
        QCOMPARE(read(dir.filePath("deploy/model/model.conf")), original);
        // Explicit refresh preserves the draft, saved selection and off active state separately.
        response["confirmation"] = "pending — input paused";
        write(dir.filePath("api.json"), QJsonDocument(response).toJson());
        bool draftInterrupted = false;
        const auto observe = connect(&c, &RecoveryController::changed, &p,
                                     [&] { draftInterrupted = draftInterrupted || !custom->isEnabled(); });
        refresh();
        disconnect(observe);
        QVERIFY(!draftInterrupted);
        QCOMPARE(custom->text(), QString("0.96"));
        QVERIFY(c.status().contains("paused"));
        QVERIFY(c.state().active().contains("disabled"));
        QVERIFY(c.state().requested().contains("0.97"));
        // Restoration honors the backend's source validation as well as our
        // byte-stable read, and does not confuse current-only startup with null calibration.
        response["source_predates_engine"] = false;
        write(dir.filePath("api.json"), QJsonDocument(response).toJson());
        refresh();
        QVERIFY(!p.findChild<QPushButton *>("restoreRecovery")->isEnabled());
        QVERIFY(c.state().sourceAvailable);
        response["source_predates_engine"] = true;
        write(dir.filePath("api.json"), QJsonDocument(response).toJson());
        refresh();
        QVERIFY(p.findChild<QPushButton *>("restoreRecovery")->isEnabled());
        p.findChild<QPushButton *>("restoreRecovery")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!c.blocking(), 5000);
        QVERIFY(read(dir.filePath("mutation.args")).startsWith("--restore-calibrated\n--json\n"));
        QCOMPARE(read(dir.filePath("deploy/model/model.conf")), original);
        const auto idle = read(dir.filePath("ssh.calls"));
        QTest::qWait(120);
        QCOMPARE(read(dir.filePath("ssh.calls")), idle);
        // A failed read drops stale model/active information but preserves an
        // edited draft for the same endpoint and engine on the next refresh.
        write(dir.filePath("read-failure"), "fail");
        refresh();
        QVERIFY(c.state().manifest.contents.isEmpty());
        QVERIFY(!c.state().sourceAvailable);
        QVERIFY(c.state().api.isEmpty());
        QCOMPARE(custom->text(), QString("0.96"));
        QFile::remove(dir.filePath("read-failure"));
        refresh();
        QCOMPARE(custom->text(), QString("0.96"));
        QVERIFY(enabled->isChecked());
        // Simulate a valid JSON file changing immediately after it is read.
        write(bin + "/cat",
              "#!/bin/bash\n/usr/bin/cat \"$@\"\nlast=${@: -1}\n"
              "if [[ $last == */train_config.json && -e mutate-source ]]; then printf '\\n' >> \"$last\"; rm "
              "mutate-source; fi\n",
              true);
        write(dir.filePath("mutate-source"), "once");
        refresh();
        QVERIFY(!c.state().sourceAvailable);
        QVERIFY(!c.state().sourceUnchanged);
        QVERIFY(c.state().information().contains("unavailable"));
        response["source_revision"] = "changed-source-revision";
        write(dir.filePath("api.json"), QJsonDocument(response).toJson());
        refresh();
        QVERIFY(!c.state().sourceAvailable);
        QVERIFY(c.status().contains("Calibration unconfirmed"));
        // Invalid/stale deployment responses and unsupported versions never enable controls.
        response["identity"] = QJsonObject{{"model_name", "new-selection"}};
        write(dir.filePath("api.json"), QJsonDocument(response).toJson());
        refresh();
        QVERIFY(!c.ready());
        QVERIFY(!apply->isEnabled());
        c.configure({.host = "different", .remoteDirectory = dir.path()}, {});
        QVERIFY(c.state().manifest.contents.isEmpty());
        QVERIFY(c.state().api.isEmpty());
        // Source JSON from another model cannot be attached to this deployment.
        auto wrong = source();
        wrong["modelname"] = "another";
        write(dir.filePath("finished_models/heart/recovery/train_config.json"), QJsonDocument(wrong).toJson());
        refresh();
        QVERIFY(!c.state().sourceAvailable);
        // A known single-output selection disables startup and live recovery
        // controls without changing the model or issuing an SSH mutation.
        write(dir.filePath("deploy/model/model.conf"), manifest(false));
        QFile::remove(dir.filePath("build_native/inference/native_live_recovery_control"));
        refresh();
        QVERIFY(!c.state().supportsRecovery());
        p.setDeploymentRecoverySupport(false);
        QVERIFY(!deployment->isEnabled());
        QVERIFY(!p.forceRecoveryOff());
        QVERIFY(!apply->isEnabled());
        QVERIFY(!enabled->isEnabled());
        QVERIFY(!mode->isEnabled());
        QVERIFY(!custom->isEnabled());
#endif
    }
};
QTEST_MAIN(RecoveryTest)
#include "test_recovery.moc"
