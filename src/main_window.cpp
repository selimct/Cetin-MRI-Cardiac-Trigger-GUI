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
#include <QAbstractSpinBox>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QEvent>
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
#include <QSplitter>
#include <QTabWidget>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QVBoxLayout>
#include <QWidget>

namespace {
constexpr auto kServiceName = "dkfz-native-uart-live";
constexpr qsizetype kMaximumOutputTailBytes = 64 * 1024;

void appendOutputTail(QByteArray &buffer, const QByteArray &data) {
  if (data.size() >= kMaximumOutputTailBytes) {
    buffer = data.right(kMaximumOutputTailBytes);
  } else {
    const auto excess = buffer.size() + data.size() - kMaximumOutputTailBytes;
    if (excess > 0) buffer.remove(0, excess);
    buffer.append(data);
  }
}

QColor informationalColor() { return QColor(QStringLiteral("#4d9b58")); }

QColor attentionColor() { return QColor(QStringLiteral("#c28b27")); }

QColor elevatedColor() { return QColor(QStringLiteral("#d45b5b")); }

QPushButton *makeActionButton(const QString &text, QWidget *parent) {
  auto *button = new QPushButton(text, parent);
  button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  return button;
}

QLabel *guidance(const QString &text, QWidget *parent) {
  auto *label = new QLabel(text, parent);
  label->setWordWrap(true);
  label->setTextFormat(Qt::PlainText);
  return label;
}

void explainAction(QPushButton *button, const QString &purpose, const QString &requirement = {}) {
  const auto text = purpose + (!button->isEnabled() && !requirement.isEmpty()
      ? "\nTo use this: " + requirement : QString());
  button->setToolTip(text);
  button->setStatusTip(text);
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

  probeButton_ = new QPushButton(QStringLiteral("Check Jetson"), central);
  probeButton_->setToolTip(QStringLiteral(
      "Connect over SSH and refresh models, saved engines and detector readiness. Reads status without changing settings or starting the detector."));
  connectionCheckButton_ =
      makeActionButton(QStringLiteral("Check Jetson"), connectionDialog);
  connectionCheckButton_->setToolTip(QStringLiteral(
      "Use these SSH settings to refresh the Jetson's models and detector state. This does not start or install anything."));
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
  serviceLayout->addWidget(guidance("Choose a model, install a saved engine or build a new one, then start the detector. Stop the detector before switching models.", servicePage));
  nextStepLabel_ = guidance("Open Connection, enter your Jetson settings, then click Check Jetson.", servicePage);
  nextStepLabel_->setObjectName("nextStep");
  serviceLayout->addWidget(nextStepLabel_);
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
      makeActionButton(QStringLiteral("Repair service setup"),
                       servicePage);
  serviceInstallButton_->setToolTip(QStringLiteral(
      "Link this checkout's deploy/dkfz-native-uart-live.service into "
      "/etc/systemd/system and reload systemd. The service must be stopped; "
      "this does not enable or start it."));
  serviceReloadButton_ =
      makeActionButton(QStringLiteral("Reload service definition"), servicePage);
  serviceMaintenanceActions->addWidget(serviceInstallButton_);
  serviceMaintenanceActions->addWidget(serviceReloadButton_);
  auto *serviceActions = new QHBoxLayout;
  serviceStatusButton_ =
      makeActionButton(QStringLiteral("Show service status"), servicePage);
  serviceStartButton_ = makeActionButton(QStringLiteral("Start detector"), servicePage);
  serviceStopButton_ = makeActionButton(QStringLiteral("Stop detector"), servicePage);
  serviceRestartButton_ =
      makeActionButton(QStringLiteral("Restart detector"), servicePage);
  serviceLogsButton_ =
      makeActionButton(QStringLiteral("View live logs"), servicePage);
  serviceActions->addWidget(serviceStartButton_);
  serviceActions->addWidget(serviceStopButton_);
  serviceActions->addWidget(serviceRestartButton_);
  auto *serviceReadActions = new QHBoxLayout;
  serviceReadActions->addWidget(serviceStatusButton_);
  serviceReadActions->addWidget(serviceLogsButton_);

  auto *modelDeploymentGroup = new QGroupBox(QStringLiteral("1. Choose and install a model"), servicePage);
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
      QStringLiteral("Build files only"), modelDeploymentGroup);
  reexportModelButton_->setObjectName(QStringLiteral("reexportModel"));
  reexportModelButton_->setToolTip(QStringLiteral(
      "Copy saved weights into a new bundle, export ONNX, build TensorRT and validate recordings. "
      "Preparation does not activate files or verify accuracy or sustained timing. "
      "Requires the full Jetson Python environment and a built runtime. Stop "
      "the service and direct inference first."));
  finishedModelForm->addRow(QStringLiteral("Model"), modelSelectionRow);
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
  activateModelButton_ = makeActionButton(QStringLiteral("Install files built this session"), modelDeploymentGroup);
  activateModelButton_->setObjectName("activatePreparedBundle");
  activateModelButton_->setToolTip("Activate the prepared pair using its fingerprinted validation report. The service must already be idle. Starting the service is a separate action.");
  installModelButton_ = makeActionButton("Build and install new engine", modelDeploymentGroup);
  installModelButton_->setObjectName("installSelectedModel");
  installModelButton_->setToolTip("Convert saved PTH weights, validate the engine, activate the checked model and verify service readiness. The service must already be stopped and will stay stopped.");
  installationStateLabel_ = new QLabel("Select a saved model to install. The service stays stopped until Start.", modelDeploymentGroup);
  installationStateLabel_->setObjectName("installationState");
  installationStateLabel_->setWordWrap(true);
  installationStateLabel_->setTextFormat(Qt::PlainText);
  finishedModelForm->addRow(guidance("Previously installed this model? Choose a saved engine below to reuse its conversion.", modelDeploymentGroup));
  savedBundleCombo_ = new QComboBox(modelDeploymentGroup);
  savedBundleCombo_->setObjectName("savedBundleSelection");
  savedBundleCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  savedBundleCombo_->setMinimumContentsLength(36);
  savedBundleCombo_->addItem("Refresh to find saved engines");
  savedBundleCombo_->setToolTip("Previous installations are kept under build_native/prepared_models/ on the Jetson. Entries match the selected model, variant and startup recovery policy; source bytes and validation are checked when reused.");
  refreshBundlesButton_ = new QPushButton("Refresh saved engines", modelDeploymentGroup);
  refreshBundlesButton_->setObjectName("refreshSavedBundles");
  auto *savedBundleRow = new QHBoxLayout;
  savedBundleRow->addWidget(savedBundleCombo_, 1);
  savedBundleRow->addWidget(refreshBundlesButton_);
  finishedModelForm->addRow("Saved engines", savedBundleRow);
  reuseBundleButton_ = new QPushButton("Use saved engine", modelDeploymentGroup);
  reuseBundleButton_->setObjectName("reuseSavedBundle");
  reuseBundleButton_->setToolTip("Verify and activate this saved engine without ONNX export or TensorRT generation. The detector must be stopped. Start remains separate. To rebuild instead, use Install selected model.");
  finishedModelForm->addRow(reuseBundleButton_);
  auto *showBuildButton = new QPushButton("Show new-engine build", modelDeploymentGroup);
  showBuildButton->setCheckable(true);
  showBuildButton->setToolTip("Expand the first-installation/rebuild controls. Opening this section does not send a command. Use a saved engine above to skip conversion.");
  auto *newBuildSettings = new QWidget(modelDeploymentGroup);
  auto *newBuildLayout = new QVBoxLayout(newBuildSettings);
  newBuildLayout->setContentsMargins(0, 0, 0, 0);
  newBuildLayout->addWidget(guidance("First installation or rebuilding? Confirm the training input, then build a new engine. This can take several minutes.", newBuildSettings));
  newBuildLayout->addWidget(ecg3SourceCheck_);
  newBuildLayout->addWidget(installModelButton_);
  newBuildSettings->hide();
  connect(showBuildButton, &QPushButton::toggled, newBuildSettings, &QWidget::setVisible);
  connect(showBuildButton, &QPushButton::toggled, showBuildButton, [showBuildButton](bool open) {
    showBuildButton->setText(open ? "Hide new-engine build" : "Show new-engine build");
  });
  finishedModelForm->addRow(showBuildButton);
  finishedModelForm->addRow(newBuildSettings);
  finishedModelForm->addRow(installationStateLabel_);
  auto *advancedButton = new QPushButton("Build options and manual steps", modelDeploymentGroup);
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
  advancedForm->addRow(guidance("Optional split workflow: build files first, then install them. Use the main build button for both steps together.", advancedSettings));
  advancedForm->addRow(reexportModelButton_);
  advancedForm->addRow(activateModelButton_);
  advancedForm->addRow(preparationStateLabel_);
  advancedForm->addRow(activationStateLabel_);
  advancedSettings->hide();
  connect(advancedButton, &QPushButton::toggled, advancedSettings, &QWidget::setVisible);
  finishedModelForm->addRow(advancedButton);
  finishedModelForm->addRow(advancedSettings);
  serviceLayout->addWidget(modelDeploymentGroup);
  auto *runGroup = new QGroupBox("2. Run the installed model", servicePage);
  auto *runLayout = new QVBoxLayout(runGroup);
  installedModelLabel_ = guidance("Installed model: click Check Jetson to read it.", runGroup);
  installedModelLabel_->setObjectName("installedModelSummary");
  runLayout->addWidget(installedModelLabel_);
  runLayout->addWidget(guidance("Start runs the installed model. Choosing a different entry above takes effect only after you install or use its engine.", runGroup));
  runLayout->addWidget(serviceSafetyCheck_);
  runLayout->addLayout(serviceActions);
  runLayout->addLayout(serviceReadActions);
  serviceLayout->addWidget(runGroup);
  auto *runtimeDetailsButton = new QPushButton("Show readiness and timing details", servicePage);
  runtimeDetailsButton->setCheckable(true);
  auto *runtimeDetails = new QWidget(servicePage);
  auto *runtimeDetailsLayout = new QVBoxLayout(runtimeDetails);
  runtimeDetailsLayout->setContentsMargins(0, 0, 0, 0);
  runtimeDetailsLayout->addWidget(serviceAvailabilityLabel_);
  workerSchedulingLabel_ = new QLabel("Worker scheduling: unobserved. View live logs to read actual worker status.", servicePage);
  workerSchedulingLabel_->setObjectName("workerSchedulingStatus");
  workerSchedulingLabel_->setTextFormat(Qt::PlainText);
  workerSchedulingLabel_->setWordWrap(true);
  runtimeDetailsLayout->addWidget(workerSchedulingLabel_);
  runtimeTimingLabel_ = new QLabel("Runtime timing: unobserved. View live logs for deadline and frame delay measurements.", servicePage);
  runtimeTimingLabel_->setObjectName("runtimeTimingStatus");
  runtimeTimingLabel_->setTextFormat(Qt::PlainText);
  runtimeTimingLabel_->setWordWrap(true);
  runtimeDetailsLayout->addWidget(runtimeTimingLabel_);
  runtimeDetails->hide();
  connect(runtimeDetailsButton, &QPushButton::toggled, runtimeDetails, &QWidget::setVisible);
  connect(runtimeDetailsButton, &QPushButton::toggled, runtimeDetailsButton, [runtimeDetailsButton](bool open) {
    runtimeDetailsButton->setText(open ? "Hide readiness and timing details" : "Show readiness and timing details");
  });
  serviceLayout->addWidget(runtimeDetailsButton);
  serviceLayout->addWidget(runtimeDetails);
  serviceLayout->addStretch();
  auto *serviceScroll = new QScrollArea(operationsTabs_);
  serviceScroll->setWidgetResizable(true);
  serviceScroll->setWidget(servicePage);
  operationsTabs_->addTab(serviceScroll, QStringLiteral("Run"));
  operationsTabs_->setTabToolTip(0, "Choose or switch a model, then start or stop the installed detector. Begin here for normal use.");
  auto *detectorPage = new QWidget(operationsTabs_);
  auto *detectorLayout = new QVBoxLayout(detectorPage);
  detectorLayout->addWidget(guidance("Adjust the running detector here. Primary settings persist across restarts; live recovery changes last for the current run. Refresh after applying to confirm what is active.", detectorPage));
  auto *thresholdPanel = new ThresholdPanel(&thresholds_, detectorPage);
  recoveryPanel_ = new RecoveryPanel(&recovery_, detectorPage);
  finishedModelForm->insertRow(1, recoveryPanel_->takeDeploymentControls());
  detectorLayout->addWidget(thresholdPanel);
  detectorLayout->addWidget(recoveryPanel_);
  auto *detectorScroll = new QScrollArea(operationsTabs_);
  detectorScroll->setWidgetResizable(true);
  detectorScroll->setWidget(detectorPage);
  operationsTabs_->addTab(detectorScroll, QStringLiteral("Detector settings"));
  operationsTabs_->setTabToolTip(1, "Change primary thresholds and live recovery. Each section explains when changes persist and how to confirm them.");

