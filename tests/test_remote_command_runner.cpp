// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "remote_command_runner.h"
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

class RemoteCommandRunnerTest final : public QObject
{
    Q_OBJECT
  private slots:
    void cancellationTimeoutCannotKillNextCommand()
    {
#ifdef Q_OS_WIN
        QSKIP("Linux fake SSH process fixture");
#else
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto oldPath = qgetenv("PATH");
        struct Restore
        {
            QByteArray path;
            ~Restore() { qputenv("PATH", path); }
        } restore{oldPath};
        QFile ssh(dir.filePath("ssh"));
        QVERIFY(ssh.open(QIODevice::WriteOnly));
        ssh.write("#!/bin/bash\nexec sleep 30\n");
        ssh.close();
        QVERIFY(ssh.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("PATH", dir.path().toUtf8() + ':' + oldPath);
        RemoteCommandRunner runner;
        QSignalSpy completed(&runner, &RemoteCommandRunner::finished);
        const SshConnectionOptions connection{.host = "fake"};
        runner.start(connection, "first command");
        QVERIFY(runner.isRunning());
        QTest::qWait(50);
        runner.cancel();
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 5000);
        QVERIFY(completed.at(0).at(2).toBool());
        runner.start(connection, "second command");
        QTest::qWait(2300); // Cross the first command's cancellation deadline.
        QVERIFY(runner.isRunning());
        QCOMPARE(completed.count(), 1);
        runner.cancel();
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 2, 5000);
        QVERIFY(completed.at(1).at(2).toBool());
#endif
    }
};
QTEST_GUILESS_MAIN(RemoteCommandRunnerTest)
#include "test_remote_command_runner.moc"
