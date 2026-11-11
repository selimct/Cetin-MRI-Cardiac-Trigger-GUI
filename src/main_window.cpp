// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#include "main_window.h"

#include "console_output_classifier.h"
#include "threshold_panel.h"
#include "recovery_panel.h"
#include "diagnostic_command_builder.h"
#include <QScrollArea>
#include <QShortcut>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QVBoxLayout>
#include <QWidget>

namespace {
constexpr auto kServiceName = "dkfz-native-uart-live";

QColor informationalColor() { return QColor(QStringLiteral("#4d9b58")); }

QColor attentionColor() { return QColor(QStringLiteral("#c28b27")); }

QColor elevatedColor() { return QColor(QStringLiteral("#d45b5b")); }

QPushButton *makeActionButton(const QString &text, QWidget *parent) {
  auto *button = new QPushButton(text, parent);
  button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  return button;
}

QString completionSummary(int exitCode, QProcess::ExitStatus exitStatus,
                          bool cancelled, qint64 elapsed) {
  if (cancelled) {
    return QStringLiteral("Cancelled after %1 ms").arg(elapsed);
  }
  if (exitStatus == QProcess::CrashExit) {
    return QStringLiteral("SSH process crashed after %1 ms").arg(elapsed);
  }
  return QStringLiteral("Finished with exit code %1 after %2 ms")
      .arg(exitCode)
      .arg(elapsed);
}

QColor completionColor(int exitCode, QProcess::ExitStatus exitStatus,
                       bool cancelled) {
  if (cancelled) {
    return QColor(QStringLiteral("#c28b27"));
  }
  if (exitStatus == QProcess::NormalExit && exitCode == 0) {
    return informationalColor();
  }
  return elevatedColor();
}
} // namespace

