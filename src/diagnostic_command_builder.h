// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

struct FinishedModel {
  QString name;
  QString directory;
  QString variant;
  bool recoverySupported{false};
  QString kind;
  QString predictionMode{"current"};
  int predictionDelayFrames{0};
  double frameHz{0};
  int windowFrames{0};
  double holdoffMs{-1};
  QString predictionDescription() const;
};

[[nodiscard]] QString finishedModelsCommand();
[[nodiscard]] QList<FinishedModel> parseFinishedModels(const QByteArray &output);

struct DiagnosticRuntimeOptions {
  QString modelName;
  QString modelDirectory;
  QString variant{"best"};
  QString preparationMode{"jetson-only"};
  QString outputMode{"none"};
  QString configPath{"config/native_uart_live.conf"};
  QString sessionLogPath;
  QString rawCapturePath;
  bool emitProbabilities{false};
  bool forceRecoveryOff{false};
  QString bundleDirectory;
  QStringList validationInputs;
  QString pythonExecutable{"/home/orin/envs/jet/bin/python"};
  bool ecg3SourceConfirmed{false};
};

[[nodiscard]] QString
validateDiagnosticOptions(const DiagnosticRuntimeOptions &options);
[[nodiscard]] QString validatePreparationOptions(const DiagnosticRuntimeOptions &options);
[[nodiscard]] QString activatePreparedModelCommand(const DiagnosticRuntimeOptions &options);
[[nodiscard]] QString diagnosticManifestPath();
[[nodiscard]] QString serviceUnitRepairCommand(bool sudoPasswordProvided);
[[nodiscard]] QString
prepareModelCommand(const DiagnosticRuntimeOptions &options);
[[nodiscard]] QString prepareStoppedModelCommand(const DiagnosticRuntimeOptions &options);
[[nodiscard]] QString
reexportModelCommand(const DiagnosticRuntimeOptions &options);
[[nodiscard]] QString
validateRuntimeCommand(const DiagnosticRuntimeOptions &options);
[[nodiscard]] QString
startDirectRuntimeCommand(const DiagnosticRuntimeOptions &options);
[[nodiscard]] QString stopDirectRuntimeCommand();
[[nodiscard]] QString
availabilityProbeCommand(const DiagnosticRuntimeOptions &options);
[[nodiscard]] QString rebuildRuntimeCommand();

[[nodiscard]] QString deploymentStopCommand(bool sudoPasswordProvided);
[[nodiscard]] QString deploymentStartCommand(bool sudoPasswordProvided,
    const QString &manifestRevision, const QString &configRevision, const QString &runtimeRevision);
