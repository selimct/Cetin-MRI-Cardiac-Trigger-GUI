// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#include "diagnostic_command_builder.h"

#include "ssh_command_builder.h"
#include "recovery_state.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
#include <cmath>

namespace {
constexpr auto kRuntimeExecutable =
    "build_native/inference/native_uart_live_runtime";
constexpr auto kServiceName = "dkfz-native-uart-live";
constexpr auto kServiceUnit = "deploy/dkfz-native-uart-live.service";
constexpr auto kInstalledServiceUnit =
    "/etc/systemd/system/dkfz-native-uart-live.service";
constexpr auto kServiceConfig = "config/native_uart_live.conf";
constexpr auto kActiveManifest = "deploy/model/model.conf";
constexpr auto kActiveEngine = "deploy/model/model.engine";

bool isSafeRelativePath(const QString &path, bool allowEmpty) {
  if (path.isEmpty()) {
    return allowEmpty;
  }
  if (QDir::isAbsolutePath(path) || path.contains(QChar::Null) ||
      path.contains(QChar::LineFeed) || path.contains(QChar::CarriageReturn)) {
    return false;
  }

  const QString clean = QDir::cleanPath(path);
  return clean != QStringLiteral(".") && clean != QStringLiteral("..") &&
         !clean.startsWith(QStringLiteral("../"));
}

QString runtimeBaseCommand(const DiagnosticRuntimeOptions &options) {
  QStringList arguments{
      QString::fromLatin1(kRuntimeExecutable),
      QStringLiteral("--config"),
      posixShellQuote(QDir::cleanPath(options.configPath)),
      QStringLiteral("--manifest"),
      posixShellQuote(diagnosticManifestPath()),
      QStringLiteral("--trigger-output"),
      posixShellQuote(options.outputMode),
      // Always override the committed config so an empty GUI field really
      // disables raw capture instead of inheriting a stale capture path.
      QStringLiteral("--capture-byte-file"),
      posixShellQuote(options.rawCapturePath.isEmpty()
                          ? QString()
                          : QDir::cleanPath(options.rawCapturePath)),
  };
  if (options.emitProbabilities) {
    arguments.append(QStringLiteral("--emit-probabilities"));
  }
  return arguments.join(QChar::Space);
}

QString inputRecoveryConfigCheck(const QString &quotedConfig) {
  return QStringLiteral(
             "grep -Eq -- '^[[:space:]]*input_reset_enabled[[:space:]]*="
             "[[:space:]]*true[[:space:]]*$' %1 && "
             "grep -Eq -- '^[[:space:]]*ecg_baseline_reset_ms[[:space:]]*="
             "[[:space:]]*[1-9][0-9]*[[:space:]]*$' %1 && "
             "grep -Eq -- '^[[:space:]]*stuck_signal_reset_ms[[:space:]]*="
             "[[:space:]]*[1-9][0-9]*[[:space:]]*$' %1 && "
             "grep -Eq -- '^[[:space:]]*input_recovery_ms[[:space:]]*="
             "[[:space:]]*[1-9][0-9]*[[:space:]]*$' %1 && "
             "grep -Eq -- '^[[:space:]]*watchdog_enabled[[:space:]]*="
             "[[:space:]]*true[[:space:]]*$' %1 && "
             "grep -Eq -- '^[[:space:]]*uart_timeout_ms[[:space:]]*="
             "[[:space:]]*[1-9][0-9]*[[:space:]]*$' %1 && "
             "grep -Eq -- '^[[:space:]]*sample_timeout_ms[[:space:]]*="
             "[[:space:]]*[1-9][0-9]*[[:space:]]*$' %1")
      .arg(quotedConfig);
}

QString stateVariables() {
  return QStringLiteral(
      "state_dir=\"${XDG_RUNTIME_DIR:-/tmp}/dkfz-jetson-control-${UID}\"; "
      "pid_file=\"${state_dir}/native_uart_live_runtime.pid\"");
}

QString preparationInputCheck(const DiagnosticRuntimeOptions &options) {
  const QString modelDirectory =
      QDir::cleanPath(options.modelDirectory) + QChar::fromLatin1('/');
  const bool tuned = options.variant == QStringLiteral("tuned");
  const QString config =
      modelDirectory + (tuned ? QStringLiteral("tuning/tuned_config.json")
                              : QStringLiteral("train_config.json"));
  const QString checkpoint =
      modelDirectory +
      (tuned ? QStringLiteral("best_tuned.pth") : QStringLiteral("best.pth"));
  QString check =
      QStringLiteral("test -f %1 && test -f %2")
          .arg(posixShellQuote(config), posixShellQuote(checkpoint));
  // Isolated preparation always exports from the saved checkpoint.
  return check;
}

QString idleDetectorGuard() {
  return QStringLiteral(
      "service_state=\"$(systemctl is-active %1.service 2>/dev/null || true)\"; "
      "case \"$service_state\" in inactive|failed) ;; active|activating|reloading|deactivating) "
      "printf 'Stop the service before preparing or activating a model.\\n' >&2; exit 3;; *) "
      "test \"$(systemctl show --value -p LoadState %1.service 2>/dev/null || true)\" = not-found || "
      "{ printf 'Cannot verify that the service is stopped. Check again.\\n' >&2; exit 3; };; esac; "
      "%2; if test -s \"$pid_file\" && kill -0 \"$(cat -- \"$pid_file\")\" 2>/dev/null; then "
      "printf 'Stop direct inference before preparing or activating a model.\\n' >&2; exit 3; fi; ")
      .arg(QString::fromLatin1(kServiceName), stateVariables());
}

QString serviceSchedulingCheck() {
  return QString::fromUtf8(R"SH(
scheduling_profile=unknown
if grep -Fqx -- 'ExecStart=/opt/dkfz-live/build_native/inference/native_uart_live_runtime --config /opt/dkfz-live/config/native_uart_live.conf' deploy/dkfz-native-uart-live.service; then
  scheduling_profile=legacy
elif grep -Fqx -- 'ExecStart=/opt/dkfz-live/build_native/inference/native_uart_live_runtime --config /opt/dkfz-live/config/native_uart_live.conf --realtime-priority 20 --require-realtime' deploy/dkfz-native-uart-live.service &&
     test "$(systemctl show --value -p LimitRTPRIO dkfz-native-uart-live.service)" = 20 &&
     test "$(systemctl show --value -p LimitRTPRIOSoft dkfz-native-uart-live.service)" = 20 &&
     test "$(systemctl show --value -p LimitRTTIME dkfz-native-uart-live.service)" = 200000 &&
     test "$(systemctl show --value -p LimitRTTIMESoft dkfz-native-uart-live.service)" = 200000 &&
     test "$(systemctl show --value -p CPUSchedulingPolicy dkfz-native-uart-live.service)" = 0 &&
     test "$(systemctl show --value -p CPUWeight dkfz-native-uart-live.service)" = 1000 &&
     test "$(systemctl show --value -p TimerSlackNSec dkfz-native-uart-live.service)" = 1000 &&
     scheduling_help=$(build_native/inference/native_uart_live_runtime --help 2>/dev/null) &&
     printf '%s' "$scheduling_help" | grep -Fq -- '--realtime-priority' &&
     printf '%s' "$scheduling_help" | grep -Fq -- '--require-realtime'; then
  scheduling_profile=bounded_rr20
fi
)SH");
}
} // namespace

