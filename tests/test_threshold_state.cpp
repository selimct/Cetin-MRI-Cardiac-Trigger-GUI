// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#include "threshold_state.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#ifndef Q_OS_WIN
#include <unistd.h>
#endif

using namespace thresholds;
namespace
{
    QByteArray fixture()
    {
        return "# calibrated model\nmodel_name=heart\nmodel_variant=best\nengine_size_bytes=123\nengine_crc32=abcdef01\n"
               "threshold=0.246\nfp_threshold=0.40812345678901234\nder_threshold=0.246\n"
               "manual_threshold=  # operator selection\nother_setting=preserved # comment\n";
    }
    void write(const QString &path, const QByteArray &bytes)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size());
    }
    QByteArray read(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return file.readAll();
    }
    QString sha(const QByteArray &bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(); }
}

class ThresholdStateTest : public QObject
{
    Q_OBJECT
private slots:
    void customAndPresetsPreserveManifest()
    {
        const auto m = Manifest::parse(fixture());
        auto expected = fixture();
        expected.replace("manual_threshold=  #", "manual_threshold=0  #");
        QCOMPARE(m.patchManual("0"), expected);
        QCOMPARE(m.numericChoice("fp_preset", "999"), QString("0.40812345678901234"));
        QCOMPARE(m.numericChoice("der_preset", "999"), QString("0.246"));
        const auto selected = Manifest::parse(m.patchManual(m.numericChoice("der_preset", {})));
        QVERIFY(selected.manual.has_value());
        QCOMPARE(*selected.manual, selected.automatic);
        QCOMPARE(sourceLabel("der_preset"), QString("Manual (DER preset)"));
        const auto values = selected.values;
        QCOMPARE(values.value("threshold"), m.values.value("threshold"));
        QCOMPARE(values.value("fp_threshold"), m.values.value("fp_threshold"));
        QCOMPARE(values.value("der_threshold"), m.values.value("der_threshold"));
        QCOMPARE(selected.patchManual(""), fixture());
        auto absent = fixture();
        absent.replace("manual_threshold=  # operator selection\n", "");
        const auto appended = Manifest::parse(absent).patchManual("1");
        QVERIFY(appended.startsWith(absent));
        QVERIFY(appended.endsWith("manual_threshold=1\n"));
    }
    void missingAndInvalidNumbers()
    {
        auto older = fixture();
        older.replace("fp_threshold=0.40812345678901234", "fp_threshold=");
        older.replace("der_threshold=0.246\n", "");
        const auto m = Manifest::parse(older);
        QVERIFY(!m.fp);
        QVERIFY(!m.der);
        QVERIFY(!m.manual);
        QCOMPARE(m.numericChoice("custom", "0"), QString("0"));
        QVERIFY_EXCEPTION_THROWN(m.numericChoice("fp_preset", "0"), std::runtime_error);
        for (const auto &invalid : {"NaN", "nan", "inf", "Infinity", "1.01", "-0.01", "0.1abc", "", " 0.1", "0x1", "1,0"})
            QVERIFY_EXCEPTION_THROWN(probability(invalid), std::runtime_error);
        QCOMPARE(probability("1"), 1.0);
        QCOMPARE(probability("1e-5"), 0.00001);
        QVERIFY_EXCEPTION_THROWN(Manifest::parse(fixture() + "threshold=0.2\n"), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(Manifest::parse(fixture() + "[section]\n"), std::runtime_error);
        auto malformed = older;
        malformed.replace("fp_threshold=", "fp_threshold=NaN");
        QVERIFY_EXCEPTION_THROWN(Manifest::parse(malformed), std::runtime_error);
    }
    void policyIsIndependentAndExact()
    {
        const auto m = Manifest::parse(Manifest::parse(fixture()).patchManual("0.2"));
        const auto bytes = m.policy(m.values.value("manual_threshold"), false);
        const auto fields = settings(bytes);
        QCOMPARE(fields.size(), 6);
        QCOMPARE(fields.value("threshold"), QString("0.2"));
        QCOMPARE(fields.value("early_emission"), QString("false"));
        QCOMPARE(fields.value("engine_crc32"), QString("abcdef01"));
        QVERIFY(bytes.size() <= 1024);
        QVERIFY(!bytes.contains('#'));
        QCOMPARE(*m.manual, 0.2);
        auto paddedSize = fixture();
        paddedSize.replace("engine_size_bytes=123", "engine_size_bytes=000123");
        const auto normalized = Manifest::parse(paddedSize);
        QCOMPARE(normalized.identity().value("engine_size_bytes").toString(), QString("123"));
        QVERIFY(normalized.policy("0", true).contains("engine_size_bytes=123\n"));
        auto old = fixture();
        old.replace("engine_crc32=abcdef01\n", "");
        QVERIFY_EXCEPTION_THROWN(Manifest::parse(old).policy("0.2", true), std::runtime_error);
    }
    void readbackRequiresFreshAcceptedAndActive()
    {
        Readback r;
        r.observe({{"code", "runtime_starting"}, {"threshold", 0.246}, {"manual_override_configured", true}}, 100, "start");
        QVERIFY(!r.matches(0.246, true, {}, 0));
        r.observe({{"code", "threshold_changed"}, {"threshold", 0.246}, {"early_emission", true}}, 200, "active");
        QVERIFY(!r.matches(0.246, true, {}, 100));
        r.observe({{"code", "manual_threshold_configured"}, {"manual_override_configured", true}, {"configured_threshold", 0.246}}, 210, "accepted");
        QVERIFY(r.matches(0.246, true, true, 100));
        QVERIFY(!r.matches(0.246, true, false, 100));
        QVERIFY(!r.matches(0.246, false, {}, 100));
        QVERIFY(!r.matches(0.246, true, {}, 210));
        // DER == automatic and identical repeats need only fresh runtime_status.
        const QJsonObject status{{"code", "runtime_status"}, {"threshold", 0.246}, {"early_emission", false}, {"manual_override_configured", true}, {"configured_threshold", 0.246}, {"input_paused", true}};
        r.observe(status, 300, "status");
        QVERIFY(r.matches(0.246, true, false, 250));
        QVERIFY(r.paused);
        r.observe({{"code", "manual_threshold_rejected"}, {"message", "Only manual_threshold may change"}}, 310, "rejected");
        QCOMPARE(*r.active, 0.246);
        QCOMPARE(r.message, QString("Only manual_threshold may change"));
    }
    void hostWritesAndFailures()
    {
#ifdef Q_OS_WIN
        QSKIP("Linux host filesystem contract");
#else
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto manifest = dir.filePath("model.conf");
        const auto config = dir.filePath("runtime.conf");
        const auto policy = dir.filePath("policy.conf");
        const auto bin = dir.filePath("bin");
        QDir().mkpath(bin);
        write(bin + "/systemctl", "#!/bin/sh\nprintf '11111111111111111111111111111111\\n'\n");
        QFile::setPermissions(bin + "/systemctl", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        write(config, "manifest_path=model.conf\n");
        write(manifest, fixture());
        QFile::setPermissions(manifest, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup);
        auto run = [&](const Manifest &m, const QString &action, const QString &number, const QString &policyFile = QString())
        {
            const auto p = policyFile.isEmpty() ? policy : policyFile;
            const auto script = checkedWrite(m, manifest, p, QFile::exists(p) ? sha(read(p)) : "missing", config,
                                             sha(read(config)), "11111111111111111111111111111111", "test.service", action, number, true,
                                             QString::number(getuid()), QString::number(getgid()));
            QProcess process;
            auto env = QProcessEnvironment::systemEnvironment();
            env.insert("PATH", bin + ':' + env.value("PATH"));
            process.setProcessEnvironment(env);
            process.start("bash", {"-c", script});
            if (!process.waitForFinished())
                return QPair<int, QByteArray>(-1, process.errorString().toUtf8());
            return QPair<int, QByteArray>(process.exitCode(), process.readAllStandardOutput() + process.readAllStandardError());
        };
        auto result = run(Manifest::parse(read(manifest)), "custom", "0.17");
        QVERIFY2(result.first == 0, result.second.constData());
        QCOMPARE(Manifest::parse(read(manifest)).manual.value(), 0.17);
        QVERIFY(QFile::permissions(manifest).testFlag(QFile::ReadGroup));
        const auto beforeCheckbox = read(manifest);
        result = run(Manifest::parse(beforeCheckbox), "checkbox", "0.17");
        QVERIFY2(result.first == 0, result.second.constData());
        QCOMPARE(read(manifest), beforeCheckbox);
        QCOMPARE(settings(read(policy)).size(), 6);
        const auto stale = Manifest::parse(read(manifest));
        write(manifest, beforeCheckbox + "# another client\n");
        result = run(stale, "custom", "0.9");
        QVERIFY(result.first != 0);
        QCOMPARE(read(manifest), beforeCheckbox + "# another client\n");
        // Reset step 1 cannot create a temporary file: retain the saved manual value.
        const auto beforeFailure = read(manifest);
        result = run(Manifest::parse(beforeFailure), "reset", "0.246", dir.filePath("missing/policy"));
        QVERIFY(result.first != 0);
        QCOMPARE(read(manifest), beforeFailure);
        // Inject a rename failure for the manifest, after the fallback has been replaced.
        write(bin + "/mv", "#!/bin/bash\nif [[ ${@: -1} == *model.conf ]]; then exit 71; fi\nexec /usr/bin/mv \"$@\"\n");
        QFile::setPermissions(bin + "/mv", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        result = run(Manifest::parse(beforeFailure), "reset", "0.246");
        QVERIFY(result.first != 0);
        QVERIFY(result.second.contains("POLICY_SAVED=1"));
        QCOMPARE(settings(read(policy)).value("threshold"), QString("0.246"));
        QCOMPARE(read(manifest), beforeFailure);
        QCOMPARE(QDir(dir.path()).entryList({"*.gui.*"}, QDir::Files).size(), 0);
        // Retry can safely complete both files.
        QFile::remove(bin + "/mv");
        result = run(Manifest::parse(read(manifest)), "reset", "0.246");
        QVERIFY2(result.first == 0, result.second.constData());
        QVERIFY(!Manifest::parse(read(manifest)).manual);
        QVERIFY(result.second.indexOf("POLICY_SAVED") < result.second.indexOf("MANIFEST_SAVED"));
#endif
    }
};

QTEST_GUILESS_MAIN(ThresholdStateTest)
#include "test_threshold_state.moc"