MainWindow::MainWindow(const SshConnectionOptions &initialConnection,
                       const QString &connectionFile, QWidget *parent)
    : QMainWindow(parent), runner_(this), controlRunner_(this) {
  setWindowTitle(QStringLiteral("Jetson Control"));
  resize(1120, 820);

  auto *central = new QWidget(this);
  auto *mainLayout = new QVBoxLayout(central);

  auto *connectionButton =
      new QPushButton(QStringLiteral("Connection"), central);
  connectionButton->setToolTip(
      QStringLiteral(
          "Edit SSH connection settings and run connection checks."));

  auto *connectionDialog = new QDialog(this);
  connectionDialog->setWindowTitle(QStringLiteral("Connection"));
  connectionDialog->setModal(false);
  connectionDialog->setMinimumWidth(620);
  auto *connectionOuterLayout = new QVBoxLayout(connectionDialog);
  auto *connectionLayout = new QFormLayout;

  hostEdit_ = new QLineEdit(connectionDialog);
  hostEdit_->setObjectName("connectionHost");
  hostEdit_->setPlaceholderText(QStringLiteral("jetson or 192.168.1.50"));
  userEdit_ = new QLineEdit(connectionDialog);
  userEdit_->setPlaceholderText(
      QStringLiteral("orin (optional when configured in SSH)"));
  portSpin_ = new QSpinBox(connectionDialog);
  portSpin_->setRange(1, 65535);
  portSpin_->setValue(initialConnection.port);
  identityFileEdit_ =
      new QLineEdit(initialConnection.identityFile, connectionDialog);
  identityFileEdit_->setPlaceholderText(
      QStringLiteral("absolute path to an SSH identity (optional)"));
  identityFileEdit_->setClearButtonEnabled(true);
  identityFileEdit_->setToolTip(QStringLiteral(
      "Absolute path to a private key, or to a public key whose matching "
      "private key is loaded in ssh-agent. A relative identity_file in "
      "connection.ini is resolved next to that file."));
  auto *identityFileButton =
      new QPushButton(QStringLiteral("Browse…"), connectionDialog);
  identityFileButton->setToolTip(
      QStringLiteral("Select an SSH identity and use its absolute path."));
  auto *identityFileRow = new QWidget(connectionDialog);
  auto *identityFileLayout = new QHBoxLayout(identityFileRow);
  identityFileLayout->setContentsMargins(0, 0, 0, 0);
  identityFileLayout->addWidget(identityFileEdit_, 1);
  identityFileLayout->addWidget(identityFileButton);
  remoteDirectoryEdit_ =
      new QLineEdit(initialConnection.remoteDirectory.isEmpty()
                        ? QStringLiteral("/home/orin/work/live_stack")
                        : initialConnection.remoteDirectory,
                    connectionDialog);
  remoteDirectoryEdit_->setToolTip(QStringLiteral(
      "The Jetson checkout path. It must match the source side of the "
      "service's BindReadOnlyPaths entry."));
  sudoPasswordEdit_ = new QLineEdit(connectionDialog);
  sudoPasswordEdit_->setEchoMode(QLineEdit::Password);
  sudoPasswordEdit_->setClearButtonEnabled(true);
  sudoPasswordEdit_->setPlaceholderText(
      QStringLiteral("optional; kept in memory until cleared or exit"));
  sudoPasswordEdit_->setToolTip(QStringLiteral(
      "Password for sudo on the remote Jetson. It is never saved or placed "
      "in a command; managed service actions send it over SSH standard "
      "input to sudo."));
  acceptNewHostKeyCheck_ = new QCheckBox(
      QStringLiteral("Trust and save a new host key on the next connection"),
      connectionDialog);
  acceptNewHostKeyCheck_->setToolTip(QStringLiteral(
      "Uses OpenSSH accept-new once. Unknown host keys are added to your "
      "known_hosts file, while changed host keys are still rejected."));

  hostEdit_->setText(initialConnection.host);
  userEdit_->setText(initialConnection.user);
  connectionLayout->addRow(QStringLiteral("Host"), hostEdit_);
  connectionLayout->addRow(QStringLiteral("User"), userEdit_);
  connectionLayout->addRow(QStringLiteral("Remote sudo password"),
                           sudoPasswordEdit_);
  connectionLayout->addRow(QStringLiteral("Port"), portSpin_);
  connectionLayout->addRow(QStringLiteral("SSH identity"), identityFileRow);
  connectionLayout->addRow(QStringLiteral("Remote project root"),
                           remoteDirectoryEdit_);
  connectionOuterLayout->addLayout(connectionLayout);
  connectionOuterLayout->addWidget(acceptNewHostKeyCheck_);

  probeButton_ = new QPushButton(QStringLiteral("Check availability"), central);
  probeButton_->setToolTip(QStringLiteral(
      "Run the availability check for the currently entered settings."));
  connectionCheckButton_ =
      makeActionButton(QStringLiteral("Check"), connectionDialog);
  connectionCheckButton_->setToolTip(QStringLiteral(
      "Run the availability check with the entered connection settings."));
  healthButton_ =
      makeActionButton(QStringLiteral("System information"), connectionDialog);
  auto *connectionActions = new QHBoxLayout;
  connectionActions->addWidget(connectionCheckButton_);
  connectionActions->addWidget(healthButton_);
  connectionOuterLayout->addLayout(connectionActions);

  auto *connectionDialogButtons =
      new QDialogButtonBox(QDialogButtonBox::Close, connectionDialog);
  connectionOuterLayout->addWidget(connectionDialogButtons);

  operationsTabs_ = new QTabWidget(central);
  operationsTabs_->setObjectName("operationsTabs");

  auto *servicePage = new QWidget(operationsTabs_);
  auto *serviceLayout = new QVBoxLayout(servicePage);
  serviceAvailabilityLabel_ = new QLabel(
      QStringLiteral(
          "Availability has not been checked. Service actions are disabled."),
      servicePage);
  serviceAvailabilityLabel_->setWordWrap(true);
  serviceAvailabilityLabel_->setObjectName("serviceState");
  serviceSafetyCheck_ =
      new QCheckBox(QStringLiteral("I reviewed the installed configuration and "
                                   "verified the connected hardware"),
                    servicePage);
  serviceSafetyCheck_->setObjectName("serviceHardwareReview");
  auto *serviceMaintenanceActions = new QHBoxLayout;
  serviceInstallButton_ =
      makeActionButton(QStringLiteral("Install / repair unit link"),
                       servicePage);
  serviceInstallButton_->setToolTip(QStringLiteral(
      "Link this checkout's deploy/dkfz-native-uart-live.service into "
      "/etc/systemd/system and reload systemd. The service must be stopped; "
      "this does not enable or start it."));
  serviceReloadButton_ =
      makeActionButton(QStringLiteral("Reload unit"), servicePage);
  serviceMaintenanceActions->addWidget(serviceInstallButton_);
  serviceMaintenanceActions->addWidget(serviceReloadButton_);
  auto *serviceActions = new QHBoxLayout;
  serviceStatusButton_ =
      makeActionButton(QStringLiteral("Status"), servicePage);
  serviceStartButton_ = makeActionButton(QStringLiteral("Start"), servicePage);
  serviceStopButton_ = makeActionButton(QStringLiteral("Stop"), servicePage);
  serviceRestartButton_ =
      makeActionButton(QStringLiteral("Restart"), servicePage);
  serviceLogsButton_ =
      makeActionButton(QStringLiteral("Follow log"), servicePage);
  serviceActions->addWidget(serviceStatusButton_);
  serviceActions->addWidget(serviceStartButton_);
  serviceActions->addWidget(serviceStopButton_);
  serviceActions->addWidget(serviceRestartButton_);
  serviceActions->addWidget(serviceLogsButton_);
  serviceLayout->addWidget(serviceAvailabilityLabel_);

  auto *modelDeploymentGroup = new QGroupBox(QStringLiteral("Model deployment"), servicePage);
  auto *finishedModelForm = new QFormLayout(modelDeploymentGroup);
  finishedModelForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  finishedModelCombo_ = new QComboBox(modelDeploymentGroup);
  finishedModelCombo_->setObjectName(QStringLiteral("finishedModelSelection"));
  finishedModelCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  finishedModelCombo_->setMinimumContentsLength(22);
  finishedModelCombo_->addItem(QStringLiteral("Select a finished model…"));
  finishedModelCombo_->setToolTip(QStringLiteral(
      "Models with saved weights and training configuration in the Jetson's "
      "finished_models directory. Select Refresh models or Check to load."));
  refreshModelsButton_ = new QPushButton(QStringLiteral("Refresh models"), modelDeploymentGroup);
  refreshModelsButton_->setObjectName(QStringLiteral("refreshFinishedModels"));
  auto *modelSelectionRow = new QHBoxLayout;
  modelSelectionRow->addWidget(finishedModelCombo_, 1);
  modelSelectionRow->addWidget(refreshModelsButton_);
  reexportModelButton_ = makeActionButton(
      QStringLiteral("Prepare isolated bundle"), modelDeploymentGroup);
  reexportModelButton_->setObjectName(QStringLiteral("reexportModel"));
  reexportModelButton_->setToolTip(QStringLiteral(
      "Copy saved weights into a new bundle, export ONNX, build TensorRT and validate recordings. "
      "Preparation does not activate files or verify accuracy or sustained timing. "
      "Requires the full Jetson Python environment and a built runtime. Stop "
      "the service and direct inference first."));
  finishedModelForm->addRow(QStringLiteral("Finished models"), modelSelectionRow);
  pythonExecutableEdit_ = new QLineEdit("/home/orin/envs/jet/bin/python", modelDeploymentGroup);
  pythonExecutableEdit_->setObjectName("jetsonPython");
  validationInputsEdit_ = new QPlainTextEdit(modelDeploymentGroup);
  validationInputsEdit_->setObjectName("validationInputs");
  validationInputsEdit_->setPlaceholderText("Automatic: existing p12/p13 raw ECG recordings.\nOverride: one remote parquet path per line.");
  validationInputsEdit_->setMaximumHeight(65);
  ecg3SourceCheck_ = new QCheckBox("I confirmed the model was trained with ECG3 = ECG2 − ECG1", modelDeploymentGroup);
  ecg3SourceCheck_->setObjectName("ecg3SourceConfirmed");
  preparationStateLabel_ = new QLabel("Preparation: not started", modelDeploymentGroup);
  preparationStateLabel_->setObjectName("preparationState");
  activationStateLabel_ = new QLabel("Activation: not started", modelDeploymentGroup);
  activationStateLabel_->setObjectName("activationState");
  for (auto *label : {preparationStateLabel_, activationStateLabel_}) {
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
  }
  activateModelButton_ = makeActionButton(QStringLiteral("Activate prepared bundle"), modelDeploymentGroup);
  activateModelButton_->setObjectName("activatePreparedBundle");
  activateModelButton_->setToolTip("Activate the prepared pair using its fingerprinted validation report. The service must already be idle. Starting the service is a separate action.");
  installModelButton_ = makeActionButton("Install selected model", modelDeploymentGroup);
  installModelButton_->setObjectName("installSelectedModel");
  installModelButton_->setToolTip("Convert saved PTH weights, validate the engine, activate the checked model and verify service readiness. The service must already be stopped and will stay stopped.");
  installationStateLabel_ = new QLabel("Select a saved model to install. The service stays stopped until Start.", modelDeploymentGroup);
  installationStateLabel_->setObjectName("installationState");
  installationStateLabel_->setWordWrap(true);
  installationStateLabel_->setTextFormat(Qt::PlainText);
  finishedModelForm->addRow(ecg3SourceCheck_);
  finishedModelForm->addRow(installModelButton_);
  finishedModelForm->addRow(installationStateLabel_);
  finishedModelForm->addRow(preparationStateLabel_);
  finishedModelForm->addRow(activationStateLabel_);
  auto *advancedButton = new QPushButton("Advanced model installation", modelDeploymentGroup);
  advancedButton->setObjectName("showAdvancedModelInstallation");
  advancedButton->setCheckable(true);
  auto *advancedSettings = new QWidget(modelDeploymentGroup);
  advancedSettings->setObjectName("advancedModelInstallation");
  auto *advancedForm = new QFormLayout(advancedSettings);
  advancedForm->addRow("Jetson Python", pythonExecutableEdit_);
  advancedForm->addRow("Validation recordings", validationInputsEdit_);
  auto *recordingHelp = new QLabel("Recorded ECG is replayed to check conversion parity. Empty uses existing data/live/p12 and p13 out recordings. These checks do not measure accuracy or sustained real-time performance.", advancedSettings);
  recordingHelp->setWordWrap(true);
  advancedForm->addRow(recordingHelp);
  advancedForm->addRow(reexportModelButton_);
  advancedForm->addRow(activateModelButton_);
  advancedSettings->hide();
  connect(advancedButton, &QPushButton::toggled, advancedSettings, &QWidget::setVisible);
  finishedModelForm->addRow(advancedButton);
  finishedModelForm->addRow(advancedSettings);
  serviceLayout->addWidget(modelDeploymentGroup);
  serviceLayout->addWidget(serviceSafetyCheck_);
  serviceLayout->addLayout(serviceMaintenanceActions);
  serviceLayout->addLayout(serviceActions);
  workerSchedulingLabel_ = new QLabel("Worker scheduling: unobserved. Follow log to read actual worker status.", servicePage);
  workerSchedulingLabel_->setObjectName("workerSchedulingStatus");
  workerSchedulingLabel_->setTextFormat(Qt::PlainText);
  workerSchedulingLabel_->setWordWrap(true);
  serviceLayout->addWidget(workerSchedulingLabel_);
  runtimeTimingLabel_ = new QLabel("Runtime timing: unobserved. Follow log for deadline and frame delay measurements.", servicePage);
  runtimeTimingLabel_->setObjectName("runtimeTimingStatus");
  runtimeTimingLabel_->setTextFormat(Qt::PlainText);
  runtimeTimingLabel_->setWordWrap(true);
  serviceLayout->addWidget(runtimeTimingLabel_);
  serviceLayout->addStretch();
  auto *serviceScroll = new QScrollArea(operationsTabs_);
  serviceScroll->setWidgetResizable(true);
  serviceScroll->setWidget(servicePage);
  operationsTabs_->addTab(serviceScroll, QStringLiteral("Installed service"));
  auto *detectorPage = new QWidget(operationsTabs_);
  auto *detectorLayout = new QVBoxLayout(detectorPage);
  auto *thresholdPanel = new ThresholdPanel(&thresholds_, detectorPage);
  recoveryPanel_ = new RecoveryPanel(&recovery_, detectorPage);
  finishedModelForm->insertRow(1, recoveryPanel_->takeDeploymentControls());
  detectorLayout->addWidget(thresholdPanel);
  detectorLayout->addWidget(recoveryPanel_);
  auto *detectorScroll = new QScrollArea(operationsTabs_);
  detectorScroll->setWidgetResizable(true);
  detectorScroll->setWidget(detectorPage);
  operationsTabs_->addTab(detectorScroll, QStringLiteral("Detector controls"));

  auto *diagnosticPage = new QWidget(operationsTabs_);
  auto *diagnosticLayout = new QVBoxLayout(diagnosticPage);
  diagnosticAvailabilityLabel_ = new QLabel(
      QStringLiteral(
          "Availability has not been checked. Direct actions are disabled."),
      diagnosticPage);
  diagnosticAvailabilityLabel_->setWordWrap(true);
  diagnosticCheckButton_ =
      new QPushButton(QStringLiteral("Check"), diagnosticPage);
  diagnosticCheckButton_->setToolTip(QStringLiteral(
      "Run the availability check with the entered model and runtime "
      "settings."));

  auto *diagnosticColumns = new QHBoxLayout;
  auto *deploymentForm = new QFormLayout;
  auto *runtimeForm = new QFormLayout;
  deploymentForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  runtimeForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  modelNameEdit_ = new QLineEdit(QStringLiteral("target_r"), diagnosticPage);
  modelNameEdit_->setObjectName(QStringLiteral("modelName"));
  modelNameEdit_->setToolTip(QStringLiteral(
      "The modelname declared by the selected training configuration."));
  modelDirectoryEdit_ =
      new QLineEdit(QStringLiteral("finished_models/target_r"), diagnosticPage);
  modelDirectoryEdit_->setObjectName(QStringLiteral("modelDirectory"));
  modelDirectoryEdit_->setToolTip(QStringLiteral(
      "Exact source directory beneath the remote project root. Use this for "
      "alternate roots such as models_noCV/target_r."));
  variantCombo_ = new QComboBox(diagnosticPage);
  variantCombo_->setObjectName(QStringLiteral("modelVariant"));
  variantCombo_->addItems({QStringLiteral("best"), QStringLiteral("tuned")});
  preparationModeCombo_ = new QComboBox(diagnosticPage);
  preparationModeCombo_->addItem(
      QStringLiteral("Isolated Jetson: PTH → ONNX → TensorRT"),
      QStringLiteral("jetson-only"));
  preparationModeCombo_->setToolTip(QStringLiteral(
      "Isolated preparation always rebuilds ONNX and TensorRT from saved weights on the Jetson."));
  outputModeCombo_ = new QComboBox(diagnosticPage);
  outputModeCombo_->addItems({QStringLiteral("none"), QStringLiteral("jsonl"),
                              QStringLiteral("gpio"),
                              QStringLiteral("gpio+jsonl")});
  outputModeCombo_->setToolTip(
      QStringLiteral("Use none for the safest live diagnostic. GPIO modes can "
                     "create physical pulses."));
  configPathEdit_ = new QLineEdit(
      QStringLiteral("config/native_uart_live.conf"), diagnosticPage);
  sessionLogPathEdit_ = new QLineEdit(diagnosticPage);
  sessionLogPathEdit_->setPlaceholderText(
      QStringLiteral("output/live-session.log (optional)"));
  sessionLogPathEdit_->setToolTip(
      QStringLiteral("Append stdout and stderr to this path beneath the remote "
                     "project root."));
  rawCapturePathEdit_ = new QLineEdit(diagnosticPage);
  rawCapturePathEdit_->setPlaceholderText(
      QStringLiteral("output/session.bin (optional)"));
  rawCapturePathEdit_->setToolTip(
      QStringLiteral("Raw UART byte capture. Empty explicitly disables "
                     "capture, overriding the config."));
  emitProbabilitiesCheck_ =
      new QCheckBox(QStringLiteral("Emit probability records"), diagnosticPage);
  emitProbabilitiesCheck_->setEnabled(false);
  gpioSafetyCheck_ =
      new QCheckBox(QStringLiteral("I verified the GPIO line, polarity, "
                                   "voltage, and connected hardware"),
                    diagnosticPage);
  gpioSafetyCheck_->setEnabled(false);

  modelTimingLabel_ = new QLabel(diagnosticPage);
  modelTimingLabel_->setObjectName(QStringLiteral("trainedPredictionContract"));
  modelTimingLabel_->setWordWrap(true);
  modelTimingLabel_->setTextFormat(Qt::PlainText);
  modelTimingLabel_->setToolTip(QStringLiteral(
      "The prediction target is fixed by training. Deploy separately trained 100 Hz weights; "
      "existing 50 Hz weights cannot be switched to a delayed target. "
      "Accuracy and sustained Jetson latency require validation."));
  deploymentForm->addRow(QStringLiteral("Trained prediction"), modelTimingLabel_);
  deploymentForm->addRow(QStringLiteral("Model name"), modelNameEdit_);
  deploymentForm->addRow(QStringLiteral("Model source directory"),
                         modelDirectoryEdit_);
  deploymentForm->addRow(QStringLiteral("Variant"), variantCombo_);
  deploymentForm->addRow(QStringLiteral("Jetson preparation"),
                         preparationModeCombo_);

  runtimeForm->addRow(QStringLiteral("Trigger output"), outputModeCombo_);
  runtimeForm->addRow(QStringLiteral("Runtime config"), configPathEdit_);
  runtimeForm->addRow(QStringLiteral("Combined session log"),
                      sessionLogPathEdit_);
  runtimeForm->addRow(QStringLiteral("Raw UART capture"), rawCapturePathEdit_);

  diagnosticColumns->addLayout(deploymentForm, 1);
  diagnosticColumns->addSpacing(18);
  diagnosticColumns->addLayout(runtimeForm, 1);

  auto *diagnosticChecks = new QHBoxLayout;
  diagnosticChecks->addWidget(emitProbabilitiesCheck_, 1);
  diagnosticChecks->addWidget(gpioSafetyCheck_, 1);

  auto *diagnosticActions = new QHBoxLayout;
  prepareModelButton_ = makeActionButton(
      QStringLiteral("Prepare isolated bundle"), diagnosticPage);
  prepareModelButton_->setToolTip(QStringLiteral(
      "Runs prepare_model.sh --prepare-only with recordings and a new output directory. The systemd service must be "
      "stopped."));
  validateRuntimeButton_ = makeActionButton(
      QStringLiteral("Validate config + engine"), diagnosticPage);
  startInferenceButton_ = makeActionButton(
      QStringLiteral("Start direct inference"), diagnosticPage);
  stopInferenceButton_ =
      makeActionButton(QStringLiteral("Stop direct inference"), diagnosticPage);
  rebuildRuntimeButton_ = makeActionButton(
      QStringLiteral("Build / rebuild runtime"), diagnosticPage);
  diagnosticActions->addWidget(prepareModelButton_);
  diagnosticActions->addWidget(validateRuntimeButton_);
  diagnosticActions->addWidget(startInferenceButton_);
  diagnosticActions->addWidget(stopInferenceButton_);
  diagnosticActions->addWidget(rebuildRuntimeButton_);

  auto *diagnosticAvailabilityRow = new QHBoxLayout;
  diagnosticAvailabilityRow->addWidget(diagnosticAvailabilityLabel_, 1);
  diagnosticAvailabilityRow->addWidget(diagnosticCheckButton_);
  diagnosticLayout->addLayout(diagnosticAvailabilityRow);
  diagnosticLayout->addLayout(diagnosticColumns);
  diagnosticLayout->addLayout(diagnosticChecks);
  diagnosticLayout->addLayout(diagnosticActions);
  operationsTabs_->addTab(diagnosticPage,
                          QStringLiteral("Model deployment / direct"));

  auto *commandGroup =
      new QGroupBox(QStringLiteral("Custom non-interactive command"), central);
  auto *commandLayout = new QHBoxLayout(commandGroup);
  commandCombo_ = new QComboBox(commandGroup);
  commandCombo_->setEditable(true);
  commandCombo_->setInsertPolicy(QComboBox::NoInsert);
  commandCombo_->lineEdit()->setPlaceholderText(
      QStringLiteral("Enter a command"));
  runButton_ = new QPushButton(QStringLiteral("Run"), commandGroup);
  cancelButton_ = new QPushButton(QStringLiteral("Cancel"), commandGroup);
  cancelButton_->setEnabled(false);
  auto *clearButton =
      new QPushButton(QStringLiteral("Clear output"), commandGroup);
  commandLayout->addWidget(commandCombo_, 1);
  commandLayout->addWidget(runButton_);
  commandLayout->addWidget(cancelButton_);
  commandLayout->addWidget(clearButton);

  console_ = new QPlainTextEdit(central);
  console_->setReadOnly(true);
  console_->setLineWrapMode(QPlainTextEdit::NoWrap);
  console_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  console_->document()->setMaximumBlockCount(20000);

  statusLabel_ = new QLabel(central);
  statusLabel_->setObjectName("operationStatus");
  const QString sshStatus = runner_.sshExecutable().isEmpty()
                                ? QStringLiteral("not found")
                                : runner_.sshExecutable();
  statusLabel_->setText(
      connectionFile.isEmpty()
          ? QStringLiteral(
                "Ready — SSH executable: %1; no connection.ini loaded")
                .arg(sshStatus)
          : QStringLiteral("Ready — loaded %1; SSH executable: %2")
                .arg(connectionFile, sshStatus));

  auto *topActions = new QHBoxLayout;
  topActions->addStretch();
  topActions->addWidget(probeButton_);
  topActions->addWidget(connectionButton);
  mainLayout->addLayout(topActions);
  mainLayout->addWidget(operationsTabs_);
  mainLayout->addWidget(commandGroup);
  mainLayout->addWidget(console_, 1);
  mainLayout->addWidget(statusLabel_);
  setCentralWidget(central);

  connect(connectionButton, &QPushButton::clicked, this,
          [connectionDialog] {
            connectionDialog->show();
            connectionDialog->raise();
            connectionDialog->activateWindow();
          });
  connect(connectionDialogButtons, &QDialogButtonBox::rejected,
          connectionDialog, &QDialog::reject);
  connect(identityFileButton, &QPushButton::clicked, this,
          [this, connectionDialog] {
            const QString currentPath = identityFileEdit_->text().trimmed();
            const QString selectedPath = QFileDialog::getOpenFileName(
                connectionDialog, QStringLiteral("Select SSH identity"),
                currentPath,
                QStringLiteral("SSH identity files (*)"));
            if (!selectedPath.isEmpty()) {
              identityFileEdit_->setText(
                  QFileInfo(selectedPath).absoluteFilePath());
            }
          });

  connect(probeButton_, &QPushButton::clicked, this,
          &MainWindow::checkAvailability);
  connect(connectionCheckButton_, &QPushButton::clicked, this,
          &MainWindow::checkAvailability);
  connect(diagnosticCheckButton_, &QPushButton::clicked, this,
          &MainWindow::checkAvailability);
  connect(healthButton_, &QPushButton::clicked, this, [this] {
    runCommand(QStringLiteral("System information"),
               QStringLiteral(
                   "printf '%s\\n' '--- system ---'; uname -a; "
                   "printf '%s\\n' '--- uptime ---'; uptime; "
                   "printf '%s\\n' '--- storage ---'; df -h .; "
                   "printf '%s\\n' '--- CUDA/TensorRT ---'; "
                   "command -v nvidia-smi >/dev/null && nvidia-smi || true; "
                   "if command -v trtexec >/dev/null; then command -v trtexec; "
                   "elif test -x /usr/src/tensorrt/bin/trtexec; then "
                   "printf '%s\\n' /usr/src/tensorrt/bin/trtexec; "
                   "else printf '%s\\n' 'trtexec not found'; fi"));
  });

  connect(serviceStatusButton_, &QPushButton::clicked, this, [this] {
    runCommand(
        QStringLiteral("Service status"),
        QStringLiteral("systemctl --no-pager --full status %1; "
                       "code=$?; test \"$code\" -eq 0 || test \"$code\" -eq 3")
            .arg(kServiceName));
  });
  connect(serviceInstallButton_, &QPushButton::clicked, this, [this] {
    if (capabilities_.serviceActive) {
      statusLabel_->setText(
          QStringLiteral("Stop the service before repairing its unit link."));
      return;
    }

    const QString remoteRoot = remoteDirectoryEdit_->text().trimmed();
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Install or repair unit link"),
        QStringLiteral(
            "Replace the installed systemd unit with a link to the deploy "
            "unit in:\n%1\n\nSystemd will be reloaded. The service will not "
            "be enabled or started.")
            .arg(remoteRoot),
        QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Ok) {
      return;
    }

    runCommand(
        QStringLiteral("Install or repair service unit link"),
        serviceUnitRepairCommand(!sudoPasswordEdit_->text().isEmpty()),
        TaskKind::serviceUnitRepair);
  });
  connect(serviceReloadButton_, &QPushButton::clicked, this, [this] {
    runCommand(
        QStringLiteral("Reload systemd unit"),
        remoteSudoCommand(QStringLiteral("systemctl daemon-reload")) +
            QStringLiteral(" && systemctl show --property=NeedDaemonReload %1")
                .arg(kServiceName),
        TaskKind::serviceMutation);
  });
  connect(serviceStartButton_, &QPushButton::clicked, this, [this] {
    runCommand(QStringLiteral("Start service"),
               deploymentStartCommand(!sudoPasswordEdit_->text().isEmpty(),
                   observedManifestRevision_, observedConfigRevision_, observedRuntimeRevision_),
               TaskKind::serviceMutation);
  });
  connect(serviceStopButton_, &QPushButton::clicked, this, [this] {
    runCommand(
        QStringLiteral("Stop service"),
        remoteSudoCommand(
            QStringLiteral("systemctl stop %1").arg(kServiceName)) +
            QStringLiteral(
                "; stop_code=$?; systemctl --no-pager --full status %1 || "
                "true; exit \"$stop_code\"")
                .arg(kServiceName),
        TaskKind::serviceMutation);
  });
  connect(serviceRestartButton_, &QPushButton::clicked, this, [this] {
    runCommand(QStringLiteral("Restart service"),
               remoteSudoCommand(
                   QStringLiteral("systemctl restart %1").arg(kServiceName)) +
                   QStringLiteral(" && systemctl --no-pager --full status %1")
                       .arg(kServiceName),
               TaskKind::serviceMutation);
  });
  connect(serviceLogsButton_, &QPushButton::clicked, this, [this] {
    runCommand(
        QStringLiteral("Follow service log"),
        QStringLiteral("journalctl --namespace=dkfz-live --follow --lines=100 "
                       "--output=cat --unit=%1")
            .arg(kServiceName),
        TaskKind::serviceLogs);
  });

  connect(installModelButton_, &QPushButton::clicked, this, [this] {
    if (!installModelButton_->isEnabled()) return;
    createPreparationDirectory();
    const auto options = diagnosticOptions();
    const auto error = validatePreparationOptions(options);
    if (!error.isEmpty()) { statusLabel_->setText(error); return; }
    bundlePrepared_ = false;
    installStage_ = "preparation";
    installationStateLabel_->setText("Installing selected model… The service will stay stopped.");
    activationStateLabel_->setText("Activation: waiting for preparation checks");
    runCommand("Install selected model", installModelCommand(options), TaskKind::installModel);
  });
  connect(activateModelButton_, &QPushButton::clicked, this, [this] {
    if (!activateModelButton_->isEnabled()) return;
    activationStateLabel_->setText("Activation: checking fingerprints and activating prepared files…");
    runCommand("Activate prepared bundle", activatePreparedModelCommand(diagnosticOptions()), TaskKind::activateModel);
  });
  connect(refreshModelsButton_, &QPushButton::clicked, this, [this] {
    runCommand(QStringLiteral("List finished models"), finishedModelsCommand(), TaskKind::listModels);
  });
  connect(finishedModelCombo_, qOverload<int>(&QComboBox::activated), this, [this](int index) {
    if (index <= 0 || index > finishedModels_.size()) return;
    const auto model = finishedModels_.at(index - 1);
    modelDirectoryFollowsName_ = false;
    modelNameEdit_->setText(model.name);
    modelDirectoryEdit_->setText(model.directory);
    variantCombo_->setCurrentText(model.variant);
    syncFinishedModelSelection();
    updateActionAvailability();
  });
  connect(reexportModelButton_, &QPushButton::clicked, this, [this] {
    createPreparationDirectory();
    auto options = diagnosticOptions();
    options.preparationMode = QStringLiteral("complete");
    const QString error = validatePreparationOptions(options);
    if (!error.isEmpty()) {
      statusLabel_->setText(error);
      return;
    }
    bundlePrepared_ = false;
    preparationStateLabel_->setText("Preparation: building isolated bundle and validating recordings…");
    runCommand(QStringLiteral("Prepare isolated model on Jetson"),
               reexportModelCommand(options), TaskKind::reexportModel);
  });
  connect(prepareModelButton_, &QPushButton::clicked, this, [this] {
    createPreparationDirectory();
    const auto options = diagnosticOptions();
    const QString error = validateDiagnosticOptions(options);
    if (!error.isEmpty()) {
      statusLabel_->setText(error);
      return;
    }
    if (!validatePreparationOptions(options).isEmpty()) { statusLabel_->setText(validatePreparationOptions(options)); return; }
    bundlePrepared_ = false;
    preparationStateLabel_->setText("Preparation: building isolated bundle and validating recordings…");
    runCommand(QStringLiteral("Prepare isolated model on Jetson"),
               prepareStoppedModelCommand(options), TaskKind::prepareModel);
  });
  connect(validateRuntimeButton_, &QPushButton::clicked, this, [this] {
    const auto options = diagnosticOptions();
    const QString error = validateDiagnosticOptions(options);
    if (!error.isEmpty()) {
      statusLabel_->setText(error);
      return;
    }
    runCommand(QStringLiteral("Validate direct runtime"),
               validateRuntimeCommand(options), TaskKind::validateRuntime);
  });
  connect(startInferenceButton_, &QPushButton::clicked, this, [this] {
    const auto options = diagnosticOptions();
    const QString error = validateDiagnosticOptions(options);
    if (!error.isEmpty()) {
      statusLabel_->setText(error);
      return;
    }
    if (options.outputMode.contains(QStringLiteral("gpio")) &&
        !gpioSafetyCheck_->isChecked()) {
      statusLabel_->setText(
          QStringLiteral("GPIO safety confirmation is required."));
      return;
    }
    runCommand(QStringLiteral("Direct inference"),
               startDirectRuntimeCommand(options), TaskKind::directInference);
  });
  connect(stopInferenceButton_, &QPushButton::clicked, this,
          &MainWindow::stopDirectInference);
  connect(rebuildRuntimeButton_, &QPushButton::clicked, this, [this] {
    runCommand(QStringLiteral("Build or rebuild runtime"),
               rebuildRuntimeCommand(), TaskKind::rebuildRuntime);
  });

  connect(runButton_, &QPushButton::clicked, this, [this] {
    const QString command = commandCombo_->currentText().trimmed();
    if (!command.isEmpty()) {
      addCommandToHistory(command);
    }
    runCommand(QStringLiteral("Custom command"), command);
  });
  connect(commandCombo_->lineEdit(), &QLineEdit::returnPressed, runButton_,
          &QPushButton::click);
  connect(cancelButton_, &QPushButton::clicked, this, [this] {
    if (activeTask_ == TaskKind::directInference) {
      stopDirectInference();
    } else {
      runner_.cancel();
    }
  });
  connect(clearButton, &QPushButton::clicked, console_, &QPlainTextEdit::clear);

  const auto invalidate = [this] { invalidatePreparedBundle(); invalidateAvailability(); };
  const auto invalidateConnection = [this] {
    sudoPasswordEdit_->clear();
    invalidatePreparedBundle();
    ecg3SourceCheck_->setChecked(false);
    workerScheduling_.clear();
    workerSchedulingLabel_->setText("Worker scheduling: unobserved for this connection");
    runtimeTimingLabel_->setText("Runtime timing: unobserved for this connection");
    applyFinishedModels({});
    invalidateAvailability();
  };
  connect(hostEdit_, &QLineEdit::textChanged, this, invalidateConnection);
  connect(userEdit_, &QLineEdit::textChanged, this, invalidateConnection);
  connect(portSpin_, qOverload<int>(&QSpinBox::valueChanged), this,
          invalidateConnection);
  connect(identityFileEdit_, &QLineEdit::textChanged, this,
          invalidateConnection);
  connect(remoteDirectoryEdit_, &QLineEdit::textChanged, this, [this, invalidate] {
    ecg3SourceCheck_->setChecked(false);
    workerScheduling_.clear();
    workerSchedulingLabel_->setText("Worker scheduling: unobserved for this checkout");
    runtimeTimingLabel_->setText("Runtime timing: unobserved for this checkout");
    applyFinishedModels({});
    invalidate();
  });
  connect(modelNameEdit_, &QLineEdit::textChanged, this,
          [this, invalidate](const QString &name) {
            ecg3SourceCheck_->setChecked(false);
            if (modelDirectoryFollowsName_) {
              modelDirectoryEdit_->setText(
                  QStringLiteral("finished_models/%1").arg(name.trimmed()));
            }
            invalidate();
          });
  connect(modelDirectoryEdit_, &QLineEdit::textEdited, this,
          [this] { modelDirectoryFollowsName_ = false; });
  connect(modelDirectoryEdit_, &QLineEdit::textChanged, this, [this, invalidate] {
    ecg3SourceCheck_->setChecked(false); invalidate();
  });
  connect(variantCombo_, &QComboBox::currentTextChanged, this, [this, invalidate] {
    ecg3SourceCheck_->setChecked(false); invalidate();
  });
  connect(modelNameEdit_, &QLineEdit::textChanged, this, &MainWindow::syncFinishedModelSelection);
  connect(modelDirectoryEdit_, &QLineEdit::textChanged, this, &MainWindow::syncFinishedModelSelection);
  connect(variantCombo_, &QComboBox::currentTextChanged, this, &MainWindow::syncFinishedModelSelection);
  connect(pythonExecutableEdit_, &QLineEdit::textChanged, this, [this] { invalidatePreparedBundle(); updateActionAvailability(); });
  connect(validationInputsEdit_, &QPlainTextEdit::textChanged, this, [this] { invalidatePreparedBundle(); updateActionAvailability(); });
  connect(ecg3SourceCheck_, &QCheckBox::toggled, this, [this] { invalidatePreparedBundle(); updateActionAvailability(); });
  connect(preparationModeCombo_,
          qOverload<int>(&QComboBox::currentIndexChanged), this,
          [invalidate] { invalidate(); });
  connect(configPathEdit_, &QLineEdit::textChanged, this, invalidate);
  connect(sessionLogPathEdit_, &QLineEdit::textChanged, this,
          [this] { updateActionAvailability(); });
  connect(rawCapturePathEdit_, &QLineEdit::textChanged, this, [this] {
    capabilities_.directRuntimeReady = false;
    if (capabilities_.known) {
      diagnosticAvailabilityLabel_->setText(QStringLiteral(
          "Direct runtime parameters changed. Validate again before starting "
          "inference."));
    }
    updateActionAvailability();
  });
  connect(emitProbabilitiesCheck_, &QCheckBox::toggled, this, [this] {
    capabilities_.directRuntimeReady = false;
    if (capabilities_.known) {
      diagnosticAvailabilityLabel_->setText(QStringLiteral(
          "Direct runtime parameters changed. Validate again before starting "
          "inference."));
    }
    updateActionAvailability();
  });
  connect(gpioSafetyCheck_, &QCheckBox::toggled, this,
          [this] { updateActionAvailability(); });
  connect(serviceSafetyCheck_, &QCheckBox::toggled, this,
          [this] { updateActionAvailability(); });
  connect(outputModeCombo_, &QComboBox::currentTextChanged, this,
          [this] { updateDiagnosticModeControls(); });

  connect(&runner_, &RemoteCommandRunner::started, this,
          [this](const QString &destination, const QString &command) {
            commandTimer_.restart();
            activeStandardOutput_.clear();
            activeStandardError_.clear();
            mainStandardOutputDisplayBuffer_.clear();
            mainStandardErrorDisplayBuffer_.clear();
            QString displayCommand = command;
            if (activeTask_ == TaskKind::availabilityProbe) {
              displayCommand = QStringLiteral("[availability preflight]");
            } else if (activeTask_ == TaskKind::listModels) {
              displayCommand = QStringLiteral("[list finished models]");
            } else if (activeTask_ == TaskKind::serviceUnitRepair) {
              displayCommand =
                  QStringLiteral("[install/repair service unit link]");
            } else if (activeTask_ == TaskKind::directInference) {
              displayCommand = QStringLiteral("[managed direct inference]");
            }
            appendConsole(
                QStringLiteral("\n[%1] %2 $ %3\n")
                    .arg(QDateTime::currentDateTime().toString(Qt::ISODate),
                         destination, displayCommand),
                QColor(QStringLiteral("#4f8dd6")));
            if (activeTask_ == TaskKind::directInference) {
              capabilities_.directInferenceActive = true;
            }
            updateActionAvailability();
          });
  connect(&runner_, &RemoteCommandRunner::standardOutputReceived, this,
          [this](const QByteArray &data) {
            activeStandardOutput_.append(data);
            if (activeTask_ != TaskKind::availabilityProbe && activeTask_ != TaskKind::listModels) {
              appendClassifiedConsoleBytes(mainStandardOutputDisplayBuffer_,
                                           data);
            }
          });
  connect(&runner_, &RemoteCommandRunner::standardErrorReceived, this,
          [this](const QByteArray &data) {
            activeStandardError_.append(data);
            appendClassifiedConsoleBytes(mainStandardErrorDisplayBuffer_, data);
          });
  connect(&runner_, &RemoteCommandRunner::failedToStart, this,
          [this](const QString &message) {
            const bool availabilityCheck =
                activeTask_ == TaskKind::availabilityProbe;
            appendConsole(QStringLiteral("Error: %1\n").arg(message),
                          QColor(QStringLiteral("#d45b5b")));
            statusLabel_->setText(message);
            if (availabilityCheck) {
              invalidateAvailability();
              thresholds_.invalidate();
              recovery_.discardReadback();
              serviceAvailabilityLabel_->setText(QStringLiteral(
                  "Availability check could not start. Service actions are "
                  "disabled."));
              diagnosticAvailabilityLabel_->setText(
                  QStringLiteral("Availability check could not start: %1")
                      .arg(message));
            }
            if (activeTask_ == TaskKind::prepareModel || activeTask_ == TaskKind::reexportModel) {
              bundlePrepared_ = false;
              preparationStateLabel_->setText("Preparation: command could not start; bundle unconfirmed");
            }
            if (activeTask_ == TaskKind::activateModel)
              activationStateLabel_->setText("Activation: command could not start; active slot unconfirmed");
            if (activeTask_ == TaskKind::installModel) {
              installationStateLabel_->setText("Installation could not start. Review the connection and retry.");
              bundlePrepared_ = false;
            }
            activeTask_ = TaskKind::none;
            updateActionAvailability();
          });
  connect(&runner_, &RemoteCommandRunner::finished, this,
          &MainWindow::finishMainCommand);

  connect(&controlRunner_, &RemoteCommandRunner::started, this,
          [this](const QString &destination, const QString &) {
            controlTimer_.restart();
            controlStandardOutputDisplayBuffer_.clear();
            controlStandardErrorDisplayBuffer_.clear();
            appendConsole(
                QStringLiteral("\n[%1] %2 $ %3\n")
                    .arg(QDateTime::currentDateTime().toString(Qt::ISODate),
                         destination,
                         QStringLiteral("[managed direct-inference stop]")),
                QColor(QStringLiteral("#4f8dd6")));
            updateActionAvailability();
          });
  connect(&controlRunner_, &RemoteCommandRunner::standardOutputReceived, this,
          [this](const QByteArray &data) {
            appendClassifiedConsoleBytes(controlStandardOutputDisplayBuffer_,
                                         data);
          });
  connect(&controlRunner_, &RemoteCommandRunner::standardErrorReceived, this,
          [this](const QByteArray &data) {
            appendClassifiedConsoleBytes(controlStandardErrorDisplayBuffer_,
                                         data);
          });
  connect(&controlRunner_, &RemoteCommandRunner::failedToStart, this,
          [this](const QString &message) {
            appendConsole(QStringLiteral("Error: %1\n").arg(message),
                          QColor(QStringLiteral("#d45b5b")));
            statusLabel_->setText(message);
            updateActionAvailability();
          });
  connect(
      &controlRunner_, &RemoteCommandRunner::finished, this,
      [this](int exitCode, QProcess::ExitStatus exitStatus, bool cancelled) {
        flushClassifiedConsoleBytes(controlStandardOutputDisplayBuffer_);
        flushClassifiedConsoleBytes(controlStandardErrorDisplayBuffer_);
        const qint64 elapsed =
            controlTimer_.isValid() ? controlTimer_.elapsed() : 0;
        const QString summary =
            completionSummary(exitCode, exitStatus, cancelled, elapsed);
        appendConsole(QStringLiteral("\n[%1]\n").arg(summary),
                      completionColor(exitCode, exitStatus, cancelled));
        if (exitStatus == QProcess::NormalExit && exitCode == 0) {
          capabilities_.directInferenceActive = false;
        }
        statusLabel_->setText(summary);
        updateActionAvailability();
      });

  recovery_.configure(connectionOptions(), sudoPasswordEdit_->text());
  connect(&thresholds_, &ThresholdController::policyChangeRequested, &recovery_, &RecoveryController::notePrimaryChange);
  connect(&recovery_, &RecoveryController::changed, this, &MainWindow::updateActionAvailability);
  // This continuation belongs only to an explicit Check/Enter request. It
  // serializes readbacks over the existing SSH transport without polling.
  connect(&recovery_, &RecoveryController::changed, this, [this] {
    if (primaryReadbackPending_ && !recovery_.blocking()) {
      primaryReadbackPending_ = false;
      thresholds_.refresh();
    }
  });
  connect(recoveryPanel_, &RecoveryPanel::deploymentPolicyChanged, this, [this] { invalidatePreparedBundle(); updateActionAvailability(); });
  thresholds_.configure(connectionOptions(), sudoPasswordEdit_->text());
  connect(&thresholds_, &ThresholdController::changed, this, &MainWindow::updateActionAvailability);
  const auto configureThresholds = [this] {
    recovery_.configure(connectionOptions(), sudoPasswordEdit_->text());
    thresholds_.configure(connectionOptions(), sudoPasswordEdit_->text());
  };
  connect(hostEdit_, &QLineEdit::textChanged, this, configureThresholds);
  connect(userEdit_, &QLineEdit::textChanged, this, configureThresholds);
  connect(portSpin_, qOverload<int>(&QSpinBox::valueChanged), this, configureThresholds);
  connect(identityFileEdit_, &QLineEdit::textChanged, this, configureThresholds);
  connect(remoteDirectoryEdit_, &QLineEdit::textChanged, this, configureThresholds);
  connect(sudoPasswordEdit_, &QLineEdit::textChanged, this, configureThresholds);
  for (auto *scope : {static_cast<QWidget *>(operationsTabs_), static_cast<QWidget *>(connectionDialog)}) {
    for (const auto key : {Qt::Key_Return, Qt::Key_Enter}) {
      auto *refreshShortcut = new QShortcut(QKeySequence(key), scope);
      refreshShortcut->setContext(Qt::WidgetWithChildrenShortcut);
      connect(refreshShortcut, &QShortcut::activated, this, &MainWindow::checkAvailability);
    }
  }
  updateDiagnosticModeControls();
}

