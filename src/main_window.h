// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#pragma once

#include "diagnostic_command_builder.h"
#include "remote_command_runner.h"
#include "threshold_controller.h"

#include <QElapsedTimer>
#include <QMainWindow>

class QCheckBox;
class QCloseEvent;
class QColor;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;

class MainWindow final : public QMainWindow {
  Q_OBJECT

public:
  explicit MainWindow(const SshConnectionOptions &initialConnection = {},
                      const QString &connectionFile = {},
                      QWidget *parent = nullptr);

protected:
  void closeEvent(QCloseEvent *event) override;

private:
  enum class TaskKind {
    none,
    ordinary,
    availabilityProbe,
    serviceMutation,
    serviceUnitRepair,
    serviceLogs,
    validateRuntime,
    prepareModel,
    rebuildRuntime,
    directInference,
  };

  struct RemoteCapabilities {
    bool known{false};
    bool service{false};
    bool serviceActive{false};
    bool serviceEnabled{false};
    bool serviceUnit{false};
    bool serviceLinkedToCheckout{false};
    bool serviceContract{false};
    bool serviceReady{false};
    bool runtime{false};
    bool serviceConfig{false};
    bool config{false};
    bool manifest{false};
    bool engine{false};
    bool activeSlotMetadata{false};
    bool activeSelectionMatches{false};
    bool directRuntimeReady{false};
    bool prepareScript{false};
    bool prepareInputs{false};
    bool rebuildScript{false};
    bool directInferenceActive{false};
    QString activeModel;
    QString activeVariant;
    QString activeSourceDirectory;
    QString serviceInputState;
    QString serviceState;
  };

  SshConnectionOptions connectionOptions() const;
  DiagnosticRuntimeOptions diagnosticOptions() const;
  QString remoteSudoCommand(const QString &command) const;
  void runCommand(const QString &label, const QString &command,
                  TaskKind kind = TaskKind::ordinary);
  void stopDirectInference();
  void appendConsole(const QString &text, const QColor &color);
  void appendConsoleBytes(const QByteArray &data, const QColor &color);
  void appendClassifiedConsoleBytes(QByteArray &pending,
                                    const QByteArray &data);
  void applyRuntimeInputEvent(const QByteArray &line);
  void flushClassifiedConsoleBytes(QByteArray &pending);
  void addCommandToHistory(const QString &command);
  void checkAvailability();
  void invalidateAvailability();
  void applyAvailabilityProbe(const QByteArray &output);
  void updateActionAvailability();
  void updateDiagnosticModeControls();
  void finishMainCommand(int exitCode, QProcess::ExitStatus exitStatus,
                         bool cancelled);

  RemoteCommandRunner runner_;
  RemoteCommandRunner controlRunner_;
  ThresholdController thresholds_;
  QElapsedTimer commandTimer_;
  QElapsedTimer controlTimer_;
  TaskKind activeTask_{TaskKind::none};
  QByteArray activeStandardOutput_;
  QByteArray activeStandardError_;
  QByteArray mainStandardOutputDisplayBuffer_;
  QByteArray mainStandardErrorDisplayBuffer_;
  QByteArray controlStandardOutputDisplayBuffer_;
  QByteArray controlStandardErrorDisplayBuffer_;
  RemoteCapabilities capabilities_;

  QLineEdit *hostEdit_{nullptr};
  QLineEdit *userEdit_{nullptr};
  QSpinBox *portSpin_{nullptr};
  QLineEdit *identityFileEdit_{nullptr};
  QLineEdit *remoteDirectoryEdit_{nullptr};
  QLineEdit *sudoPasswordEdit_{nullptr};
  QCheckBox *acceptNewHostKeyCheck_{nullptr};
  QPushButton *probeButton_{nullptr};
  QPushButton *connectionCheckButton_{nullptr};
  QPushButton *healthButton_{nullptr};

  QTabWidget *operationsTabs_{nullptr};
  QLabel *serviceAvailabilityLabel_{nullptr};
  QCheckBox *serviceSafetyCheck_{nullptr};
  QPushButton *serviceInstallButton_{nullptr};
  QPushButton *serviceStatusButton_{nullptr};
  QPushButton *serviceReloadButton_{nullptr};
  QPushButton *serviceStartButton_{nullptr};
  QPushButton *serviceStopButton_{nullptr};
  QPushButton *serviceRestartButton_{nullptr};
  QPushButton *serviceLogsButton_{nullptr};

  QLabel *diagnosticAvailabilityLabel_{nullptr};
  QLineEdit *modelNameEdit_{nullptr};
  QLineEdit *modelDirectoryEdit_{nullptr};
  QComboBox *variantCombo_{nullptr};
  QComboBox *preparationModeCombo_{nullptr};
  QComboBox *outputModeCombo_{nullptr};
  QLineEdit *configPathEdit_{nullptr};
  QLineEdit *sessionLogPathEdit_{nullptr};
  QLineEdit *rawCapturePathEdit_{nullptr};
  QCheckBox *emitProbabilitiesCheck_{nullptr};
  QCheckBox *gpioSafetyCheck_{nullptr};
  QPushButton *diagnosticCheckButton_{nullptr};
  QPushButton *prepareModelButton_{nullptr};
  QPushButton *validateRuntimeButton_{nullptr};
  QPushButton *startInferenceButton_{nullptr};
  QPushButton *stopInferenceButton_{nullptr};
  QPushButton *rebuildRuntimeButton_{nullptr};
  bool modelDirectoryFollowsName_{true};

  QComboBox *commandCombo_{nullptr};
  QPlainTextEdit *console_{nullptr};
  QLabel *statusLabel_{nullptr};
  QPushButton *runButton_{nullptr};
  QPushButton *cancelButton_{nullptr};
};
