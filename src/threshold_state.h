// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <optional>

namespace thresholds
{
    // All parsing and policy reconciliation also run in tests without SSH or hardware.
    QMap<QString, QString> settings(const QByteArray &text);
    double probability(const QString &text);
    bool equal(double a, double b);

    struct Manifest
    {
        QByteArray contents;
        QString revision;
        QMap<QString, QString> values;
        double automatic{};
        std::optional<double> manual, fp, der;
        static Manifest parse(const QByteArray &text);
        QJsonObject identity() const;
        bool fingerprinted() const;
        QByteArray patchManual(const QString &value) const;
        QString numericChoice(const QString &source, const QString &custom) const;
        QByteArray policy(const QString &number, bool early) const;
    };

    QString sourceLabel(const QString &source);

    struct Readback
    {
        std::optional<bool> manual, early;
        std::optional<double> configured, active, emission;
        std::optional<double> queuedThreshold;
        std::optional<bool> queuedEarly, queuedManual;
        bool paused{false};
        QString cursor, message;
        qint64 acceptedAt{}, activeAt{}, observedAt{};
        void observe(const QJsonObject &event, qint64 timestamp, const QString &journalCursor);
        bool matches(double value, bool manualExpected, std::optional<bool> earlyExpected,
                     qint64 after) const;
    };

    // Linux host commands; all values supplied by a remote host are shell quoted.
    QString deploymentLock(const QString &command);
    QString atomicWrite(const QString &path, const QByteArray &contents,
                        const QString &owner, const QString &group);
    QString checkedWrite(const Manifest &manifest, const QString &manifestPath,
                         const QString &policyPath, const QString &policyRevision,
                         const QString &configPath, const QString &configRevision,
                         const QString &invocation, const QString &service,
                         const QString &action, const QString &number, bool early,
                         const QString &owner, const QString &group);
} // namespace thresholds