  auto *diagnosticPage = new QWidget(operationsTabs_);
  auto *diagnosticLayout = new QVBoxLayout(diagnosticPage);
  diagnosticLayout->addWidget(guidance("Use these tools for troubleshooting or a temporary test run over SSH. For everyday operation, start the detector from Run.", diagnosticPage));
  auto *maintenanceGroup = new QGroupBox("Service setup", diagnosticPage);
  auto *maintenanceLayout = new QVBoxLayout(maintenanceGroup);
  maintenanceLayout->addWidget(guidance("Use these only when Check Jetson reports a service setup problem, or after editing the service definition.", maintenanceGroup));
  maintenanceLayout->addLayout(serviceMaintenanceActions);
  diagnosticLayout->addWidget(maintenanceGroup);
  diagnosticAvailabilityLabel_ = new QLabel(
      QStringLiteral(
          "Availability has not been checked. Direct actions are disabled."),
      diagnosticPage);
  diagnosticAvailabilityLabel_->setWordWrap(true);
  diagnosticCheckButton_ =
      new QPushButton(QStringLiteral("Check test setup"), diagnosticPage);
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
  variantCombo_->setToolTip("best selects the saved base checkpoint; tuned selects the saved fine-tuned checkpoint. Normally filled by the Model selector in Run.");
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
  outputModeCombo_->setItemData(0, "Inference only, with no trigger pulses or event records.", Qt::ToolTipRole);
  outputModeCombo_->setItemData(1, "Print trigger events as JSON records in the console, without GPIO pulses.", Qt::ToolTipRole);
  outputModeCombo_->setItemData(2, "Send physical trigger pulses through the configured GPIO output.", Qt::ToolTipRole);
  outputModeCombo_->setItemData(3, "Send physical GPIO pulses and print trigger events in the console.", Qt::ToolTipRole);
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
  preparationModeCombo_->hide(); // Preparation always uses the same isolated pipeline.

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
  prepareModelButton_->hide(); // The Run tab already exposes the manual build step.
  prepareModelButton_->setToolTip(QStringLiteral(
      "Runs prepare_model.sh --prepare-only with recordings and a new output directory. The systemd service must be "
      "stopped."));
  validateRuntimeButton_ = makeActionButton(
      QStringLiteral("Validate test setup"), diagnosticPage);
  startInferenceButton_ = makeActionButton(
      QStringLiteral("Start test run"), diagnosticPage);
  stopInferenceButton_ =
      makeActionButton(QStringLiteral("Stop test run"), diagnosticPage);
  rebuildRuntimeButton_ = makeActionButton(
      QStringLiteral("Rebuild detector software"), diagnosticPage);
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
  auto *diagnosticScroll = new QScrollArea(operationsTabs_);
  diagnosticScroll->setWidgetResizable(true);
  diagnosticScroll->setWidget(diagnosticPage);
  operationsTabs_->addTab(diagnosticScroll, QStringLiteral("Advanced"));
  operationsTabs_->setTabToolTip(2, "Inspect test configuration, run temporary diagnostics, or repair the Jetson service setup.");