QString validateDiagnosticOptions(const DiagnosticRuntimeOptions &options) {
  static const QRegularExpression modelPattern(
      QStringLiteral("^[A-Za-z0-9._-]+$"));
  if (!modelPattern.match(options.modelName).hasMatch()) {
    return QStringLiteral("Model name must contain only letters, digits, dots, "
                          "underscores, and hyphens.");
  }
  if (options.variant != QStringLiteral("best") &&
      options.variant != QStringLiteral("tuned")) {
    return QStringLiteral("Model variant must be 'best' or 'tuned'.");
  }
  if (options.preparationMode != QStringLiteral("jetson-only") &&
      options.preparationMode != QStringLiteral("complete")) {
    return QStringLiteral("Unsupported Jetson preparation mode.");
  }
  if (!isSafeRelativePath(options.modelDirectory, false)) {
    return QStringLiteral(
        "Model source directory must stay beneath the remote project root.");
  }
  static const QStringList outputModes{
      QStringLiteral("none"),
      QStringLiteral("jsonl"),
      QStringLiteral("gpio"),
      QStringLiteral("gpio+jsonl"),
  };
  if (!outputModes.contains(options.outputMode)) {
    return QStringLiteral("Unsupported trigger output mode.");
  }
  if (options.emitProbabilities &&
      !options.outputMode.contains(QStringLiteral("jsonl"))) {
    return QStringLiteral(
        "Probability output requires jsonl or gpio+jsonl mode.");
  }
  if (!isSafeRelativePath(options.configPath, false)) {
    return QStringLiteral(
        "Config path must stay beneath the remote project root.");
  }
  if (!isSafeRelativePath(options.sessionLogPath, true)) {
    return QStringLiteral(
        "Session log path must stay beneath the remote project root.");
  }
  if (!isSafeRelativePath(options.rawCapturePath, true)) {
    return QStringLiteral(
        "Raw capture path must stay beneath the remote project root.");
  }
  return {};
}

QString diagnosticManifestPath() {
  return QString::fromLatin1(kActiveManifest);
}

QString validatePreparationOptions(const DiagnosticRuntimeOptions &options) {
  const auto error = validateDiagnosticOptions(options);
  if (!error.isEmpty()) return error;
  const auto bundle = QDir::cleanPath(options.bundleDirectory);
  if (!isSafeRelativePath(options.bundleDirectory, false) || bundle == "deploy" || bundle.startsWith("deploy/"))
    return "Choose a new bundle directory beneath the project root, outside deploy/.";
  if (options.validationInputs.isEmpty() && !options.autoValidationInputs)
    return "Add representative raw parquet validation recordings.";
  for (const auto &input : options.validationInputs)
    if (!isSafeRelativePath(input, false)) return "Validation recordings must stay beneath the remote project root.";
  if (!options.ecg3SourceConfirmed) return "Confirm training uses ECG2 − ECG1 for the UART-derived ECG3 channel.";
  if (options.pythonExecutable.isEmpty() || options.pythonExecutable.contains(QChar::Null) ||
      options.pythonExecutable.contains('\n') || options.pythonExecutable.contains('\r'))
    return "Enter the Jetson Python executable.";
  return {};
}

