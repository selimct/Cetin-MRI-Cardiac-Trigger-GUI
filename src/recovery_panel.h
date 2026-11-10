// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#pragma once
#include "recovery_controller.h"
#include <QWidget>
class QLabel;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;
class QPlainTextEdit;
class RecoveryPanel final : public QWidget
{
    Q_OBJECT
  public:
    explicit RecoveryPanel(RecoveryController *controller, QWidget *parent = nullptr);
    bool forceRecoveryOff() const;
    void setDeploymentRecoverySupport(std::optional<bool> supported);
    QWidget *takeDeploymentControls();
  signals:
    void deploymentPolicyChanged();

  private:
    void update();
    RecoveryController *controller_;
    QComboBox *deployment_, *thresholdMode_;
    QCheckBox *enabled_;
    QLineEdit *custom_;
    QPushButton *apply_, *restore_, *refresh_;
    QLabel *model_, *calibrated_, *saved_, *active_, *status_, *summary_;
    QWidget *deploymentGroup_;
    QPlainTextEdit *information_;
    bool edited_{false}, supportForDraft_{false};
    QString draftIdentity_;
    std::optional<bool> deploymentSupport_;
};
