// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#include "recovery_panel.h"
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

RecoveryPanel::RecoveryPanel(RecoveryController *controller, QWidget *parent)
    : QWidget(parent), controller_(controller)
{
    auto *layout = new QVBoxLayout(this);
    auto *deploymentGroup = new QGroupBox("Recovery after restart", this);
    deploymentGroup_ = deploymentGroup;
    auto *deploymentLayout = new QVBoxLayout(deploymentGroup);
    deployment_ = new QComboBox(deploymentGroup);
    deployment_->setObjectName("recoveryDeploymentPolicy");
    deployment_->addItem("Use saved recovery settings", false);
    deployment_->addItem("Recovery off — current output only", true);
    deployment_->setToolTip(
        "Saved settings enable recovery only when the model enables it and has a calibrated threshold. "
        "Recovery off uses --no-recovery during preparation and activation and keeps both model outputs.");
    auto *restoreDeployment = new QPushButton("Reset selection", deploymentGroup);
    restoreDeployment->setObjectName("restoreRecoveryDeployment");
    restoreDeployment->setToolTip("Choose Use saved recovery settings again. Only changes this local selection; install or use an engine to change startup behavior.");
    auto *deploymentRow = new QHBoxLayout;
    deploymentRow->addWidget(deployment_, 1);
    deploymentRow->addWidget(restoreDeployment);
    deploymentLayout->addLayout(deploymentRow);
    auto *explanation = new QLabel(
        "Choose the recovery policy for the engine you install or reuse. This selection takes effect after installation and the next detector start.",
        deploymentGroup);
    explanation->setWordWrap(true);
    deploymentLayout->addWidget(explanation);
    layout->addWidget(deploymentGroup);
    auto *live = new QGroupBox("Recovery during this run", this);
    auto *form = new QFormLayout(live);
    auto *intro = new QLabel("Edit the controls, then Apply recovery for this run. Refresh afterward to confirm. These changes expire when the detector restarts.", live);
    intro->setWordWrap(true);
    form->addRow(intro);
    summary_ = new QLabel(live);
    summary_->setWordWrap(true);
    summary_->setObjectName("recoverySummary");
    summary_->setTextFormat(Qt::PlainText);
    form->addRow(summary_);
    auto *details = new QWidget(live);
    details->setObjectName("recoveryDetails");
    auto *detailForm = new QFormLayout(details);
    const auto label = [live]
    {
        auto *l = new QLabel(live);
        l->setWordWrap(true);
        l->setTextFormat(Qt::PlainText);
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return l;
    };
    model_ = label();
    saved_ = label();
    active_ = label();
    status_ = label();
    status_->setObjectName("recoveryStatus");
    active_->setObjectName("activeRecovery");
    calibrated_ = label();
    calibrated_->setObjectName("calibratedRecovery");
    form->addRow("Calibrated recovery", calibrated_);
    detailForm->addRow("Deployed model", model_);
    form->addRow("Saved and requested settings", saved_);
    form->addRow("Confirmed active settings", active_);
    detailForm->addRow("Status", status_);
    enabled_ = new QCheckBox("Enable recovery (press Apply to send)", live);
    enabled_->setObjectName("recoveryEnabled");
    form->addRow(enabled_);
    thresholdMode_ = new QComboBox(live);
    thresholdMode_->setObjectName("recoveryThresholdMode");
    thresholdMode_->addItem("Use calibrated threshold");
    thresholdMode_->addItem("Advanced: custom threshold");
    thresholdMode_->addItem("Keep selected threshold");
    thresholdMode_->setToolTip("Keep selected uses the last numeric recovery threshold. Calibrated uses the trained model's saved value. Custom lets you enter a value. Press Apply to send your selection.");
    custom_ = new QLineEdit(live);
    custom_->setObjectName("customRecoveryThreshold");
    custom_->setPlaceholderText("Explicit finite threshold in (0, 1]");
    form->addRow("Edited threshold selection", thresholdMode_);
    form->addRow("Custom recovery threshold", custom_);
    auto *buttons = new QHBoxLayout;
    apply_ = new QPushButton("Apply recovery for this run", live);
    apply_->setObjectName("applyRecovery");
    restore_ = new QPushButton("Restore saved recovery policy", live);
    restore_->setObjectName("restoreRecovery");
    refresh_ = new QPushButton("Refresh recovery state", live);
    refresh_->setObjectName("refreshRecovery");
    buttons->addWidget(apply_);
    buttons->addWidget(restore_);
    buttons->addWidget(refresh_);
    form->addRow(buttons);
    auto *scope = new QLabel("Apply and Restore change the live override for this service invocation. "
        "After restart, the persistent startup policy applies. The last selected threshold is retained. "
        "Refresh to confirm native application.", live);
    scope->setWordWrap(true);
    form->addRow(scope);
    information_ = new QPlainTextEdit(live);
    information_->setObjectName("recoveryInformation");
    information_->setReadOnly(true);
    information_->setMinimumHeight(200);
    detailForm->addRow("Calibration / provenance", information_);
    auto *showDetails = new QPushButton("Model and calibration details", live);
    showDetails->setObjectName("showRecoveryDetails");
    showDetails->setCheckable(true);
    form->addRow(showDetails);
    form->addRow(details);
    details->hide();
    connect(showDetails, &QPushButton::toggled, details, &QWidget::setVisible);
    layout->addWidget(live);
    connect(deployment_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { emit deploymentPolicyChanged(); });
    connect(restoreDeployment, &QPushButton::clicked, this, [this] { deployment_->setCurrentIndex(0); });
    connect(enabled_, &QCheckBox::toggled, this, [this] { edited_ = true; });
    connect(thresholdMode_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this]
            {
                edited_ = true;
                update();
            });
    connect(custom_, &QLineEdit::textEdited, this, [this] { edited_ = true; });
    connect(refresh_, &QPushButton::clicked, controller_, &RecoveryController::refresh);
    connect(restore_, &QPushButton::clicked, controller_, &RecoveryController::restoreCalibrated);
    connect(
        apply_, &QPushButton::clicked, this, [this]
        {
            if (thresholdMode_->currentIndex() == 2)
                controller_->setEnabled(enabled_->isChecked());
            else
                controller_->apply(enabled_->isChecked(), thresholdMode_->currentIndex() == 1, custom_->text());
        });
    connect(controller_, &RecoveryController::changed, this, &RecoveryPanel::update);
    connect(controller_, &RecoveryController::invalidated, this,
            [this]
            {
                edited_ = false;
                draftIdentity_.clear();
                supportForDraft_ = false;
            });
    update();
}
bool RecoveryPanel::forceRecoveryOff() const
{
    return deploymentSupport_ != false && deployment_->currentData().toBool();
}
void RecoveryPanel::setDeploymentRecoverySupport(std::optional<bool> supported)
{
    deploymentSupport_ = supported;
    update();
}
QWidget *RecoveryPanel::takeDeploymentControls()
{
    layout()->removeWidget(deploymentGroup_);
    deploymentGroup_->setParent(nullptr);
    return deploymentGroup_;
}
void RecoveryPanel::update()
{
    const auto &s = controller_->state();
    const auto active = s.api.value("active").toObject();
    QString summary;
    if (s.manifest.contents.isEmpty())
        summary = "Check the Jetson to read recovery status.";
    else if (!s.supportsRecovery())
        summary = "Recovery unavailable: single-output model";
    else if (!s.liveSupported())
    {
        summary = s.manifest.values.value("recovery_threshold").isEmpty()
                      ? "Saved startup: recovery off (current output only)."
                      : "Saved startup: recovery on.";
        summary += " Live switching is unavailable on this stack; choose Recovery after restart in Run, then use a saved engine or build and install a new one. "
                   "Active recovery state is unconfirmed.";
    }
    else if (s.api.value("confirmation") != "confirmed" || !active.value("enabled").isBool())
        summary = s.api.value("input_paused").toBool() ? "Waiting for input to confirm recovery settings."
                                                       : "Waiting for confirmation of recovery settings.";
    else
        summary = active.value("enabled").toBool() ? "Recovery on" : "Recovery off";
    if (s.sourceAvailable && s.supportsRecovery() && s.source.value("recovery_threshold").isNull())
    {
        if (active.value("enabled").toBool())
            summary += " (custom policy; saved calibration disables recovery)";
        else
            summary += " Recovery disabled by calibration (saved settings).";
    }
    const auto operation = controller_->status();
    if (operation.startsWith("Recovery threshold") || operation.startsWith("Threshold must") ||
        operation.startsWith("Recovery disabled by calibration or"))
        summary = operation;
    else if (operation.startsWith("Unconfirmed / rejected"))
        summary = "Recovery status could not be confirmed. Refresh or open the details.";
    summary_->setText(summary);
    model_->setText(s.manifest.values.value("model_name", "Unconfirmed") + " / " +
                    s.manifest.values.value("model_variant") +
                    (s.manifest.contents.isEmpty() ? ""
                     : s.supportsRecovery()        ? " — recovery supported"
                                                   : " — Recovery unavailable: single-output model"));
    if (s.manifest.fingerprinted())
        model_->setText(model_->text() + "; engine CRC32 " + s.manifest.values.value("engine_crc32") +
                        "; bytes " + s.manifest.values.value("engine_size_bytes"));
    saved_->setText(s.manifest.contents.isEmpty() ? "Not read — click Refresh recovery state." : s.requested());
    calibrated_->setText(
        !s.manifest.contents.isEmpty() && !s.supportsRecovery() ? "Not applicable: single-output model"
        : !s.sourceAvailable ? "Unavailable / unconfirmed"
        : s.calibrated() ? QString::number(*s.calibrated(), 'g', 12) +
            (s.sourceEnabled() ? "; source default enabled" : "; source default disabled") +
            (s.sourceUnchanged ? "" : "; source changed since deployment — restore unavailable")
        : s.source.value("recovery_threshold").isNull() ? "Disabled by calibration (null threshold)"
        : "Unavailable / invalid source threshold");
    active_->setText(s.active());
    status_->setText(controller_->status());
    information_->setPlainText(s.information());
    const auto identity =
        QString::fromUtf8(QJsonDocument(s.manifest.identity()).toJson(QJsonDocument::Compact));
    if (!s.manifest.contents.isEmpty() && identity != draftIdentity_)
    {
        draftIdentity_ = identity;
        edited_ = false;
        supportForDraft_ = false;
    }
    if (!edited_ && !s.api.isEmpty())
    {
        const QSignalBlocker a(enabled_), b(thresholdMode_), c(custom_);
        enabled_->setChecked(s.api.value("requested").toObject().value("enabled").toBool());
        const auto selected = s.api.value("selected_threshold");
        thresholdMode_->setCurrentIndex(2);
        custom_->setText(selected.isDouble() ? QString::number(selected.toDouble(), 'g', 17) : QString());
    }
    // Editing drafts never sends a command. Refresh leaves existing drafts intact.
    if (!controller_->blocking())
        supportForDraft_ = s.supportsRecovery() && s.liveSupported();
    const bool supported =
        (s.supportsRecovery() && s.liveSupported()) || (controller_->blocking() && supportForDraft_);
    enabled_->setEnabled(supported);
    thresholdMode_->setEnabled(supported);
    custom_->setEnabled(supported && thresholdMode_->currentIndex() == 1);
    const QString unavailable = controller_->ready() ? QString()
        : "\nRequires a running two-output model with live recovery support. Start it from Run and Refresh recovery state. Current status: " + controller_->status();
    enabled_->setToolTip("Edit the requested recovery enabled state. Click Apply recovery for this run to send it; changing this checkbox alone sends nothing." + unavailable);
    custom_->setToolTip("Recovery threshold must be greater than 0 and at most 1. Select Advanced: custom threshold, enter a value, then Apply recovery for this run." + unavailable);
    apply_->setToolTip("Send the edited recovery state and threshold to the current service run. Does not change restart defaults. Refresh recovery state to confirm application; paused input can delay it." + unavailable);
    restore_->setToolTip("Request the source model's saved recovery enabled state and calibrated threshold for this run. Can restore disabled recovery too. Refresh to confirm." + unavailable +
        (!s.sourceAvailable || !s.sourceUnchanged ? "\nSaved source calibration is unavailable or changed since installation." : ""));
    refresh_->setToolTip("Read the installed model's recovery capability, saved calibration, pending request and active settings. Does not send a recovery change.");
    apply_->setEnabled(controller_->ready());
    restore_->setEnabled(controller_->ready() && s.sourceAvailable && s.sourceUnchanged);
    refresh_->setEnabled(controller_->refreshAvailable());
    deploymentGroup_->setEnabled(controller_->refreshAvailable() && deploymentSupport_ != false);
    deploymentGroup_->setToolTip(deploymentSupport_ == false ? "Recovery unavailable: selected model has a single output" : "");
}