void MainWindow::closeEvent(QCloseEvent *event) {
  if (thresholds_.blocking() || recovery_.blocking()) {
    statusLabel_->setText(QStringLiteral("Wait for the current operation to finish, or cancel it before closing."));
    event->ignore();
    return;
  }
  if (!runner_.isRunning() && !controlRunner_.isRunning()) {
    sudoPasswordEdit_->clear();
    event->accept();
    return;
  }

  if (controlRunner_.isRunning()) {
    QMessageBox::information(
        this, QStringLiteral("Stop request in progress"),
        QStringLiteral("Wait for the direct-inference stop request to finish "
                       "before closing."));
    event->ignore();
    return;
  }

  if (activeTask_ == TaskKind::directInference) {
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Inference is running"),
        QStringLiteral(
            "Stop direct inference before closing the application?"));
    if (answer == QMessageBox::Yes) {
      stopDirectInference();
    }
    event->ignore();
    return;
  }

  const QString text = QStringLiteral(
      "Cancel the active SSH command and close the application?");
  const auto answer =
      QMessageBox::question(this, QStringLiteral("Command is running"), text);
  if (answer == QMessageBox::Yes) {
    runner_.cancel();
    controlRunner_.cancel();
    sudoPasswordEdit_->clear();
    event->accept();
  } else {
    event->ignore();
  }
}

