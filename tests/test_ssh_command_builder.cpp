// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#include "ssh_command_builder.h"

#include <QTest>

class SshCommandBuilderTest final : public QObject {
  Q_OBJECT

private slots:
  void quotesPosixShellValues();
  void buildsDestination();
  void buildsRemoteCommandWithDirectory();
  void buildsArgumentsForNonInteractiveSsh();
  void acceptsOnlyNewHostKeysWhenRequested();
  void omitsIdentityArgumentsWhenNotConfigured();
};

void SshCommandBuilderTest::quotesPosixShellValues() {
  QCOMPARE(posixShellQuote(QString()), QStringLiteral("''"));
  QCOMPARE(posixShellQuote(QStringLiteral("plain value")),
           QStringLiteral("'plain value'"));
  QCOMPARE(posixShellQuote(QStringLiteral("it's here")),
           QStringLiteral("'it'\"'\"'s here'"));
}

void SshCommandBuilderTest::buildsDestination() {
  SshConnectionOptions options{.host = QStringLiteral("jetson.local")};
  QCOMPARE(sshDestination(options), QStringLiteral("jetson.local"));

  options.user = QStringLiteral("orin");
  QCOMPARE(sshDestination(options), QStringLiteral("orin@jetson.local"));
}

void SshCommandBuilderTest::buildsRemoteCommandWithDirectory() {
  const SshConnectionOptions options{
      .host = QStringLiteral("jetson"),
      .remoteDirectory = QStringLiteral("/opt/dkfz live"),
  };

  QCOMPARE(remoteShellCommand(options, QStringLiteral("printf 'hello\\n'")),
           QStringLiteral("cd -- '/opt/dkfz live' && exec bash -lc "
                          "'printf '\"'\"'hello\\n'\"'\"''"));
}

void SshCommandBuilderTest::buildsArgumentsForNonInteractiveSsh() {
  const SshConnectionOptions options{
      .host = QStringLiteral("10.0.0.8"),
      .user = QStringLiteral("orin"),
      .port = 2202,
      .identityFile = QStringLiteral("/tmp/jetson key.pub"),
  };
  const QStringList arguments =
      sshArguments(options, QStringLiteral("uname -a"));

  QCOMPARE(arguments.first(), QStringLiteral("-T"));
  QVERIFY(arguments.contains(QStringLiteral("BatchMode=yes")));
  QVERIFY(arguments.contains(QStringLiteral("StrictHostKeyChecking=yes")));
  QVERIFY(arguments.contains(QStringLiteral("IdentitiesOnly=yes")));
  const int identityIndex = arguments.indexOf(QStringLiteral("-i"));
  QVERIFY(identityIndex >= 0);
  QCOMPARE(arguments.at(identityIndex + 1),
           QStringLiteral("/tmp/jetson key.pub"));
  QVERIFY(arguments.contains(QStringLiteral("2202")));
  QCOMPARE(arguments.at(arguments.size() - 2), QStringLiteral("orin@10.0.0.8"));
  QCOMPARE(arguments.last(), QStringLiteral("exec bash -lc 'uname -a'"));
}

void SshCommandBuilderTest::omitsIdentityArgumentsWhenNotConfigured() {
  const SshConnectionOptions options{.host = QStringLiteral("jetson")};
  const QStringList arguments =
      sshArguments(options, QStringLiteral("uname -a"));

  QVERIFY(!arguments.contains(QStringLiteral("-i")));
  QVERIFY(!arguments.contains(QStringLiteral("IdentitiesOnly=yes")));
}

void SshCommandBuilderTest::acceptsOnlyNewHostKeysWhenRequested() {
  const SshConnectionOptions options{
      .host = QStringLiteral("new-jetson"),
      .acceptNewHostKey = true,
  };
  const QStringList arguments =
      sshArguments(options, QStringLiteral("uname -a"));

  QVERIFY(arguments.contains(
      QStringLiteral("StrictHostKeyChecking=accept-new")));
  QVERIFY(!arguments.contains(QStringLiteral("StrictHostKeyChecking=yes")));
}

QTEST_APPLESS_MAIN(SshCommandBuilderTest)

#include "test_ssh_command_builder.moc"