  auto *commandGroup =
      new QGroupBox(QStringLiteral("Remote command (advanced)"), central);
  auto *commandLayout = new QHBoxLayout(commandGroup);
  commandCombo_ = new QComboBox(commandGroup);
  commandCombo_->setEditable(true);
  commandCombo_->setInsertPolicy(QComboBox::NoInsert);
  commandCombo_->lineEdit()->setPlaceholderText(
      QStringLiteral("Enter a command"));
  runButton_ = new QPushButton(QStringLiteral("Run command"), commandGroup);
  cancelButton_ = new QPushButton(QStringLiteral("Cancel"), commandGroup);
  cancelButton_->setEnabled(false);
  auto *clearButton =
      new QPushButton(QStringLiteral("Clear output"), commandGroup);
  commandLayout->addWidget(commandCombo_, 1);
  commandLayout->addWidget(runButton_);
  commandLayout->addWidget(cancelButton_);
  commandLayout->addWidget(clearButton);
  runButton_->setToolTip("Execute the typed shell command on the Jetson over SSH. Commands may change files or settings; interactive password prompts are unsupported.");
  clearButton->setToolTip("Clear the visible console text. Remote logs and the detector keep running.");
  healthButton_->setToolTip("Read the Jetson's operating system, storage and CUDA/TensorRT tool information into the console.");
  refreshModelsButton_->setToolTip("Read available trained models and saved engines from the Jetson. Selection alone does not install a model.");
  refreshBundlesButton_->setToolTip("Find previous successful installations that match this model, variant and startup recovery policy. Reads files without installing anything.");
  serviceSafetyCheck_->setToolTip("Required before Start or Restart because the installed service can drive the configured hardware output.");
  ecg3SourceCheck_->setToolTip("Required for a new build: confirm the training ECG3 input is ECG2 minus ECG1. Saved-engine reuse uses its existing validation evidence.");
  pythonExecutableEdit_->setToolTip("Python on the Jetson with the backend's export and parity dependencies. Used during builds and saved-engine activation.");
  validationInputsEdit_->setToolTip("Raw ECG parquet files on the Jetson, one project-relative path per line. Empty uses existing standard p12/p13 recordings. Used to validate a new conversion.");
  configPathEdit_->setToolTip("Project-relative runtime configuration for test runs. The installed service uses its own fixed configuration.");
  emitProbabilitiesCheck_->setToolTip("Print per-frame probabilities during a test run. Requires a JSONL output mode and can produce a large stream of records.");