SshConnectionOptions MainWindow::connectionOptions() const {
  return {
      .host = hostEdit_->text(),
      .user = userEdit_->text(),
      .port = static_cast<quint16>(portSpin_->value()),
      .identityFile = identityFileEdit_->text(),
      .remoteDirectory = remoteDirectoryEdit_->text(),
      .acceptNewHostKey = acceptNewHostKeyCheck_->isChecked(),
  };
}

DiagnosticRuntimeOptions MainWindow::diagnosticOptions() const {
  return {
      .modelName = modelNameEdit_->text().trimmed(),
      .modelDirectory = modelDirectoryEdit_->text().trimmed(),
      .variant = variantCombo_->currentText(),
      .preparationMode = preparationModeCombo_->currentData().toString(),
      .outputMode = outputModeCombo_->currentText(),
      .configPath = configPathEdit_->text().trimmed(),
      .sessionLogPath = sessionLogPathEdit_->text().trimmed(),
      .rawCapturePath = rawCapturePathEdit_->text().trimmed(),
      .emitProbabilities = emitProbabilitiesCheck_->isChecked(),
      .forceRecoveryOff = recoveryPanel_->forceRecoveryOff(),
      .bundleDirectory = preparedBundleDirectory_.isEmpty() ? "build_native/prepared_models/gui-pending" : preparedBundleDirectory_,
      .validationInputs = validationInputsEdit_->toPlainText().split(QRegularExpression("[\\r\\n]+"), Qt::SkipEmptyParts),
      .pythonExecutable = pythonExecutableEdit_->text().trimmed(),
      .ecg3SourceConfirmed = ecg3SourceCheck_->isChecked(),
      .autoValidationInputs = true,
  };
}

