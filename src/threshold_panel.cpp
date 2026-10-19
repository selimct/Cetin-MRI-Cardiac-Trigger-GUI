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
    auto *form = new QFormLayout;
    model_ = label(this);
    values_ = label(this);
    saved_ = label(this);
    active_ = label(this);
    status_ = label(this);
    policy_ = label(this);
    form->addRow("Deployed model", model_);
    form->addRow("Calibration", values_);
    form->addRow("Saved numeric selection", saved_);
    form->addRow("Saved checkbox / fallback", policy_);
    form->addRow("Confirmed detector policy", active_);
    form->addRow("Request status", status_);
    layout->addLayout(form);
    custom_ = new QLineEdit(this);
    custom_->setObjectName("customThreshold");
    custom_->setPlaceholderText("Finite probability, 0 through 1");
    apply_ = new QPushButton("Apply custom", this);
    apply_->setObjectName("applyThreshold");
    auto *customRow = new QHBoxLayout;
    customRow->addWidget(custom_);
    customRow->addWidget(apply_);
    layout->addLayout(customRow);
    auto *buttons = new QHBoxLayout;
    der_ = new QPushButton("Use Weighted DER", this);
    fp_ = new QPushButton("Use Historical FP-first", this);
    reset_ = new QPushButton("Restore automatic", this);
    refresh_ = new QPushButton("Refresh deployed thresholds", this);
    buttons->addWidget(der_);
    buttons->addWidget(fp_);
    buttons->addWidget(reset_);
    buttons->addWidget(refresh_);
    layout->addLayout(buttons);
    early_ = new QCheckBox("Early emission", this);
    early_->setObjectName("earlyEmission");
    early_->setToolTip("Emit earlier on the falling edge when a peak reaches the FP reference level.\n"
                       "Smaller peaks still wait for the selected detection threshold.");
    layout->addWidget(early_);
    auto *help = label(this);
    help->setText("Numeric choices are manual snapshots. Early emission changes trigger timing; calibration error counts do not measure live performance with it enabled. Effective policy changes clear an unfinished trigger candidate. Application waits for native readback and may wait for input.");
    layout->addWidget(help);
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
    model_->setText(controller_->deployment());
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
    custom_->setEnabled(ready);
    der_->setEnabled(ready && m.der.has_value());
    fp_->setEnabled(ready && m.fp.has_value());
    reset_->setEnabled(ready && controller_->canReset());
    reset_->setToolTip("Reset writes the automatic fallback before clearing the manual value. With policy polling disabled, same-invocation startup evidence is required to prove no fallback was accepted.");
    refresh_->setEnabled(!controller_->blocking());
    early_->setEnabled(ready && controller_->policyAvailable());
    const QSignalBlocker blocker(early_);
    early_->setChecked(controller_->selectedEarly());
}