  console_ = new QPlainTextEdit(central);
  console_->setObjectName("remoteOutput");
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
  auto *splitter = new QSplitter(Qt::Vertical, central);
  splitter->setObjectName("outputSplitter");
  splitter->setHandleWidth(9);
  splitter->addWidget(operationsTabs_);
  auto *outputPanel = new QWidget(splitter);
  auto *outputLayout = new QVBoxLayout(outputPanel);
  outputLayout->setContentsMargins(0, 0, 0, 0);
  auto *commandToggle = new QPushButton("Show remote command", central);
  commandToggle->setCheckable(true);
  commandToggle->setToolTip("Show the optional SSH command field for advanced troubleshooting.");
  commandGroup->hide();
  connect(commandToggle, &QPushButton::toggled, commandGroup, &QWidget::setVisible);
  connect(commandToggle, &QPushButton::toggled, commandToggle, [commandToggle](bool open) {
    commandToggle->setText(open ? "Hide remote command" : "Show remote command");
  });
  auto *outputActions = new QHBoxLayout;
  outputActions->addWidget(commandToggle);
  outputActions->addStretch();
  auto *expandOutput = new QPushButton("Expand output", outputPanel);
  expandOutput->setObjectName("expandOutput");
  expandOutput->setToolTip("Give the output pane the full working area. Click again to restore the controls. You can also drag the divider above this pane.");
  outputActions->addWidget(expandOutput);
  outputActions->addWidget(clearButton);
  outputActions->addWidget(cancelButton_);
  outputLayout->addLayout(outputActions);
  outputLayout->addWidget(commandGroup);
  outputLayout->addWidget(console_, 1);
  splitter->addWidget(outputPanel);
  splitter->setStretchFactor(0, 4);
  splitter->setStretchFactor(1, 1);
  splitter->setSizes({590, 150});
  splitter->handle(1)->setToolTip("Drag up or down to resize the output pane.");
  connect(expandOutput, &QPushButton::clicked, splitter,
          [splitter, expandOutput, previousSizes = QList<int>{590, 150}]() mutable {
    if (splitter->sizes().front() == 0) {
      splitter->setSizes(previousSizes);
      expandOutput->setText("Expand output");
    } else {
      previousSizes = splitter->sizes();
      splitter->setSizes({0, splitter->height()});
      expandOutput->setText("Restore controls");
    }
  });
  connect(splitter, &QSplitter::splitterMoved, expandOutput, [splitter, expandOutput] {
    expandOutput->setText(splitter->sizes().front() == 0 ? "Restore controls" : "Expand output");
  });
  mainLayout->addWidget(splitter, 1);
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
               deploymentRestartCommand(!sudoPasswordEdit_->text().isEmpty(),
                   observedManifestRevision_, observedConfigRevision_, observedRuntimeRevision_),
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
  connect(refreshBundlesButton_, &QPushButton::clicked, this, [this] {
    runCommand("List saved engines", preparedBundlesCommand(), TaskKind::listBundles);
  });
  connect(savedBundleCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this] { updateActionAvailability(); });
  connect(reuseBundleButton_, &QPushButton::clicked, this, [this] {
    if (!reuseBundleButton_->isEnabled()) return;
    const auto *bundle = selectedPreparedBundle();
    if (!bundle) return;
    const auto revision = bundle->reportRevision;
    preparedBundleDirectory_ = bundle->directory;
    bundlePrepared_ = false;
    installStage_ = "reuse-validation";
    installationStateLabel_->setText("Reusing saved engine… The service will stay stopped.");
    activationStateLabel_->setText("Activation: waiting for saved-engine checks");
    runCommand("Reuse saved engine", reuseModelCommand(diagnosticOptions(), revision), TaskKind::installModel);
  });
  connect(refreshModelsButton_, &QPushButton::clicked, this, [this] {
    runCommand(QStringLiteral("List finished models and saved engines"),
               finishedModelsCommand() + preparedBundlesCommand(), TaskKind::listModels);
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
    applyPreparedBundles({});
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
    applyPreparedBundles({});
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
          [this](const QString &, const QString &) {
            commandTimer_.restart();
            activeStandardOutput_.clear();
            activeStandardError_.clear();
            mainStandardOutputDisplayBuffer_.clear();
            mainStandardErrorDisplayBuffer_.clear();
            if (activeTask_ == TaskKind::directInference) {
              capabilities_.directInferenceActive = true;
            }
            updateActionAvailability();
          });
  connect(&runner_, &RemoteCommandRunner::standardOutputReceived, this,
          [this](const QByteArray &data) {
            // Only finite structured responses need complete capture. Live
            // streams and arbitrary console commands retain a bounded tail.
            if (activeTask_ == TaskKind::availabilityProbe || activeTask_ == TaskKind::listModels || activeTask_ == TaskKind::listBundles) {
              activeStandardOutput_.append(data);
            } else if (activeTask_ != TaskKind::installModel) {
              appendOutputTail(activeStandardOutput_, data);
            }
            if (activeTask_ != TaskKind::availabilityProbe && activeTask_ != TaskKind::listModels && activeTask_ != TaskKind::listBundles) {
              appendClassifiedConsoleBytes(mainStandardOutputDisplayBuffer_,
                                           data);
            }
          });
  connect(&runner_, &RemoteCommandRunner::standardErrorReceived, this,
          [this](const QByteArray &data) {
            appendOutputTail(activeStandardError_, data);
            appendClassifiedConsoleBytes(mainStandardErrorDisplayBuffer_, data);
          });
  connect(&runner_, &RemoteCommandRunner::failedToStart, this,
          [this](const QString &message) {
            const bool availabilityCheck =
                activeTask_ == TaskKind::availabilityProbe;
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
          [this](const QString &, const QString &) {
            controlTimer_.restart();
            controlStandardOutputDisplayBuffer_.clear();
            controlStandardErrorDisplayBuffer_.clear();
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
  connect(recoveryPanel_, &RecoveryPanel::deploymentPolicyChanged, this, [this] {
    invalidatePreparedBundle(); syncPreparedBundles(); updateActionAvailability();
  });
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
  // Wheel gestures should scroll the surrounding page, never change a setting,
  // even when its control has keyboard focus. Popup lists still scroll normally.
  for (auto *combo : findChildren<QComboBox *>()) {
    combo->setFocusPolicy(Qt::StrongFocus);
    combo->installEventFilter(this);
  }
  for (auto *spin : findChildren<QAbstractSpinBox *>()) {
    spin->setFocusPolicy(Qt::StrongFocus);
    spin->installEventFilter(this);
  }
  updateDiagnosticModeControls();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
  if (event->type() == QEvent::Wheel &&
      (qobject_cast<QComboBox *>(watched) || qobject_cast<QAbstractSpinBox *>(watched))) {
    event->ignore();
    return true;
  }
  return QMainWindow::eventFilter(watched, event);
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
    // Keep private protocol records intact even when a large catalog arrives
    // across several SSH chunks. Capture only those records for readiness;
    // ordinary installation output is displayed without retaining a second copy.
    if (activeTask_ == TaskKind::installModel &&
        &pending == &mainStandardOutputDisplayBuffer_ && pending.startsWith("__JCG_")) {
      if (newline < 0) break;
      const auto record = pending.left(newline + 1);
      pending.remove(0, newline + 1);
      activeStandardOutput_.append(record);
      applyRuntimeInputEvent(record);
      continue;
    }
    if (newline < 0 && pending.size() < kMaximumOutputTailBytes) {
      break;
    }

    // A producer that never writes a newline must not grow the pending buffer
    // without limit. Ordinary backend JSON records fit within this limit.
    const auto length = newline < 0 ? kMaximumOutputTailBytes
                                   : qMin(newline + 1, kMaximumOutputTailBytes);
    QByteArray line = pending.left(length);
    pending.remove(0, length);
    if (!line.endsWith('\n')) line.append('\n');
    applyRuntimeInputEvent(line);
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
    } else if (installStage_ == "reuse-validation") {
      preparationStateLabel_->setText("Saved engine: checking source bytes and the validation report; no conversion");
    } else if (installStage_ == "activation") {
      preparationStateLabel_->setText("Preparation: validation checks passed. Accuracy and sustained timing remain unverified.");
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
    statusLabel_->setText(error);
    return;
  }
  serviceAvailabilityLabel_->setText(
      QStringLiteral("Checking service availability…"));
  diagnosticAvailabilityLabel_->setText(
      QStringLiteral("Checking direct-runtime availability…"));
  runCommand(QStringLiteral("Full availability probe"),
             availabilityProbeCommand(options), TaskKind::availabilityProbe);
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
  applyPreparedBundles(output);

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
    if (!capabilities_.serviceLinkedToCheckout && capabilities_.serviceUnit) {
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

  const auto scheduling = value("__JCG_SCHEDULING_PROFILE__");
  serviceAvailabilityLabel_->setText(serviceAvailabilityLabel_->text() +
      QString(" Observed service state: %1 (process state, not accuracy or timing validation).")
          .arg(capabilities_.serviceState.isEmpty() ? "unknown" : capabilities_.serviceState));
  serviceAvailabilityLabel_->setText(serviceAvailabilityLabel_->text() +
      (scheduling == "bounded_rr20" ? " Requested service profile: bounded worker RR/20, RT CPU limit 200 ms; actual scheduling and timing require runtime evidence."
       : scheduling == "legacy" ? " Requested service profile: legacy; worker scheduling unconfirmed."
       : " Service scheduling profile: unknown or unsupported."));
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
  syncPreparedBundles();
}

void MainWindow::applyPreparedBundles(const QByteArray &output) {
  preparedBundles_ = parsePreparedBundles(output);
  syncPreparedBundles();
}

const PreparedBundle *MainWindow::selectedPreparedBundle() const {
  const auto directory = savedBundleCombo_->currentData().toString();
  for (const auto &bundle : preparedBundles_)
    if (bundle.directory == directory) return &bundle;
  return nullptr;
}

void MainWindow::syncPreparedBundles() {
  const auto previous = savedBundleCombo_->currentData().toString();
  const auto options = diagnosticOptions();
  const QSignalBlocker blocker(savedBundleCombo_);
  savedBundleCombo_->clear();
  QMap<QString, int> labelCounts;
  for (const auto &bundle : preparedBundles_)
    if (bundle.modelName == options.modelName && bundle.variant == options.variant &&
        bundle.forceRecoveryOff == options.forceRecoveryOff) ++labelCounts[bundle.displayName()];
  QMap<QString, int> labelNumbers;
  for (const auto &bundle : preparedBundles_) {
    if (bundle.modelName != options.modelName || bundle.variant != options.variant ||
        bundle.forceRecoveryOff != options.forceRecoveryOff) continue;
    auto label = bundle.displayName();
    if (labelCounts.value(label) > 1) label += QString(" (%1)").arg(++labelNumbers[label]);
    savedBundleCombo_->addItem(label, bundle.directory);
    savedBundleCombo_->setItemData(savedBundleCombo_->count() - 1,
        label + "\nSaved installation: " + bundle.directory +
        "\nSource config, weights and validation are rechecked on reuse.", Qt::ToolTipRole);
  }
  if (savedBundleCombo_->count() == 0)
    savedBundleCombo_->addItem("No saved engines match this model and startup policy — refresh");
  const auto index = savedBundleCombo_->findData(previous);
  if (index >= 0) savedBundleCombo_->setCurrentIndex(index);
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
  savedBundleCombo_->setEnabled(!anyBusy);
  refreshBundlesButton_->setEnabled(!anyBusy);
  reuseBundleButton_->setEnabled(!anyBusy && selectedPreparedBundle() && parametersValid &&
      !pythonExecutableEdit_->text().trimmed().isEmpty() &&
      !capabilities_.serviceActive && !capabilities_.directInferenceActive);
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
                             ? QStringLiteral("Stop test run")
                             : activeTask_ == TaskKind::serviceLogs ? QStringLiteral("Close live logs")
                                                                 : QStringLiteral("Cancel operation"));

  const QString busyReason = activeTask_ == TaskKind::serviceLogs
      ? "Click Close live logs to finish viewing the stream, then use this action."
      : "Wait for the current operation to finish, or cancel it.";
  const QString idleReason = anyBusy ? busyReason : capabilities_.serviceActive
      ? "Click Stop detector, then Check Jetson to confirm it has stopped."
      : capabilities_.directInferenceActive ? "Stop the test run in Advanced first." : QString();
  const QString checkReason = anyBusy ? busyReason : "Click Check Jetson and resolve the reported readiness issue.";
  const auto *saved = selectedPreparedBundle();
  installedModelLabel_->setText(capabilities_.activeModel.isEmpty()
      ? "Installed model: click Check Jetson to read it."
      : QString("Installed model: %1 (%2). Detector state: %3.")
          .arg(capabilities_.activeModel, capabilities_.activeVariant,
               capabilities_.serviceState.isEmpty() ? "unknown" : capabilities_.serviceState));
  QString next;
  if (anyBusy) next = activeTask_ == TaskKind::serviceLogs
      ? "Viewing live logs. Click Close live logs when you want to use other actions."
      : "An operation is in progress. Its output appears in the console below.";
  else if (hostEdit_->text().trimmed().isEmpty()) next = "Open Connection, enter your Jetson SSH settings, then click Check Jetson.";
  else if (capabilities_.directInferenceActive) next = "A temporary test run is active. Stop test run in Advanced before using the installed detector.";
  else if (capabilities_.serviceActive) next = capabilities_.serviceInputState == "paused"
      ? "The detector is active and waiting for usable input. Use Detector settings for live adjustments; stop it before switching models."
      : "The detector is active. Use Detector settings for live adjustments, or Stop detector before switching models.";
  else if (capabilities_.serviceReady && capabilities_.activeSelectionMatches) next = serviceSafetyCheck_->isChecked()
      ? "Ready: click Start detector to run the installed model."
      : "Ready: review the hardware confirmation below, then click Start detector.";
  else if (!capabilities_.known) next = "Click Check Jetson to read the installed model and detector state. Saved-engine reuse and builds require the detector to be stopped.";
  else if (saved) next = "A saved engine is available. Click Use saved engine to install it, then Start detector when ready.";
  else if (!capabilities_.activeModel.isEmpty() && capabilities_.serviceReady) next = "The installed model differs from your selection. Install the selected model's engine to switch; Start detector runs the installed model shown below.";
  else if (!capabilities_.service && capabilities_.serviceUnit) next = "The Jetson service needs setup. Open Advanced and use Repair service setup, then Check Jetson again.";
  else next = "Select a model and refresh saved engines. Use one if available, or confirm ECG3 and Build and install new engine. Open readiness details for any setup issues.";
  nextStepLabel_->setText("Next step: " + next);

  explainAction(serviceStartButton_, "Start the currently installed model as the Jetson's background detector. Uses the installed hardware configuration; model selection alone does not switch it.",
      anyBusy ? busyReason : capabilities_.directInferenceActive ? idleReason : capabilities_.serviceActive ? "The detector is already active." : !capabilities_.serviceReady ? checkReason : "Review and tick the hardware confirmation first.");
  explainAction(serviceStopButton_, "Stop the installed background detector and release its hardware resources. The installed engine stays available for the next start.", anyBusy ? busyReason : "Click Check Jetson to confirm that the detector is active.");
  explainAction(serviceRestartButton_, "Stop and start the installed detector using its saved startup settings. Live recovery overrides expire at restart; deployment revisions are checked before restarting.", anyBusy ? busyReason : !capabilities_.serviceReady ? checkReason : !serviceSafetyCheck_->isChecked() ? "Review and tick the hardware confirmation first." : "The detector must be active; use Start detector if it is stopped.");
  explainAction(serviceStatusButton_, "Print the installed service's current status in the console. This is a single read and does not change the detector.", checkReason);
  explainAction(serviceLogsButton_, "Show recent service logs and stream new records into the console. Click Close live logs to release the console for other actions; the detector keeps running.", checkReason);
  explainAction(installModelButton_, "Create a fresh ONNX/TensorRT engine from the selected trained weights, validate it, install it and check readiness. This can take several minutes. The service stays stopped until Start detector.",
      !idleReason.isEmpty() ? idleReason : !preparationValid ? validatePreparationOptions(diagnosticOptions()) : "Select a model and click Check Jetson to confirm the build tools and source files.");
  explainAction(reuseBundleButton_, "Verify and install the selected saved engine, then check readiness. Skips ONNX export and TensorRT generation; existing validation checks still run. The service stays stopped until Start detector.",
      !idleReason.isEmpty() ? idleReason : !saved ? "Select a model and startup policy, then Refresh saved engines and choose an entry." : pythonExecutableEdit_->text().trimmed().isEmpty() ? "Set the Jetson Python executable under Build options and manual steps." : optionError);
  explainAction(reexportModelButton_, "Build and validate model files in a new bundle without installing them. Follow with Install files built this session. The main build button performs both steps together.",
      !idleReason.isEmpty() ? idleReason : !preparationValid ? validatePreparationOptions(diagnosticOptions()) : checkReason);
  explainAction(activateModelButton_, "Install the bundle built by Build files only in this session. Checks its report and fingerprints. This does not start the detector; click Check Jetson afterward.",
      !idleReason.isEmpty() ? idleReason : !bundlePrepared_ ? "Complete Build files only first; changing model/build settings clears that selection." : checkReason);
  explainAction(serviceInstallButton_, "Link this checkout's existing service definition into systemd and reload it. Host account and hardware provisioning must already exist. Does not enable or start the detector.", !idleReason.isEmpty() ? idleReason : checkReason);
  explainAction(serviceReloadButton_, "Ask systemd to reread service definitions after editing them. This does not restart the detector or reload its model.", checkReason);
  explainAction(validateRuntimeButton_, "Check the test configuration and deserialize the installed engine without opening UART/GPIO or starting inference.", !idleReason.isEmpty() ? idleReason : checkReason);
  explainAction(startInferenceButton_, "Run the installed engine temporarily with the test settings above. Output stays attached to SSH; use Stop test run when finished. Requires a matching selected model and successful setup validation.", !idleReason.isEmpty() ? idleReason : !gpioApproved ? "Review and tick the GPIO hardware confirmation for this output mode." : checkReason);
  explainAction(stopInferenceButton_, "Send a stop request to the GUI-managed temporary test process. Uses its tracked PID; it does not stop the background service.", "A GUI-managed test run must be active.");
  explainAction(rebuildRuntimeButton_, "Compile the detector's native software on the Jetson. This rebuilds runtime binaries, not model weights or TensorRT engines. Check Jetson again when finished.", !idleReason.isEmpty() ? idleReason : checkReason);
  explainAction(cancelButton_, activeTask_ == TaskKind::serviceLogs ? "Close this log stream while leaving the detector running."
      : activeTask_ == TaskKind::directInference ? "Request a clean stop of the temporary test run."
      : "Cancel the active SSH operation. Remote build children may continue; Check Jetson before retrying.", "Start a command or open live logs first.");
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
    applyPreparedBundles(activeStandardOutput_);
  } else if (completedTask == TaskKind::listBundles && !cancelled &&
             exitStatus == QProcess::NormalExit && exitCode == 0) {
    applyPreparedBundles(activeStandardOutput_);
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
  const bool unknownHostKey =
      activeStandardError_.toLower().contains("host key is known for");
  if (unknownHostKey) {
    const QString guidance = QStringLiteral(
        "Unknown SSH host key — open Connection, verify the host, enable "
        "trust for the next connection, and retry.");
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
      if (installStage_ == "preparation" || installStage_ == "reuse-validation") {
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