QString MainWindow::remoteSudoCommand(const QString &command) const {
  if (sudoPasswordEdit_->text().isEmpty()) {
    return QStringLiteral("sudo -n -- %1").arg(command);
  }
  return QStringLiteral("sudo -S -p '' -- %1").arg(command);
}

void MainWindow::runCommand(const QString &label, const QString &command,
                            TaskKind kind) {
  if (thresholds_.blocking() || recovery_.blocking()) {
    statusLabel_->setText(QStringLiteral("Wait for the threshold operation to finish."));
    return;
  }
  if (runner_.isRunning()) {
    statusLabel_->setText(
        QStringLiteral("Stop or cancel the active command first."));
    return;
  }

  activeTask_ = kind;
  statusLabel_->setText(QStringLiteral("Starting %1…").arg(label));
  QByteArray standardInput;
  if ((kind == TaskKind::serviceMutation ||
       kind == TaskKind::serviceUnitRepair) &&
      !sudoPasswordEdit_->text().isEmpty()) {
    standardInput = sudoPasswordEdit_->text().toUtf8();
    standardInput.append('\n');
  }
  const SshConnectionOptions options = connectionOptions();
  const bool mutation = kind == TaskKind::installModel || kind == TaskKind::activateModel || kind == TaskKind::prepareModel || kind == TaskKind::reexportModel ||
      kind == TaskKind::serviceMutation || kind == TaskKind::serviceUnitRepair || kind == TaskKind::rebuildRuntime;
  if (mutation) invalidateAvailability(); // A failed/cancelled request can still have changed remote state.
  if (mutation) { thresholds_.invalidate(); recovery_.invalidate(); }
  runner_.start(options, (kind == TaskKind::prepareModel || kind == TaskKind::reexportModel || kind == TaskKind::activateModel || kind == TaskKind::installModel)
      ? thresholds::deploymentLock(command) : command, standardInput);
  if (options.acceptNewHostKey && runner_.isRunning()) {
    acceptNewHostKeyCheck_->setChecked(false);
  }
  standardInput.fill('\0');
  updateActionAvailability();
}

void MainWindow::stopDirectInference() {
  if (controlRunner_.isRunning()) {
    statusLabel_->setText(QStringLiteral("A stop request is already running."));
    return;
  }
  controlRunner_.start(connectionOptions(), stopDirectRuntimeCommand());
  updateActionAvailability();
}

void MainWindow::appendConsole(const QString &text, const QColor &color) {
  QTextCursor cursor = console_->textCursor();
  cursor.movePosition(QTextCursor::End);
  QTextCharFormat format;
  format.setForeground(color);
  cursor.insertText(text, format);
  console_->setTextCursor(cursor);
  console_->ensureCursorVisible();
}

void MainWindow::appendConsoleBytes(const QByteArray &data,
                                    const QColor &color) {
  appendConsole(QString::fromUtf8(data), color);
}

