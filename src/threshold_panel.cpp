// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#include "threshold_panel.h"
#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace
{
    QString number(std::optional<double> value)
    {
        return value ? QString::number(*value, 'g', 12) : "Unavailable";
    }
    QLabel *label(QWidget *parent)
    {
        auto *value = new QLabel(parent);
        value->setWordWrap(true);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setTextFormat(Qt::PlainText);
        return value;
    }
}
ThresholdPanel::ThresholdPanel(ThresholdController *controller, QWidget *parent)
    : QWidget(parent), controller_(controller)
{
    auto *layout = new QVBoxLayout(this);
    auto *heading = new QLabel("<b>Primary detection threshold</b>", this);
    layout->addWidget(heading);
    auto *intro = label(this);
    intro->setText("Set the main event-detection threshold. Applied changes are saved for future starts. Refresh active settings afterward to confirm what the running detector accepted.");
    layout->addWidget(intro);
    auto *form = new QFormLayout;
    auto *details = new QWidget(this);
    auto *detailsForm = new QFormLayout(details);
    model_ = label(this);
    values_ = label(this);
    saved_ = label(this);
    active_ = label(this);
    status_ = label(this);
    policy_ = label(this);
    detailsForm->addRow("Installed model", model_);
    detailsForm->addRow("Model calibration", values_);
    form->addRow("Saved for next start", saved_);
    detailsForm->addRow("Saved timing policy", policy_);
    form->addRow("Confirmed active settings", active_);
    form->addRow("Last request", status_);
    layout->addLayout(form);
    custom_ = new QLineEdit(this);
    custom_->setObjectName("customThreshold");
    custom_->setPlaceholderText("Finite probability, 0 through 1");
    apply_ = new QPushButton("Apply threshold", this);
    apply_->setObjectName("applyThreshold");
    auto *customRow = new QHBoxLayout;
    customRow->addWidget(custom_);
    customRow->addWidget(apply_);
    layout->addLayout(customRow);
    auto *buttons = new QHBoxLayout;
    der_ = new QPushButton("Use DER preset", this);
    fp_ = new QPushButton("Use FP-first preset", this);
    reset_ = new QPushButton("Restore model default", this);
    reset_->setObjectName("restorePrimaryThreshold");
    refresh_ = new QPushButton("Refresh active settings", this);
    refresh_->setObjectName("refreshThresholds");
    auto *presetButtons = new QHBoxLayout;
    presetButtons->addWidget(der_);
    presetButtons->addWidget(fp_);
    detailsForm->addRow("Other saved presets", presetButtons);
    buttons->addWidget(reset_);
    buttons->addWidget(refresh_);
    layout->addLayout(buttons);
    early_ = new QCheckBox("Emit qualifying peaks earlier (applies immediately)", this);
    early_->setObjectName("earlyEmission");
    early_->setToolTip("Emit earlier on the falling edge when a peak reaches the FP reference level.\n"
                       "Smaller peaks still wait for the selected detection threshold.");
    layout->addWidget(early_);
    auto *help = label(this);
    help->setText("The saved presets are calibration values from the installed model. Applying one saves a manual primary selection; Restore model default returns to automatic selection. "
        "Early-emission timing is saved separately and survives a restart. Changes clear any unfinished primary candidate. "
        "Refresh active settings confirms application; paused input can delay it. Runtime changes do not recalibrate the model.");
    // Keep the runtime/calibration bookkeeping available without crowding the
    // normal edit/apply/confirm flow.
    detailsForm->addRow(help);
    auto *showDetails = new QPushButton("Show presets, calibration and timing", this);
    showDetails->setCheckable(true);
    showDetails->setToolTip("Expand saved calibration, model identity and fallback/timing information.");
    layout->addWidget(showDetails);
    layout->addWidget(details);
    details->hide();
    connect(showDetails, &QPushButton::toggled, details, &QWidget::setVisible);
    connect(showDetails, &QPushButton::toggled, showDetails, [showDetails](bool open) {
        showDetails->setText(open ? "Hide presets, calibration and timing" : "Show presets, calibration and timing");
    });
    layout->addStretch();
    connect(refresh_, &QPushButton::clicked, controller_, &ThresholdController::refresh);
    connect(apply_, &QPushButton::clicked, this, [this]
            { controller_->apply("custom", custom_->text()); });
    connect(der_, &QPushButton::clicked, this, [this]
            { controller_->apply("der_preset"); });
    connect(fp_, &QPushButton::clicked, this, [this]
            { controller_->apply("fp_preset"); });
    connect(reset_, &QPushButton::clicked, controller_, &ThresholdController::restoreAutomatic);
    connect(early_, &QCheckBox::clicked, controller_, &ThresholdController::setEarlyEmission);
    connect(controller_, &ThresholdController::changed, this, &ThresholdPanel::update);
    update();
}
void ThresholdPanel::update()
{
    const auto &m = controller_->manifest();
    const auto &r = controller_->readback();
    QString timing;
    if (!m.contents.isEmpty()) {
        const auto size = m.values.value("frame_size_raw").toDouble();
        const auto hz = m.values.value("raw_hz").toDouble();
        timing = m.values.value("prediction_mode", "current") == "delayed_100hz"
            ? "\n100 Hz; previous-frame primary; 10 ms context"
            : "\nCurrent-frame primary";
        if (size > 0 && hz > 0) {
            timing += QString("; %1 Hz").arg(hz / size, 0, 'g', 6);
            bool windowOk = false, holdoffOk = false;
            const auto window = m.values.value("window_size").toDouble(&windowOk);
            const auto holdoff = m.values.value("holdoff_frames").toDouble(&holdoffOk);
            timing += windowOk && window > 0 ? QString("; window %1 s").arg(window * size / hz, 0, 'g', 6) : "; window unavailable";
            timing += holdoffOk && holdoff >= 0 ? QString("; holdoff %1 ms").arg(holdoff * size / hz * 1000, 0, 'g', 6) : "; holdoff unavailable";
        }
    }
    model_->setText(controller_->deployment() + timing);
    values_->setText(m.contents.isEmpty() ? "Unavailable" : QString("Automatic %1; Weighted DER %2; Historical FP-first %3").arg(number(m.automatic), number(m.der), number(m.fp)));
    saved_->setText(m.contents.isEmpty() ? "Unavailable" : controller_->savedSource() + (m.manual ? ": " + number(m.manual) : "; no manual value saved; model default " + number(m.automatic)));
    policy_->setText(controller_->savedPolicy() + QString("; configured early-emission default %1")
                                                      .arg(controller_->configuredEarly() ? "on" : "off"));
    active_->setText(r.active ? QString("Threshold %1; early emission %2; emission level %3\nAccepted manual %4; configured threshold %5%6")
                                    .arg(number(r.active), r.early.value_or(false) ? "on" : "off", number(r.emission),
                                         r.manual ? (*r.manual ? "yes" : "no") : "unknown", number(r.configured),
                                         r.message.isEmpty() ? QString() : "\nNative message: " + r.message)
                              : "Unconfirmed — status reporting unavailable");
    status_->setText(controller_->status());
    const bool ready = controller_->ready();
    apply_->setEnabled(ready);
    // This is an unsaved draft. Refreshes and pending writes must not interrupt
    // typing or steal focus; only submitting the value requires readiness.
    der_->setEnabled(ready && m.der.has_value());
    fp_->setEnabled(ready && m.fp.has_value());
    reset_->setEnabled(ready && controller_->canReset());
    const QString prerequisite = ready ? QString() : "\nStart the installed detector from Run, then Refresh active settings. Refresh also confirms a pending request. Current status: " + controller_->status();
    apply_->setToolTip("Save the entered value (0 through 1) as the primary threshold and request live application. Also applies after restart. Refresh to confirm; paused input can delay application." + prerequisite);
    der_->setToolTip("Use the installed model's saved Weighted DER calibration as a manual primary threshold. Saves for restart and requests live application. This does not recalibrate the model." + (m.der ? prerequisite : "\nThe installed model has no DER preset."));
    fp_->setToolTip("Use the installed model's saved historical FP-first calibration as a manual primary threshold. Saves for restart and requests live application. This does not recalibrate the model." + (m.fp ? prerequisite : "\nThe installed model has no FP-first preset."));
    reset_->setToolTip("Restore the model's calibrated automatic primary threshold and clear the saved manual selection. Keeps the chosen early-emission timing policy. Refresh afterward to confirm." + prerequisite);
    refresh_->setToolTip("Read saved thresholds and recent status from the running installed detector. Confirms whether the last request has applied. Makes no setting changes.");
    refresh_->setEnabled(!controller_->blocking());
    early_->setEnabled(ready && controller_->policyAvailable());
    early_->setToolTip("Clicking immediately saves and requests this timing policy. Qualifying peaks that reach the reference level can be emitted earlier on the falling edge. Smaller peaks wait for the detection threshold. Persists across restarts; Refresh active settings to confirm." + prerequisite);
    const QSignalBlocker blocker(early_);
    early_->setChecked(controller_->selectedEarly());
}
