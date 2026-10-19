// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#pragma once
#include "threshold_controller.h"
#include <QWidget>
class QLabel;
class QLineEdit;
class QPushButton;
class QCheckBox;

class ThresholdPanel final : public QWidget
{
    Q_OBJECT
public:
    explicit ThresholdPanel(ThresholdController *controller, QWidget *parent = nullptr);

private:
    void update();
    ThresholdController *controller_;
    QLabel *model_, *values_, *saved_, *active_, *status_, *policy_;
    QLineEdit *custom_;
    QPushButton *refresh_, *apply_, *der_, *fp_, *reset_;
    QCheckBox *early_;
};