void MainWindow::appendClassifiedConsoleBytes(QByteArray &pending,
                                              const QByteArray &data) {
  pending.append(data);
  while (true) {
    const qsizetype newline = pending.indexOf('\n');
    if (newline < 0) {
      break;
    }

    const QByteArray line = pending.left(newline + 1);
    pending.remove(0, newline + 1);
    applyRuntimeInputEvent(line);
    if (activeTask_ == TaskKind::installModel && line.startsWith("__JCG_")) continue;
    const ConsoleLineSeverity severity = classifyConsoleLine(line);
    const QByteArray displayLine = activeTask_ == TaskKind::serviceLogs
                                       ? serviceLogDisplayLine(line)
                                       : line;
    if (severity == ConsoleLineSeverity::elevated) {
      appendConsoleBytes(displayLine, elevatedColor());
    } else if (severity == ConsoleLineSeverity::attention) {
      appendConsoleBytes(displayLine, attentionColor());
    } else if (severity == ConsoleLineSeverity::informational) {
      appendConsoleBytes(displayLine, informationalColor());
    } else {
      appendConsoleBytes(displayLine, palette().color(QPalette::Text));
    }
  }
}

void MainWindow::applyRuntimeInputEvent(const QByteArray &line) {
  const QByteArray installPrefix = "__JCG_INSTALL_STAGE__=";
  if (activeTask_ == TaskKind::installModel && line.startsWith(installPrefix)) {
    installStage_ = QString::fromUtf8(line.mid(installPrefix.size()).trimmed());
    if (installStage_ == "preparation") {
      preparationStateLabel_->setText("Preparation: converting saved weights and checking the engine…");
    } else if (installStage_ == "activation") {
      preparationStateLabel_->setText("Preparation: conversion checks passed. Accuracy and sustained timing remain unverified.");
      activationStateLabel_->setText("Activation: verifying the report and installing checked files…");
    } else if (installStage_ == "verification") {
      activationStateLabel_->setText("Activation: checked files installed; service not started");
      installationStateLabel_->setText("Model installed. Verifying service readiness…");
    }
  }
  if (activeTask_ == TaskKind::directInference || activeTask_ == TaskKind::serviceLogs) {
    const auto start = line.indexOf('{');
    const auto object = start >= 0 ? QJsonDocument::fromJson(line.mid(start)).object() : QJsonObject{};
    if (object.value("code") == "runtime_status") {
      const auto missed = object.value("missed_frame_deadlines");
      const auto maxDelay = object.value("max_frame_end_to_completion_ns");
      runtimeTimingLabel_->setText(QString("Last observed timing (host decode to completion): missed frame deadlines=%1; maximum frame delay=%2. Sustained performance unverified.")
          .arg(missed.isDouble() ? QString::number(missed.toInteger()) : "unavailable",
               maxDelay.isDouble() ? QString::number(maxDelay.toDouble()/1e6, 'g', 8) + " ms" : "unavailable"));
    }
    if (object.value("code") == "worker_scheduling") {
      const auto thread = object.value("thread").toString();
      const auto policy = object.value("policy").toString("unknown");
      workerScheduling_[thread] = QString("%1: actual %2/%3; requested priority=%4; realtime applied=%5; degraded=%6; error=%7; %8")
          .arg(thread, policy, object.contains("priority") ? QString::number(object.value("priority").toInt()) : "unknown",
               object.contains("requested_realtime_priority") ? QString::number(object.value("requested_realtime_priority").toInt()) : "unknown",
               object.contains("realtime_applied") ? (object.value("realtime_applied").toBool() ? "yes" : "no") : "unknown",
               object.contains("degraded") ? (object.value("degraded").toBool() ? "yes" : "no") : "unknown",
               object.contains("error") ? QString::number(object.value("error").toInt()) : "unknown",
               object.value("prerequisite_error").toString());
      workerSchedulingLabel_->setText("Last observed worker scheduling (log history; current invocation unconfirmed):\n" +
          workerScheduling_.values().join('\n'));
    }
  }
  const RuntimeInputEvent event = runtimeInputEventForConsoleLine(line);
  if (event == RuntimeInputEvent::none) {
    return;
  }

  const bool direct = activeTask_ == TaskKind::directInference;
  const bool service = activeTask_ == TaskKind::serviceLogs;
  if (event == RuntimeInputEvent::paused) {
    statusLabel_->setText(
        direct ? QStringLiteral(
                     "Direct inference paused — waiting for usable signal.")
               : QStringLiteral(
                     "Runtime paused — waiting for usable signal; process is "
                     "healthy."));
    if (service) {
      serviceAvailabilityLabel_->setText(QStringLiteral(
          "The service is active but input processing is paused while it "
          "waits for usable signal. No restart is required."));
    }
    return;
  }

  if (event == RuntimeInputEvent::recovered) {
    statusLabel_->setText(
        direct ? QStringLiteral(
                     "Direct inference running — usable signal detected; "
                     "building a fresh model window.")
               : QStringLiteral(
                     "Runtime input recovered — processing resumed with a "
                     "fresh model window."));
    if (service) {
      serviceAvailabilityLabel_->setText(QStringLiteral(
          "The service is active and usable input has recovered. Processing "
          "resumed with a fresh model window."));
    }
    return;
  }

  if (direct) {
    statusLabel_->setText(
        QStringLiteral("Direct inference running — input is usable."));
  }
}

void MainWindow::flushClassifiedConsoleBytes(QByteArray &pending) {
  if (pending.isEmpty()) {
    return;
  }
  QByteArray finalLine = pending;
  pending.clear();
  finalLine.append('\n');
  appendClassifiedConsoleBytes(pending, finalLine);
}

void MainWindow::addCommandToHistory(const QString &command) {
  const int existing = commandCombo_->findText(command);
  if (existing >= 0) {
    commandCombo_->removeItem(existing);
  }
  commandCombo_->insertItem(0, command);
  commandCombo_->setCurrentIndex(0);

  constexpr int kMaximumHistory = 20;
  while (commandCombo_->count() > kMaximumHistory) {
    commandCombo_->removeItem(commandCombo_->count() - 1);
  }
}

void MainWindow::checkAvailability() {
  if (runner_.isRunning() || controlRunner_.isRunning() || thresholds_.blocking() || recovery_.blocking()) return;
  const DiagnosticRuntimeOptions options = diagnosticOptions();
  const QString error = validateDiagnosticOptions(options);
  if (!error.isEmpty()) {
    appendConsole(QStringLiteral("Error: %1\n").arg(error), elevatedColor());
    statusLabel_->setText(error);
    return;
  }
  serviceAvailabilityLabel_->setText(
      QStringLiteral("Checking service availability…"));
  diagnosticAvailabilityLabel_->setText(
      QStringLiteral("Checking direct-runtime availability…"));
  // Use quick check instead of full probe for faster response
  runCommand(QStringLiteral("Availability check"),
             quickAvailabilityCheck(options), TaskKind::availabilityProbe);
}

void MainWindow::invalidateAvailability() {
  capabilities_ = {};
  primaryReadbackPending_ = false;
  observedManifestRevision_.clear(); observedConfigRevision_.clear(); observedRuntimeRevision_.clear();
  serviceSafetyCheck_->setChecked(false);
  gpioSafetyCheck_->setChecked(false);
  serviceAvailabilityLabel_->setText(QStringLiteral(
      "Settings changed. Select Check before using "
      "service actions."));
  diagnosticAvailabilityLabel_->setText(QStringLiteral(
      "Settings changed. Select Check before using direct actions."));
  updateActionAvailability();
}