QString serviceUnitRepairCommand(bool sudoPasswordProvided) {
  const QString sudoCommand =
      sudoPasswordProvided ? QStringLiteral("sudo -S -p '' --")
                           : QStringLiteral("sudo -n --");
  const QString privilegedScript =
      QStringLiteral("ln -sfnT -- \"$1\" %1 && systemctl daemon-reload")
          .arg(posixShellQuote(QString::fromLatin1(kInstalledServiceUnit)));

  return QStringLiteral(
             "unit_path=\"$(readlink -f -- %1 2>/dev/null)\"; "
             "if test -z \"$unit_path\" || ! test -f \"$unit_path\" || "
             "! test -r \"$unit_path\"; then "
             "printf 'Checkout service unit is missing or unreadable: %1\\n' "
             ">&2; exit 2; fi; "
             "service_state=\"$(systemctl is-active %2.service 2>/dev/null || "
             "true)\"; "
             "case \"$service_state\" in "
             "active|activating|reloading|deactivating) "
             "printf 'Refusing to replace the unit link while %2.service is "
             "%s. Stop it first.\\n' \"$service_state\" >&2; exit 3;; "
             "esac; "
             "%3 bash -c %4 bash \"$unit_path\" && "
             "unit_fragment=\"$(systemctl show --property=FragmentPath "
             "--value %2.service 2>/dev/null)\"; "
             "if test -n \"$unit_fragment\" && "
             "test \"$(readlink -f -- \"$unit_fragment\")\" = "
             "\"$unit_path\"; then "
             "printf 'Installed unit link now resolves to %s\\n' "
             "\"$unit_path\"; else "
             "printf 'The installed unit does not resolve to %s after "
             "daemon-reload.\\n' \"$unit_path\" >&2; exit 4; fi")
      .arg(posixShellQuote(QString::fromLatin1(kServiceUnit)),
           QString::fromLatin1(kServiceName), sudoCommand,
           posixShellQuote(privilegedScript));
}

QString prepareModelCommand(const DiagnosticRuntimeOptions &options) {
  const auto error = validatePreparationOptions(options);
  if (!error.isEmpty()) return "printf '%s\\n' " + posixShellQuote(error) + " >&2; exit 2";
  QString command =
      QStringLiteral("bash ./prepare_model.sh %1 --model-dir %2 --variant %3")
          .arg(posixShellQuote(options.modelName),
               posixShellQuote(QDir::cleanPath(options.modelDirectory)),
               posixShellQuote(options.variant));
  command += " --jetson-only --prepare-only --output-dir " + posixShellQuote(QDir::cleanPath(options.bundleDirectory));
  command += " --python " + posixShellQuote(options.pythonExecutable);
  command += " --max-stream-samples 0 --ecg3-source ecg2_minus_ecg1";
  for (const auto &input : options.validationInputs)
    command += " --validation-input " + posixShellQuote(QDir::cleanPath(input));
  if (options.forceRecoveryOff) command += QStringLiteral(" --no-recovery");
  if (options.validationInputs.isEmpty() && options.autoValidationInputs) {
    command = QString::fromUtf8(R"SH(
validation_args=()
for recording in data/live/p12/p12_out.parquet data/live/p13/p13_out.parquet; do
  if test -f "$recording"; then validation_args+=(--validation-input "$recording"); fi
done
if test "${#validation_args[@]}" -eq 0; then
  echo 'No standard parity recordings found. Set existing raw parquet paths under Advanced model installation → Validation recordings.' >&2
  exit 2
fi
)SH") + command + " \"${validation_args[@]}\"";
  }
  return command;
}

QString activatePreparedModelCommand(const DiagnosticRuntimeOptions &options) {
  const auto error = validatePreparationOptions(options);
  if (!error.isEmpty()) return "printf '%s\\n' " + posixShellQuote(error) + " >&2; exit 2";
  const auto model = QDir::cleanPath(options.bundleDirectory) + "/model";
  QString command = "bash run/deploy_model.sh " + posixShellQuote(options.modelName) +
      " --variant " + posixShellQuote(options.variant) + " --model-dir " + posixShellQuote(model) +
      " --config " + posixShellQuote(model + "/train_config.json") +
      " --checkpoint " + posixShellQuote(model + "/best.pth") +
      " --engine " + posixShellQuote(model + "/model.engine") +
      " --validated-report " + posixShellQuote(QDir::cleanPath(options.bundleDirectory) + "/validation_report.json") +
      " --python " + posixShellQuote(options.pythonExecutable);
  if (options.forceRecoveryOff) command += " --no-recovery";
  // Recheck activity at execution time as well as in the GUI. The backend
  // verifies fingerprints and checks the service again before replacing files.
  return idleDetectorGuard() + command;
}

