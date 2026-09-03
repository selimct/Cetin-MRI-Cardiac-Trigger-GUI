// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#include "connection_config.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

class ConnectionConfigTest final : public QObject {
  Q_OBJECT

private slots:
  void loadsRelativeIdentityFile();
  void rejectsMissingIdentityFile();
  void findsFileBesideBuildDirectory();
};

void ConnectionConfigTest::loadsRelativeIdentityFile() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());

  QFile key(directory.filePath(QStringLiteral("jetson_ed25519")));
  QVERIFY(key.open(QIODevice::WriteOnly));
  key.write("test key\n");
  key.close();

  QFile config(directory.filePath(QStringLiteral("connection.ini")));
  QVERIFY(config.open(QIODevice::WriteOnly));
  config.write("[ssh]\n"
               "host=192.0.2.10\n"
               "user=orin\n"
               "port=2202\n"
               "identity_file=jetson_ed25519\n"
               "remote_directory=/srv/live stack\n");
  config.close();

  const ConnectionConfigResult result = loadConnectionConfig(config.fileName());
  QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
  QVERIFY(result.found);
  QCOMPARE(result.options.host, QStringLiteral("192.0.2.10"));
  QCOMPARE(result.options.user, QStringLiteral("orin"));
  QCOMPARE(result.options.port, quint16{2202});
  QCOMPARE(result.options.identityFile, key.fileName());
  QCOMPARE(result.options.remoteDirectory, QStringLiteral("/srv/live stack"));
}

void ConnectionConfigTest::rejectsMissingIdentityFile() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());

  QFile config(directory.filePath(QStringLiteral("connection.ini")));
  QVERIFY(config.open(QIODevice::WriteOnly));
  config.write("[ssh]\nhost=jetson\nidentity_file=missing_key\n");
  config.close();

  const ConnectionConfigResult result = loadConnectionConfig(config.fileName());
  QVERIFY(!result.found);
  QVERIFY(result.error.contains(QStringLiteral("not readable")));
}

void ConnectionConfigTest::findsFileBesideBuildDirectory() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("build")));

  QFile config(directory.filePath(QStringLiteral("connection.ini")));
  QVERIFY(config.open(QIODevice::WriteOnly));
  config.write("[ssh]\nhost=jetson\n");
  config.close();

  QCOMPARE(findDefaultConnectionConfig(
               directory.filePath(QStringLiteral("build")),
               directory.filePath(QStringLiteral("elsewhere"))),
           config.fileName());
}

QTEST_APPLESS_MAIN(ConnectionConfigTest)

#include "test_connection_config.moc"