void MainWindow::applyAvailabilityProbe(const QByteArray &output) {
  // Detect whether this is a quick check (no __JCG_SERVICE__ marker) or full probe
  const bool isQuickCheck = !output.contains("__JCG_SERVICE__");

  const auto present = [&output](const char *marker) {
    return output.contains(QByteArray(marker) + "=yes");
  };

  const auto value = [&output](const char *marker) {
    const QByteArray prefix = QByteArray(marker) + '=';
    const qsizetype start = output.indexOf(prefix);
    if (start < 0) {
      return QString{};
    }
    const qsizetype valueStart = start + prefix.size();
    qsizetype end = output.indexOf('\n', valueStart);
    if (end < 0) {
      end = output.size();
    }
    return QString::fromUtf8(output.mid(valueStart, end - valueStart))
        .trimmed();
  };

  capabilities_.known = true;

  if (isQuickCheck) {
    // Handle quick check output - simplified capability flags
    capabilities_.service = output.contains("__SERVICE_STATE__");
    capabilities_.manifest = present("__MODEL_READY__");
    capabilities_.serviceActive = false;
    capabilities_.serviceEnabled = false;
    capabilities_.serviceUnit = false;
    capabilities_.serviceLinkedToCheckout = false;
    capabilities_.serviceContract = false;
    capabilities_.serviceReady = false;
    capabilities_.runtime = false;
    capabilities_.serviceConfig = false;
    capabilities_.config = false;
    capabilities_.engine = false;
    capabilities_.activeSlotMetadata = false;
    capabilities_.activeSelectionMatches = false;
    capabilities_.directRuntimeReady = false;
    capabilities_.prepareScript = false;
    capabilities_.prepareInputs = false;
    capabilities_.reexportInputs = false;
    capabilities_.rebuildScript = false;
    capabilities_.directInferenceActive = false;
    capabilities_.serviceState = value("__SERVICE_STATE__");
    capabilities_.activeModel = value("__ACTIVE_MODEL__");
    capabilities_.activeVariant = value("__ACTIVE_VARIANT__");
    capabilities_.activeSourceDirectory.clear();
    capabilities_.serviceInputState.clear();
    observedManifestRevision_.clear();
    observedConfigRevision_.clear();
    observedRuntimeRevision_.clear();

    // Apply finished models only on full probe
  } else {
    // Handle full probe output (original behavior)
    capabilities_.service = present("__JCG_SERVICE__");
    capabilities_.serviceActive = present("__JCG_SERVICE_ACTIVE__");
    capabilities_.serviceEnabled = present("__JCG_SERVICE_ENABLED__");
    capabilities_.serviceUnit = present("__JCG_SERVICE_UNIT__");
    capabilities_.serviceLinkedToCheckout = present("__JCG_SERVICE_LINKED__");
    capabilities_.serviceContract = present("__JCG_SERVICE_CONTRACT__");
    capabilities_.serviceReady = present("__JCG_SERVICE_READY__");
    capabilities_.runtime = present("__JCG_RUNTIME__");
    capabilities_.serviceConfig = present("__JCG_SERVICE_CONFIG__");
    capabilities_.config = present("__JCG_CONFIG__");
    capabilities_.manifest = present("__JCG_MANIFEST__");
    capabilities_.engine = present("__JCG_ENGINE__");
    capabilities_.activeSlotMetadata = present("__JCG_SLOT_METADATA__");
    capabilities_.activeSelectionMatches = present("__JCG_ACTIVE_SELECTION__");
    capabilities_.directRuntimeReady = present("__JCG_DIRECT_READY__");
    capabilities_.prepareScript = present("__JCG_PREPARE__");
    capabilities_.prepareInputs = present("__JCG_PREPARE_INPUTS__");
    capabilities_.reexportInputs = present("__JCG_REEXPORT_INPUTS__");
    capabilities_.rebuildScript = present("__JCG_REBUILD__");
    capabilities_.directInferenceActive = present("__JCG_DIRECT_ACTIVE__");
    capabilities_.activeModel = value("__JCG_ACTIVE_MODEL__");
    capabilities_.activeVariant = value("__JCG_ACTIVE_VARIANT__");
    capabilities_.activeSourceDirectory = value("__JCG_ACTIVE_SOURCE__");
    capabilities_.serviceInputState = value("__JCG_SERVICE_INPUT__");
    capabilities_.serviceState = value("__JCG_SERVICE_STATE__");
    observedManifestRevision_ = value("__JCG_DEPLOY_MANIFEST_REVISION__");
    observedConfigRevision_ = value("__JCG_DEPLOY_CONFIG_REVISION__");
    observedRuntimeRevision_ = value("__JCG_DEPLOY_RUNTIME_REVISION__");
    applyFinishedModels(output);
  }

  if (capabilities_.serviceReady) {
    serviceAvailabilityLabel_->setText(
        QStringLiteral("The checkout-linked unit is installed, %1 and %2. "
                       "Its bind mount, service account, state directory, "
                       "configuration, and active model slot passed the "
                       "non-I/O preflight.")
            .arg(capabilities_.serviceState, capabilities_.serviceEnabled
                                                 ? QStringLiteral("enabled")
                                                 : QStringLiteral("disabled")));
    if (capabilities_.serviceActive &&
        capabilities_.serviceInputState == QStringLiteral("paused")) {
      serviceAvailabilityLabel_->setText(
          serviceAvailabilityLabel_->text() +
          QStringLiteral(" Input is paused while the healthy process waits "
                         "for usable signal."));
    } else if (capabilities_.serviceActive &&
               capabilities_.serviceInputState == QStringLiteral("running")) {
      serviceAvailabilityLabel_->setText(
          serviceAvailabilityLabel_->text() +
          QStringLiteral(" Input is usable and processing is running."));
    }
  } else if (capabilities_.service) {
    QStringList reasons;
    if (!capabilities_.serviceLinkedToCheckout) {
      reasons.append(QStringLiteral(
          "the installed unit is not linked to this checkout's deploy unit"));
    }
    if (!capabilities_.serviceContract) {
      reasons.append(QStringLiteral(
          "the bind path/account/groups/state directory/config sandbox is "
          "invalid, or systemd needs daemon-reload"));
    }
    if (!capabilities_.runtime || !capabilities_.serviceConfig ||
        !capabilities_.manifest || !capabilities_.engine ||
        !capabilities_.activeSlotMetadata) {
      reasons.append(QStringLiteral(
          "the runtime, service config, or deploy/model slot is incomplete"));
    }
    if (reasons.isEmpty()) {
      reasons.append(QStringLiteral("runtime or engine validation failed"));
    }
    serviceAvailabilityLabel_->setText(
        QStringLiteral("The unit is installed but %1. Start and Restart "
                       "remain disabled.")
            .arg(reasons.join(QStringLiteral("; "))));
    if (!capabilities_.serviceLinkedToCheckout && capabilities_.serviceUnit && !isQuickCheck) {
      serviceAvailabilityLabel_->setText(
          serviceAvailabilityLabel_->text() +
          QStringLiteral(
              " Stop the service, then select Install / repair unit link."));
    }
  } else {
    serviceAvailabilityLabel_->setText(
        capabilities_.serviceUnit
            ? QStringLiteral(
                  "The systemd unit is not installed. Select Install / repair "
                  "unit link to link this checkout; service actions remain "
                  "disabled.")
            : QStringLiteral(
                  "The systemd unit is not installed, and this checkout's "
                  "deploy unit is missing or unreadable. Service actions "
                  "remain disabled."));
  }

  if (!isQuickCheck) {
    const auto scheduling = value("__JCG_SCHEDULING_PROFILE__");
    serviceAvailabilityLabel_->setText(serviceAvailabilityLabel_->text() +
        QString(" Observed service state: %1 (process state, not accuracy or timing validation).")
            .arg(capabilities_.serviceState.isEmpty() ? "unknown" : capabilities_.serviceState));
    serviceAvailabilityLabel_->setText(serviceAvailabilityLabel_->text() +
        (scheduling == "bounded_rr20" ? " Requested service profile: bounded worker RR/20, RT CPU limit 200 ms; actual scheduling and timing require runtime evidence."
         : scheduling == "legacy" ? " Requested service profile: legacy; worker scheduling unconfirmed."
         : " Service scheduling profile: unknown or unsupported."));
  } else {
    serviceAvailabilityLabel_->setText(serviceAvailabilityLabel_->text() +
        QString(" Observed service state: %1.").arg(capabilities_.serviceState.isEmpty() ? "unknown" : capabilities_.serviceState));
  }

  if (!isQuickCheck) {
    QStringList available;
    QStringList missing;
    const auto addCapability = [&available, &missing](bool value,
                                                      const QString &name) {
      (value ? available : missing).append(name);
    };
    addCapability(capabilities_.runtime, QStringLiteral("runtime"));
    addCapability(capabilities_.config, QStringLiteral("config"));
    addCapability(capabilities_.manifest, QStringLiteral("active-slot manifest"));
    addCapability(capabilities_.engine, QStringLiteral("active-slot engine"));
    addCapability(capabilities_.activeSlotMetadata,
                  QStringLiteral("active-slot identity"));
    addCapability(capabilities_.activeSelectionMatches,
                  QStringLiteral("selected model is active"));
    addCapability(capabilities_.directRuntimeReady,
                  QStringLiteral("selected runtime validation"));
    addCapability(capabilities_.prepareScript,
                  QStringLiteral("model preparation"));
    addCapability(capabilities_.prepareInputs,
                  QStringLiteral("selected preparation inputs"));
    addCapability(capabilities_.rebuildScript, QStringLiteral("build scripts"));

    QString message =
        QStringLiteral("Available: %1.")
            .arg(available.isEmpty() ? QStringLiteral("none")
                                     : available.join(QStringLiteral(", ")));
    if (!missing.isEmpty()) {
      message +=
          QStringLiteral(" Missing: %1.").arg(missing.join(QStringLiteral(", ")));
    }
    if (!capabilities_.activeModel.isEmpty()) {
      message += QStringLiteral(" Active slot: %1 (%2), source %3.")
                     .arg(capabilities_.activeModel, capabilities_.activeVariant,
                          capabilities_.activeSourceDirectory);
    }
    if (capabilities_.serviceActive) {
      message += QStringLiteral(
          " Stop the service before preparing or replacing the active model.");
    }
    if (capabilities_.directInferenceActive) {
      message +=
          QStringLiteral(" A GUI-managed direct inference process is active.");
    }
    diagnosticAvailabilityLabel_->setText(message);
  } else {
    // Simplified message for quick check
    QString message = QStringLiteral("Quick check completed.");
    if (!capabilities_.activeModel.isEmpty()) {
      message += QStringLiteral(" Active model: %1 (%2).")
                     .arg(capabilities_.activeModel, capabilities_.activeVariant);
    }
    diagnosticAvailabilityLabel_->setText(message);
  }

  appendConsole(QStringLiteral("SSH connection successful.\n%1\n")
                    .arg(serviceAvailabilityLabel_->text()),
                QColor(QStringLiteral("#4d9b58")));
}

void MainWindow::applyFinishedModels(const QByteArray &output) {
  finishedModels_ = parseFinishedModels(output);
  const QSignalBlocker blocker(finishedModelCombo_);
  finishedModelCombo_->clear();
  finishedModelCombo_->addItem(finishedModels_.isEmpty()
      ? QStringLiteral("No finished models loaded — refresh")
      : QStringLiteral("Select a finished model…"));
  for (const auto &model : finishedModels_) {
    finishedModelCombo_->addItem(model.name + (model.variant == "tuned" ? " — Fine-tuned" : "") +
        " — " + model.kind +
        " — " + model.predictionDescription() +
        (model.recoverySupported ? " — Recovery supported" : " — Single output"));
    finishedModelCombo_->setItemData(finishedModelCombo_->count()-1,
        QString("Source: %1\nPublished artifact: %2").arg(model.directory, model.variant), Qt::ToolTipRole);
  }
  syncFinishedModelSelection();
}

void MainWindow::syncFinishedModelSelection() {
  const auto options = diagnosticOptions();
  int index = 0;
  for (qsizetype i = 0; i < finishedModels_.size(); ++i) {
    const auto &model = finishedModels_.at(i);
    if (model.name == options.modelName && model.directory == options.modelDirectory &&
        model.variant == options.variant) {
      index = static_cast<int>(i + 1);
      break;
    }
  }
  finishedModelCombo_->setCurrentIndex(index);
  modelTimingLabel_->setText(index > 0 ? finishedModels_.at(index-1).predictionDescription()
      : QStringLiteral("Refresh source models to read their trained timing contract; native validation is required."));
  recoveryPanel_->setDeploymentRecoverySupport(index > 0
      ? std::optional<bool>(finishedModels_.at(index - 1).recoverySupported) : std::nullopt);
}