QString finishedModelsCommand() {
  // Discover published artifacts remotely with POSIX tools; parse source JSON
  // locally in Qt. Runtime-only deployments do not require Python.
  return QString::fromUtf8(R"SH(
catalog_root=$(pwd -P)
for family in finished_models/*; do
 for directory in "$family" "$family/recovery" "$family/frozen_recovery" "$family/lopo_recovery" "$family/delayed_100hz"; do
  test -d "$directory" || continue
  resolved_directory=$(readlink -f -- "$directory") || continue
  case "$resolved_directory" in "$catalog_root"/*) ;; *) continue;; esac
  for variant in best tuned; do
    if test "$variant" = best; then config="$directory/train_config.json"; weights="$directory/best.pth"
    else config="$directory/tuning/tuned_config.json"; weights="$directory/best_tuned.pth"; fi
    test -f "$config" && test -f "$weights" || continue
    resolved_config=$(readlink -f -- "$config") || continue
    resolved_weights=$(readlink -f -- "$weights") || continue
    case "$resolved_config" in "$catalog_root"/*) ;; *) continue;; esac
    case "$resolved_weights" in "$catalog_root"/*) ;; *) continue;; esac
    test "$(stat -Lc %s -- "$config")" -le 2097152 || continue
    printf '__JCG_FINISHED_MODEL__='
    printf '%s' "$directory" | base64 -w0
    printf ':%s:' "$variant"
    base64 -w0 -- "$config"
    printf '\n'
  done
 done
done
)SH");
}


QString FinishedModel::predictionDescription() const {
  const auto target = predictionDelayFrames == 1 ? QStringLiteral("Previous-frame primary; 10 ms context")
                                                 : QStringLiteral("Current-frame target");
  QString description = frameHz > 0 ? QString("%1; %2 Hz; window %3").arg(target)
      .arg(frameHz, 0, 'g', 6).arg(windowFrames > 0 ? QString::number(windowFrames / frameHz, 'g', 6) + " s" : "unavailable") : target;
  description += holdoffMs >= 0 ? QString("; holdoff %1 ms").arg(holdoffMs, 0, 'g', 6) : "; holdoff unavailable";
  return description;
}

static bool readPredictionContract(const QJsonObject &config, FinishedModel &model) {
  const auto mode = config.value("prediction_mode");
  if (!mode.isUndefined() && !mode.isString()) return false;
  model.predictionMode = mode.toString("current");
  if (model.predictionMode != "current" && model.predictionMode != "delayed_100hz") return false;
  const int expected = model.predictionMode == "delayed_100hz" ? 1 : 0;
  const auto delay = config.value("prediction_delay_frames");
  if ((!delay.isUndefined() && (!delay.isDouble() || delay.toDouble() != expected)) ||
      (expected && delay.isUndefined())) return false;
  model.predictionDelayFrames = expected;
  const auto size = config.value("frame_size_raw").toDouble();
  const auto hz = config.value("raw_hz").toDouble();
  model.frameHz = size > 0 && hz > 0 ? hz / size : 0;
  model.windowFrames = config.value("window_size").toInt();
  const auto holdoff = config.value("holdoff_frames");
  if (model.frameHz > 0) {
    if (holdoff.isDouble() && holdoff.toDouble() >= 0)
      model.holdoffMs = std::trunc(holdoff.toDouble()) * 1000.0 / model.frameHz;
    else if (holdoff.isNull() || holdoff.isUndefined()) {
      const auto configuredMs = config.value("holdoff_ms");
      const double ms = configuredMs.isUndefined() ? 200 : configuredMs.toDouble(-1);
      if (ms >= 0) model.holdoffMs = std::ceil(ms * model.frameHz / 1000.0) * 1000.0 / model.frameHz;
    }
  }
  return !expected || (hz == 400 && size == 4 && model.windowFrames >= 2 && !model.recoverySupported &&
      !config.value("recovery_only").toBool(false) &&
      (config.value("recovery_threshold").isNull() || config.value("recovery_threshold").isUndefined()));
}

QList<FinishedModel> parseFinishedModels(const QByteArray &output) {
  QList<FinishedModel> result;
  for (const auto &line : output.split('\n')) {
    const QByteArray itemPrefix = "__JCG_FINISHED_MODEL__=";
    if (line.startsWith(itemPrefix)) {
      const auto parts = line.mid(itemPrefix.size()).split(':');
      if (parts.size() != 3) continue;
      const auto config = QJsonDocument::fromJson(QByteArray::fromBase64(parts[2])).object();
      FinishedModel model{config.value("modelname").toString(),
                          QString::fromUtf8(QByteArray::fromBase64(parts[0])),
                          QString::fromLatin1(parts[1]), config.value("recovery_head").toBool(false),
                          recovery::modelKind(config)};
      if (!readPredictionContract(config, model)) continue;
      DiagnosticRuntimeOptions options;
      options.modelName = model.name; options.modelDirectory = model.directory; options.variant = model.variant;
      if (model.directory.startsWith("finished_models/") && validateDiagnosticOptions(options).isEmpty()) result.append(model);
      continue;
    }
    const QByteArray prefix = "__JCG_FINISHED_MODELS__=";
    if (!line.startsWith(prefix)) continue;
    const auto entries = QJsonDocument::fromJson(
        QByteArray::fromBase64(line.mid(prefix.size()))).array();
    for (const auto &entry : entries) {
      const auto object = entry.toObject();
      FinishedModel model{object.value("name").toString(),
                          object.value("directory").toString(),
                          object.value("variant").toString(), object.value("recovery_head").toBool(false),
                          recovery::modelKind(object)};
      if (!readPredictionContract(object, model)) continue;
      DiagnosticRuntimeOptions options;
      options.modelName = model.name;
      options.modelDirectory = model.directory;
      options.variant = model.variant;
      if (model.directory.startsWith("finished_models/") &&
          validateDiagnosticOptions(options).isEmpty()) result.append(model);
    }
  }
  return result;
}

QString prepareStoppedModelCommand(const DiagnosticRuntimeOptions &options) {
  QString command = idleDetectorGuard() + prepareModelCommand(options);
  if (options.variant == "tuned" && validatePreparationOptions(options).isEmpty()) {
    // Isolated preparation normalizes the copied tuned config to train_config.json.
    // Preserve the backend recovery helper's variant-based source lookup without
    // changing that config or its fingerprint. Create only inside the new bundle.
    const auto model = QDir::cleanPath(options.bundleDirectory) + "/model";
    command += " && mkdir -- " + posixShellQuote(model + "/tuning") +
        " && ln -s -- ../train_config.json " + posixShellQuote(model + "/tuning/tuned_config.json");
  }
  return command;
}

QString reexportModelCommand(const DiagnosticRuntimeOptions &options) {
  auto complete = options;
  complete.preparationMode = QStringLiteral("complete");
  return prepareStoppedModelCommand(complete);
}

QString installModelCommand(const DiagnosticRuntimeOptions &options) {
  const auto error = validatePreparationOptions(options);
  if (!error.isEmpty()) return "printf '%s\\n' " + posixShellQuote(error) + " >&2; exit 2";
  // Keep the report and all intermediate artifacts in the new working directory.
  // Each subshell must succeed before the next stage; no service mutation occurs.
  return "printf '__JCG_INSTALL_STAGE__=preparation\\n'\n(\n" +
      prepareStoppedModelCommand(options) + "\n) || exit $?\n" +
      "printf '__JCG_INSTALL_STAGE__=activation\\n'\n(\n" +
      activatePreparedModelCommand(options) + "\n) || exit $?\n" +
      "printf '__JCG_INSTALL_STAGE__=verification\\n'\n(\n" +
      availabilityProbeCommand(options) + "\n) || exit $?\n";
}

QString validateRuntimeCommand(const DiagnosticRuntimeOptions &options) {
  return runtimeBaseCommand(options) +
         QStringLiteral(" --validate-only --check-engine && ") +
         inputRecoveryConfigCheck(
             posixShellQuote(QDir::cleanPath(options.configPath)));
}

QString startDirectRuntimeCommand(const DiagnosticRuntimeOptions &options) {
  QString command =
      QStringLiteral("set -o pipefail; %1; ").arg(stateVariables());
  command +=
      QStringLiteral("mkdir -p -- \"$state_dir\"; chmod 700 -- \"$state_dir\"; "
                     "if test -s \"$pid_file\"; then "
                     "existing_pid=\"$(cat -- \"$pid_file\")\"; "
                     "if test -n \"$existing_pid\" && kill -0 "
                     "\"$existing_pid\" 2>/dev/null; then "
                     "printf 'Direct inference is already running with PID "
                     "%s\\n' \"$existing_pid\" >&2; "
                     "exit 73; fi; rm -f -- \"$pid_file\"; fi; ");

  QString runtime = runtimeBaseCommand(options);
  if (!options.sessionLogPath.isEmpty()) {
    command +=
        QStringLiteral(
            "session_log=%1; mkdir -p -- \"$(dirname -- \"$session_log\")\"; ")
            .arg(posixShellQuote(QDir::cleanPath(options.sessionLogPath)));
    runtime += QStringLiteral(" > >(tee -a -- \"$session_log\") 2> >(tee -a -- "
                              "\"$session_log\" >&2)");
  }

  command +=
      QStringLiteral(
          "cleanup() { rm -f -- \"$pid_file\"; }; "
          "on_signal() { kill -TERM \"$child_pid\" 2>/dev/null || true; }; "
          "trap cleanup EXIT; %1 & child_pid=$!; "
          "printf '%s\\n' \"$child_pid\" > \"$pid_file\"; "
          "trap on_signal HUP INT TERM; "
          "printf 'Direct inference started with PID %s\\n' \"$child_pid\"; "
          "status=0; wait \"$child_pid\" || status=$?; exit \"$status\"")
          .arg(runtime);
  return command;
}

QString stopDirectRuntimeCommand() {
  return QStringLiteral(
             "set -o pipefail; %1; "
             "if ! test -s \"$pid_file\"; then "
             "printf 'No GUI-managed direct inference PID was found.\\n'; exit "
             "3; fi; "
             "pid=\"$(cat -- \"$pid_file\")\"; "
             "case \"$pid\" in ''|*[!0-9]*) "
             "printf 'Invalid direct inference PID file.\\n' >&2; exit 4;; "
             "esac; "
             "if ! kill -0 \"$pid\" 2>/dev/null; then "
             "rm -f -- \"$pid_file\"; printf 'Tracked inference is no longer "
             "running.\\n'; exit 3; fi; "
             "cmdline=\"$(tr '\\0' ' ' < \"/proc/$pid/cmdline\")\"; "
             "case \"$cmdline\" in *native_uart_live_runtime*) ;; *) "
             "printf 'Refusing to stop PID %s because it is not the native "
             "runtime.\\n' \"$pid\" >&2; "
             "exit 4;; esac; "
             "kill -TERM \"$pid\"; printf 'Sent SIGTERM to direct inference "
             "PID %s.\\n' \"$pid\"")
      .arg(stateVariables());
}

QString quickAvailabilityCheck(const DiagnosticRuntimeOptions & /* options */) {
  const QString service = QString::fromLatin1(kServiceName);
  const QString activeManifest = QString::fromLatin1(kActiveManifest);
  const QString activeEngine = QString::fromLatin1(kActiveEngine);
  return QStringLiteral(
      "printf 'SSH connection successful\\n'; "
      "service_state=\"$(systemctl is-active %1.service 2>/dev/null || true)\"; "
      "printf '__SERVICE_STATE__=%s\\n' \"$service_state\"; "
      "if [ \"$service_state\" = \"active\" ]; then "
      "  printf '__SERVICE_ACTIVE__=%s\\n' \"yes\"; "
      "else "
      "  printf '__SERVICE_ACTIVE__=%s\\n' \"no\"; "
      "fi; "
      "if test -r %2 && test -r %3; then "
      "  active_model=\"$(sed -n 's/^model_name=//p' %2 | tail -n 1)\"; "
      "  active_variant=\"$(sed -n 's/^model_variant=//p' %2 | tail -n 1)\"; "
      "  printf '__MODEL_READY__=%s\\n' \"yes\"; "
      "  printf '__ACTIVE_MODEL__=%s\\n' \"$active_model\"; "
      "  printf '__ACTIVE_VARIANT__=%s\\n' \"$active_variant\"; "
      "else "
      "  printf '__MODEL_READY__=%s\\n' \"no\"; "
      "fi")
      .arg(service, posixShellQuote(activeManifest), posixShellQuote(activeEngine));
}

QString availabilityProbeCommand(const DiagnosticRuntimeOptions &options) {
  const QString config = posixShellQuote(QDir::cleanPath(options.configPath));
  const QString modelName = posixShellQuote(options.modelName);
  const QString modelDirectory =
      posixShellQuote(QDir::cleanPath(options.modelDirectory));
  const QString variant = posixShellQuote(options.variant);
  const QString directValidation = validateRuntimeCommand(options);
  const QString preparationInputs = preparationInputCheck(options);
  auto complete = options;
  complete.preparationMode = QStringLiteral("complete");
  const QString service = QString::fromLatin1(kServiceName);
  const QString runtime = QString::fromLatin1(kRuntimeExecutable);
  const QString serviceUnit = QString::fromLatin1(kServiceUnit);
  const QString serviceConfig = QString::fromLatin1(kServiceConfig);
  const QString activeManifest = QString::fromLatin1(kActiveManifest);
  const QString activeEngine = QString::fromLatin1(kActiveEngine);

  QString command = QString::fromUtf8(R"SH(
declare -A gui_revision_before
for entry in MANIFEST:deploy/model/model.conf CONFIG:config/native_uart_live.conf RUNTIME:build_native/inference/native_uart_live_runtime; do
  key=${entry%%:*}; path=${entry#*:}
  if test -r "$path"; then gui_revision_before[$key]=$(sha256sum -- "$path" | cut -d ' ' -f1); else gui_revision_before[$key]=missing; fi
done
printf 'SSH connection successful\n';
)SH");
  command +=
      QStringLiteral("repo_root=\"$(pwd -P)\"; %1; ").arg(stateVariables());
  command += serviceSchedulingCheck();
  command += QStringLiteral(
      "service_installed=no; service_active=no; service_enabled=no; "
      "service_unit_present=no; service_linked=no; service_contract=no; "
      "service_validation=no; "
      "runtime_present=no; service_config_present=no; config_present=no; "
      "manifest_present=no; engine_present=no; slot_metadata=no; "
      "active_selection=no; direct_validation=no; prepare_present=no; "
      "prepare_inputs=no; rebuild_present=no; direct_active=no; "
      "service_state=unknown; service_input=unknown; ");
  command +=
      QStringLiteral(
          "if test -f %2 && test -r %2; then "
          "service_unit_present=yes; fi; "
          "if systemctl cat %1.service >/dev/null 2>&1; then "
          "service_installed=yes; fi; "
          "service_state=\"$(systemctl is-active %1.service 2>/dev/null || "
          "true)\"; "
          "service_status=\"$(systemctl show --property=StatusText --value "
          "%1.service 2>/dev/null || true)\"; "
          "case \"$service_status\" in input=paused*) service_input=paused;; "
          "input=running*) service_input=running;; esac; "
          "case \"$service_state\" in "
          "active|activating|reloading|deactivating) service_active=yes;; "
          "esac; "
          "if systemctl is-enabled --quiet %1.service; then "
          "service_enabled=yes; fi; "
          "unit_fragment=\"$(systemctl show --property=FragmentPath --value "
          "%1.service 2>/dev/null)\"; "
          "if test -n \"$unit_fragment\" && test -r %2 && "
          "test \"$(readlink -f -- \"$unit_fragment\")\" = "
          "\"$(readlink -f -- %2)\"; then service_linked=yes; fi; ")
          .arg(service, posixShellQuote(serviceUnit));
  command +=
      QStringLiteral(
          "capture_path=\"$(sed -n "
          "'s/^[[:space:]]*capture_path[[:space:]]*=[[:space:]]*//p' %1 | "
          "tail -n 1)\"; "
          "if test \"$(systemctl show --property=NeedDaemonReload --value "
          "%2.service 2>/dev/null)\" = no && "
          "grep -Fqx -- \"BindReadOnlyPaths=${repo_root}:/opt/dkfz-live\" "
          "%3 && "
          "grep -Fqx -- 'WorkingDirectory=/opt/dkfz-live' %3 && "
          "test \"$scheduling_profile\" != unknown && "
          "grep -Fqx -- 'ReadWritePaths=/var/lib/dkfz-live' %3 && "
          "grep -Eq -- '^[[:space:]]*manifest_path[[:space:]]*="
          "[[:space:]]*deploy/model/model\\.conf[[:space:]]*$' %1 && "
          "%4 && "
          "test -d /var/lib/dkfz-live && "
          "id dkfz-live >/dev/null 2>&1 && "
          "getent group dkfz-live >/dev/null 2>&1 && "
          "getent group dialout >/dev/null 2>&1 && "
          "getent group gpio >/dev/null 2>&1 && "
          "{ test -z \"$capture_path\" || case \"$capture_path\" in "
          "/var/lib/dkfz-live/*) true;; *) false;; esac; }; then "
          "service_contract=yes; fi; ")
          .arg(posixShellQuote(serviceConfig), service,
               posixShellQuote(serviceUnit),
               inputRecoveryConfigCheck(posixShellQuote(serviceConfig)));
  command +=
      QStringLiteral("if test -x %1; then runtime_present=yes; fi; "
                     "if test -r %2; then service_config_present=yes; fi; "
                     "if test -r %3; then config_present=yes; fi; "
                     "if test -r %4; then manifest_present=yes; fi; "
                     "if test -r %5; then engine_present=yes; fi; ")
          .arg(posixShellQuote(runtime), posixShellQuote(serviceConfig), config,
               posixShellQuote(activeManifest), posixShellQuote(activeEngine));
  command +=
      QStringLiteral(
          "active_model=; active_variant=; active_source=; "
          "if test \"$manifest_present\" = yes; then "
          "active_model=\"$(sed -n 's/^model_name=//p' %1 | tail -n 1)\"; "
          "active_variant=\"$(sed -n 's/^model_variant=//p' %1 | tail -n "
          "1)\"; "
          "active_source=\"$(sed -n 's/^source_model_dir=//p' %1 | tail -n "
          "1)\"; fi; "
          "if test -n \"$active_model\" && test -n \"$active_source\"; then "
          "case \"$active_variant\" in best|tuned) slot_metadata=yes;; esac; "
          "fi; "
          "if test \"$active_model\" = %2 && "
          "test \"$active_variant\" = %3 && "
          "{ test \"$active_source\" = %4 || test \"$active_source\" = %5 || "
          "test \"$active_source\" = \"${repo_root}/\"%5; }; then active_selection=yes; fi; ")
          .arg(posixShellQuote(activeManifest), modelName, variant,
               modelDirectory, posixShellQuote(QDir::cleanPath(options.bundleDirectory) + "/model"));
  command += QStringLiteral(
                 "if test -s \"$pid_file\" && "
                 "pid=\"$(cat -- \"$pid_file\")\" && "
                 "kill -0 \"$pid\" 2>/dev/null; then direct_active=yes; fi; "
                 "if test \"$service_active\" = no && "
                 "test \"$direct_active\" = no && %1 >/dev/null 2>&1; then "
                 "direct_validation=yes; fi; ")
                 .arg(directValidation);
  command +=
      QStringLiteral(
          "if test \"$service_installed\" = yes && "
          "test \"$service_linked\" = yes && "
          "test \"$service_contract\" = yes && "
          "test \"$runtime_present\" = yes && "
          "test \"$service_config_present\" = yes && "
          "test \"$manifest_present\" = yes && "
          "test \"$engine_present\" = yes && "
          "test \"$slot_metadata\" = yes; then "
          "if test \"$service_active\" = yes; then "
          "%1 --config %2 --validate-only >/dev/null 2>&1 && "
          "service_validation=yes; else "
          "%1 --config %2 --validate-only --check-engine >/dev/null 2>&1 && "
          "service_validation=yes; fi; fi; ")
          .arg(posixShellQuote(runtime), posixShellQuote(serviceConfig));
  command +=
      QStringLiteral(
          "if test -x ./prepare_model.sh && test -x run/deploy_model.sh; then "
          "prepare_present=yes; fi; "
          "if %1; then prepare_inputs=yes; fi; "
          "if test -x run/rebuild_native.sh && "
          "test -x run/build_native.sh; then rebuild_present=yes; fi; "
          "service_ready=no; "
          "if test \"$service_installed\" = yes && "
          "test \"$service_linked\" = yes && "
          "test \"$service_contract\" = yes && "
          "test \"$service_validation\" = yes; then service_ready=yes; fi; ")
          .arg(preparationInputs);
  command += QStringLiteral(
      "printf '__JCG_SERVICE__=%s\\n' \"$service_installed\"; "
      "printf '__JCG_SERVICE_ACTIVE__=%s\\n' \"$service_active\"; "
      "printf '__JCG_SERVICE_STATE__=%s\\n' \"$service_state\"; "
      "printf '__JCG_SCHEDULING_PROFILE__=%s\\n' \"$scheduling_profile\"; "
      "printf '__JCG_SERVICE_INPUT__=%s\\n' \"$service_input\"; "
      "printf '__JCG_SERVICE_ENABLED__=%s\\n' \"$service_enabled\"; "
      "printf '__JCG_SERVICE_UNIT__=%s\\n' \"$service_unit_present\"; "
      "printf '__JCG_SERVICE_LINKED__=%s\\n' \"$service_linked\"; "
      "printf '__JCG_SERVICE_CONTRACT__=%s\\n' \"$service_contract\"; "
      "printf '__JCG_SERVICE_READY__=%s\\n' \"$service_ready\"; "
      "printf '__JCG_RUNTIME__=%s\\n' \"$runtime_present\"; "
      "printf '__JCG_SERVICE_CONFIG__=%s\\n' "
      "\"$service_config_present\"; "
      "printf '__JCG_CONFIG__=%s\\n' \"$config_present\"; "
      "printf '__JCG_MANIFEST__=%s\\n' \"$manifest_present\"; "
      "printf '__JCG_ENGINE__=%s\\n' \"$engine_present\"; "
      "printf '__JCG_SLOT_METADATA__=%s\\n' \"$slot_metadata\"; "
      "printf '__JCG_ACTIVE_SELECTION__=%s\\n' \"$active_selection\"; "
      "printf '__JCG_ACTIVE_MODEL__=%s\\n' \"$active_model\"; "
      "printf '__JCG_ACTIVE_VARIANT__=%s\\n' \"$active_variant\"; "
      "printf '__JCG_ACTIVE_SOURCE__=%s\\n' \"$active_source\"; "
      "printf '__JCG_DIRECT_READY__=%s\\n' \"$direct_validation\"; "
      "printf '__JCG_PREPARE__=%s\\n' \"$prepare_present\"; "
      "printf '__JCG_PREPARE_INPUTS__=%s\\n' \"$prepare_inputs\"; "
      "printf '__JCG_REBUILD__=%s\\n' \"$rebuild_present\"; "
      "printf '__JCG_DIRECT_ACTIVE__=%s\\n' \"$direct_active\"");
  command += QStringLiteral("; if %1; then printf '__JCG_REEXPORT_INPUTS__=yes\\n'; "
                            "else printf '__JCG_REEXPORT_INPUTS__=no\\n'; fi; ")
                 .arg(preparationInputCheck(complete));
  command += finishedModelsCommand();
  // Pin the active deployment for a subsequent explicit service start.
  // This reads files only; it never probes an installer or transfers binaries.
  command += QString::fromUtf8(R"SH(
for entry in MANIFEST:deploy/model/model.conf CONFIG:config/native_uart_live.conf RUNTIME:build_native/inference/native_uart_live_runtime; do
  key=${entry%%:*}; path=${entry#*:}
  if test -r "$path"; then revision=$(sha256sum -- "$path" | cut -d ' ' -f1); else revision=missing; fi
  test "$revision" = "${gui_revision_before[$key]}" || { echo 'Deployment changed during validation. Check again.' >&2; exit 4; }
  printf '__JCG_DEPLOY_%s_REVISION__=' "$key"
  if test "$revision" != missing; then printf '%s\n' "$revision"; else printf '\n'; fi
done
)SH");
  return command;
}

QString rebuildRuntimeCommand() {
  return QStringLiteral(
      "if test -f build_native/CMakeCache.txt; then "
      "bash run/rebuild_native.sh --target native_uart_live_runtime; "
      "else bash run/build_native.sh --tensorrt on --test; fi");
}

namespace {
QString deploymentServiceGuard() {
  return QString::fromUtf8(R"SH(
set -euo pipefail
service=dkfz-native-uart-live.service
root=$(pwd -P)
test "$(systemctl show --value -p NeedDaemonReload "$service")" = no
test "$(readlink -f -- "$(systemctl show --value -p FragmentPath "$service")")" = "$root/deploy/dkfz-native-uart-live.service"
test "$(systemctl show --value -p WorkingDirectory "$service")" = /opt/dkfz-live
test -z "$(systemctl show --value -p RootDirectory "$service")"
binds=$(systemctl show --value -p BindReadOnlyPaths "$service")
case "$binds" in "$root:/opt/dkfz-live"|"$root:/opt/dkfz-live:rbind"|"$root:/opt/dkfz-live:norbind") ;; *) echo 'Service project changed. Check again.' >&2; exit 1;; esac
if test -s "$1" && kill -0 "$(cat -- "$1")" 2>/dev/null; then
  echo 'Stop direct inference before deploying a model.' >&2; exit 1
fi
)SH") + serviceSchedulingCheck() + "test \"$scheduling_profile\" != unknown\n";
}
QString guardedServiceCommand(bool password,const QString &script) {
  const auto sudo = password ? "sudo -S -p '' -- bash -c " : "sudo -n -- bash -c ";
  // Resolve the direct session path as the SSH user before entering sudo.
  return stateVariables()+"; flock -x .jetson-control-deployment.lock "+sudo+
      posixShellQuote(script)+" bash \"$pid_file\"";
}
}
QString deploymentStopCommand(bool password) {
  return guardedServiceCommand(password,deploymentServiceGuard()+
      "state=$(systemctl is-active \"$service\" || true)\n"
      "case \"$state\" in active|inactive|failed) ;; *) echo 'Wait for the service to finish changing state.' >&2; exit 1;; esac\n"
      "systemctl stop \"$service\"\n");
}
QString deploymentStartCommand(bool password,const QString &manifest,const QString &config,const QString &runtime) {
  const QRegularExpression sha("^[0-9a-f]{64}$");
  for (const auto &value : {manifest,config,runtime})
    if (!sha.match(value).hasMatch()) return "echo 'Deployment revision unavailable; Check again.' >&2; exit 1";
  auto script = deploymentServiceGuard();
  const QMap<QString,QString> files{{"deploy/model/model.conf",manifest},{"config/native_uart_live.conf",config},{"build_native/inference/native_uart_live_runtime",runtime}};
  for(auto it=files.begin();it!=files.end();++it)
    script += "test \"$(sha256sum -- "+posixShellQuote(it.key())+" | cut -d ' ' -f1)\" = "+posixShellQuote(it.value())+" || { echo 'Deployment changed after validation; Check again.' >&2; exit 1; }\n";
  script += "# DKFZ_DEPLOY_START\nsystemctl start \"$service\"\nsystemctl is-active --quiet \"$service\"\n";
  return guardedServiceCommand(password,script);
}
