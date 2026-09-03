// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#pragma once

#include <QString>

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
};

[[nodiscard]] QString
validateDiagnosticOptions(const DiagnosticRuntimeOptions &options);
[[nodiscard]] QString diagnosticManifestPath();
[[nodiscard]] QString
prepareModelCommand(const DiagnosticRuntimeOptions &options);
[[nodiscard]] QString
validateRuntimeCommand(const DiagnosticRuntimeOptions &options);
[[nodiscard]] QString
startDirectRuntimeCommand(const DiagnosticRuntimeOptions &options);
[[nodiscard]] QString stopDirectRuntimeCommand();
[[nodiscard]] QString
availabilityProbeCommand(const DiagnosticRuntimeOptions &options);
[[nodiscard]] QString rebuildRuntimeCommand();