void MainWindow::updateActionAvailability() {
  const bool mainBusy = runner_.isRunning();
  const bool controlBusy = controlRunner_.isRunning();
  const bool anyBusy = mainBusy || controlBusy || thresholds_.blocking() || recovery_.blocking();
  thresholds_.setExternalBusy((mainBusy && activeTask_ != TaskKind::serviceLogs) || controlBusy || recovery_.blocking());
  recovery_.setExternalBusy((mainBusy && activeTask_ != TaskKind::serviceLogs) || controlBusy || thresholds_.blocking());
  const QString optionError = validateDiagnosticOptions(diagnosticOptions());
  const bool parametersValid = optionError.isEmpty();
  const bool gpioMode =
      outputModeCombo_->currentText().contains(QStringLiteral("gpio"));
  const bool gpioApproved = !gpioMode || gpioSafetyCheck_->isChecked();

  hostEdit_->setEnabled(!anyBusy);
  userEdit_->setEnabled(!anyBusy);
  portSpin_->setEnabled(!anyBusy);
  identityFileEdit_->setEnabled(!anyBusy);
  remoteDirectoryEdit_->setEnabled(!anyBusy);
  sudoPasswordEdit_->setEnabled(!anyBusy);
  acceptNewHostKeyCheck_->setEnabled(!anyBusy);
  probeButton_->setEnabled(!anyBusy);
  connectionCheckButton_->setEnabled(!anyBusy);
  healthButton_->setEnabled(!anyBusy);

  const bool publishedSelection = finishedModelCombo_->currentIndex() > 0;
  const bool preparationValid = validatePreparationOptions(diagnosticOptions()).isEmpty();
  activateModelButton_->setEnabled(!anyBusy && preparationValid && bundlePrepared_ &&
      capabilities_.known && capabilities_.prepareScript &&
      !capabilities_.serviceActive && !capabilities_.directInferenceActive);
  installModelButton_->setEnabled(!anyBusy && preparationValid &&
      (publishedSelection || (capabilities_.known && capabilities_.prepareScript && capabilities_.reexportInputs && capabilities_.runtime)) &&
      !capabilities_.serviceActive && !capabilities_.directInferenceActive);
  pythonExecutableEdit_->setEnabled(!anyBusy);
  validationInputsEdit_->setEnabled(!anyBusy);
  ecg3SourceCheck_->setEnabled(!anyBusy);

  serviceStatusButton_->setEnabled(!anyBusy && capabilities_.service);
  serviceInstallButton_->setEnabled(
      !anyBusy && capabilities_.known && capabilities_.serviceUnit &&
      !capabilities_.serviceActive && !capabilities_.directInferenceActive);
  serviceReloadButton_->setEnabled(!anyBusy && capabilities_.service);
  serviceSafetyCheck_->setEnabled(!anyBusy && (publishedSelection || capabilities_.serviceReady || (capabilities_.serviceContract && capabilities_.runtime && capabilities_.serviceConfig)));
  serviceStartButton_->setEnabled(
      !anyBusy && capabilities_.serviceReady && !capabilities_.serviceActive &&
      !capabilities_.directInferenceActive && serviceSafetyCheck_->isChecked());
  serviceStopButton_->setEnabled(!anyBusy && capabilities_.service &&
                                 capabilities_.serviceActive);
  serviceRestartButton_->setEnabled(
      !anyBusy && capabilities_.serviceReady && capabilities_.serviceActive &&
      !capabilities_.directInferenceActive && serviceSafetyCheck_->isChecked());
  serviceLogsButton_->setEnabled(!anyBusy && capabilities_.service);

  modelNameEdit_->setEnabled(!anyBusy);
  finishedModelCombo_->setEnabled(!anyBusy);
  refreshModelsButton_->setEnabled(!anyBusy);
  modelDirectoryEdit_->setEnabled(!anyBusy);
  variantCombo_->setEnabled(!anyBusy);
  preparationModeCombo_->setEnabled(!anyBusy);
  outputModeCombo_->setEnabled(!anyBusy);
  configPathEdit_->setEnabled(!anyBusy);
  sessionLogPathEdit_->setEnabled(!anyBusy);
  rawCapturePathEdit_->setEnabled(!anyBusy);
  emitProbabilitiesCheck_->setEnabled(
      !anyBusy &&
      outputModeCombo_->currentText().contains(QStringLiteral("jsonl")));
  gpioSafetyCheck_->setEnabled(!anyBusy && gpioMode);
  diagnosticCheckButton_->setEnabled(!anyBusy);

  prepareModelButton_->setEnabled(
      !anyBusy && preparationValid && capabilities_.prepareScript &&
      capabilities_.prepareInputs && !capabilities_.serviceActive &&
      !capabilities_.directInferenceActive);
  reexportModelButton_->setEnabled(
      !anyBusy && preparationValid && capabilities_.prepareScript &&
      capabilities_.reexportInputs && capabilities_.runtime &&
      !capabilities_.serviceActive && !capabilities_.directInferenceActive);
  validateRuntimeButton_->setEnabled(
      !anyBusy && parametersValid && capabilities_.runtime &&
      capabilities_.config && capabilities_.manifest && capabilities_.engine &&
      capabilities_.activeSlotMetadata && !capabilities_.serviceActive &&
      !capabilities_.directInferenceActive);
  startInferenceButton_->setEnabled(
      !anyBusy && parametersValid && gpioApproved && capabilities_.runtime &&
      capabilities_.config && capabilities_.manifest && capabilities_.engine &&
      capabilities_.activeSlotMetadata &&
      capabilities_.activeSelectionMatches &&
      capabilities_.directRuntimeReady && !capabilities_.serviceActive &&
      !capabilities_.directInferenceActive);
  rebuildRuntimeButton_->setEnabled(!anyBusy && capabilities_.rebuildScript &&
                                    !capabilities_.serviceActive &&
                                    !capabilities_.directInferenceActive);
  stopInferenceButton_->setEnabled(!controlBusy &&
                                   (capabilities_.directInferenceActive ||
                                    activeTask_ == TaskKind::directInference));

  commandCombo_->setEnabled(!anyBusy);
  runButton_->setEnabled(!anyBusy);
  cancelButton_->setEnabled(mainBusy && !controlBusy);
  cancelButton_->setText(activeTask_ == TaskKind::directInference
                             ? QStringLiteral("Stop inference")
                             : QStringLiteral("Cancel"));
}

void MainWindow::updateDiagnosticModeControls() {
  capabilities_.directRuntimeReady = false;
  if (capabilities_.known) {
    diagnosticAvailabilityLabel_->setText(QStringLiteral(
        "Trigger output changed. Validate again before starting inference."));
  }
  const bool hasJsonl =
      outputModeCombo_->currentText().contains(QStringLiteral("jsonl"));
  const bool hasGpio =
      outputModeCombo_->currentText().contains(QStringLiteral("gpio"));
  if (!hasJsonl) {
    emitProbabilitiesCheck_->setChecked(false);
  }
  if (!hasGpio) {
    gpioSafetyCheck_->setChecked(false);
  }
  updateActionAvailability();
}

void MainWindow::finishMainCommand(int exitCode,
                                   QProcess::ExitStatus exitStatus,
                                   bool cancelled) {
  const TaskKind completedTask = activeTask_;
  if (completedTask != TaskKind::availabilityProbe) {
    flushClassifiedConsoleBytes(mainStandardOutputDisplayBuffer_);
  }
  flushClassifiedConsoleBytes(mainStandardErrorDisplayBuffer_);
  const qint64 elapsed = commandTimer_.isValid() ? commandTimer_.elapsed() : 0;

  if (completedTask == TaskKind::availabilityProbe &&
      exitStatus == QProcess::NormalExit && exitCode == 0 && !cancelled) {
    applyAvailabilityProbe(activeStandardOutput_);
  } else if (completedTask == TaskKind::listModels && !cancelled &&
             exitStatus == QProcess::NormalExit && exitCode == 0) {
    applyFinishedModels(activeStandardOutput_);
    appendConsole(QStringLiteral("Loaded %1 saved model variants from finished_models.\n")
                      .arg(finishedModels_.size()), informationalColor());
  } else if (completedTask == TaskKind::availabilityProbe) {
    invalidateAvailability();
    thresholds_.invalidate();
    recovery_.discardReadback();
    serviceAvailabilityLabel_->setText(QStringLiteral(
        "Availability check failed. Service actions are disabled; retry from "
        "the main window."));
    diagnosticAvailabilityLabel_->setText(QStringLiteral(
        "Availability check failed. Direct actions are disabled; retry from "
        "the main window."));
  }
  if (completedTask == TaskKind::directInference) {
    capabilities_.directInferenceActive = false;
  }
  if (completedTask == TaskKind::validateRuntime &&
      exitStatus == QProcess::NormalExit && exitCode == 0) {
    capabilities_.directRuntimeReady = true;
    diagnosticAvailabilityLabel_->setText(
        QStringLiteral("The direct runtime configuration and active model "
                       "slot passed validation."));
  }
  if ((completedTask == TaskKind::prepareModel ||
       completedTask == TaskKind::rebuildRuntime) &&
      exitStatus == QProcess::NormalExit && exitCode == 0) {
    invalidateAvailability();
  }
  if ((completedTask == TaskKind::serviceMutation ||
       completedTask == TaskKind::serviceUnitRepair) &&
      exitStatus == QProcess::NormalExit && exitCode == 0) {
    invalidateAvailability();
  }

  const QString summary =
      completionSummary(exitCode, exitStatus, cancelled, elapsed);
  appendConsole(QStringLiteral("\n[%1]\n").arg(summary),
                completionColor(exitCode, exitStatus, cancelled));
  const bool unknownHostKey =
      activeStandardError_.toLower().contains("host key is known for");
  if (unknownHostKey) {
    const QString guidance = QStringLiteral(
        "Unknown SSH host key — open Connection, verify the host, enable "
        "trust for the next connection, and retry.");
    appendConsole(guidance + QLatin1Char('\n'), attentionColor());
    statusLabel_->setText(guidance);
  } else {
    statusLabel_->setText(summary);
  }
  activeTask_ = TaskKind::none;
  const bool success = exitStatus == QProcess::NormalExit && exitCode == 0 && !cancelled;
  if (completedTask == TaskKind::installModel) {
    bundlePrepared_ = false;
    if (success) applyAvailabilityProbe(activeStandardOutput_);
    const bool ready = success && capabilities_.serviceReady && capabilities_.activeSelectionMatches &&
        !capabilities_.serviceActive && !capabilities_.directInferenceActive;
    if (ready) {
      installationStateLabel_->setText("Installed — ready to start the service. Accuracy and sustained timing remain unverified.");
      statusLabel_->setText("Model installation complete. Start the service when ready.");
    } else {
      capabilities_.serviceReady = false;
      if (installStage_ == "preparation") {
        preparationStateLabel_->setText("Preparation: failed or cancelled; model was not activated");
        activationStateLabel_->setText("Activation: not attempted");
      } else if (installStage_ == "activation") {
        activationStateLabel_->setText("Activation: failed or cancelled; active model unconfirmed");
      }
      if (!success) invalidateAvailability();
      installationStateLabel_->setText(installStage_ == "verification"
          ? "Model activation finished, but service readiness is unconfirmed. Review the console before starting."
          : "Installation did not finish. Review the console and retry after resolving the error. Service was not started.");
      statusLabel_->setText(installationStateLabel_->text());
    }
    installStage_.clear();
  }
  if (completedTask == TaskKind::prepareModel || completedTask == TaskKind::reexportModel) {
    bundlePrepared_ = success;
    preparationStateLabel_->setText(success
        ? "Preparation: checks passed; bundle not activated. Accuracy, sustained processing and physical timing remain unverified."
        : "Preparation: failed or cancelled; bundle unconfirmed. Review retained evidence and use a new output directory.");
  }
  if (completedTask == TaskKind::activateModel) {
    activationStateLabel_->setText(success
        ? "Activation: prepared files activated; service not started. Check before a separate service start."
        : "Activation: failed or cancelled; active slot unconfirmed. Check before continuing.");
    bundlePrepared_ = false;
  }
  updateActionAvailability();
  if (completedTask == TaskKind::availabilityProbe && exitStatus == QProcess::NormalExit && exitCode == 0 && !cancelled) {
    // Check/Enter explicitly requests these snapshots. Apply, Start and model
    // selection do not schedule another status request.
    primaryReadbackPending_ = true;
    recovery_.refresh();
  }
}

void MainWindow::invalidatePreparedBundle() {
  bundlePrepared_ = false;
  preparedBundleDirectory_.clear();
  preparationStateLabel_->setText("Preparation: waiting for installation");
  installationStateLabel_->setText("Selection or settings changed. Install the selected model when ready.");
}

void MainWindow::createPreparationDirectory() {
  preparedBundleDirectory_ = QString("build_native/prepared_models/gui-%1-%2-%3")
      .arg(modelNameEdit_->text().trimmed(), QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss"),
           QUuid::createUuid().toString(QUuid::WithoutBraces));
}
