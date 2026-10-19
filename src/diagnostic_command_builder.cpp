// Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Developed at the German Cancer Research Center (DKFZ).

#include "diagnostic_command_builder.h"

#include "ssh_command_builder.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>

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
  if (options.preparationMode == QStringLiteral("jetson-only")) {
    const QString onnx =
        modelDirectory +
        (tuned ? QStringLiteral("%1_best_tuned.onnx").arg(options.modelName)
               : QStringLiteral("%1.onnx").arg(options.modelName));
    check += QStringLiteral(" && test -f %1").arg(posixShellQuote(onnx));
  }
  return check;
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
  QString command =
      QStringLiteral("./prepare_model.sh %1 --model-dir %2 --variant %3")
          .arg(posixShellQuote(options.modelName),
               posixShellQuote(QDir::cleanPath(options.modelDirectory)),
               posixShellQuote(options.variant));
  if (options.preparationMode == QStringLiteral("jetson-only")) {
    command += QStringLiteral(" --jetson-only");
  }
  return command;
}

QString finishedModelsCommand() {
  // Read the declared model name, which need not match its directory name.
  // Encode the catalog so paths and JSON cannot collide with probe markers.
  const QString script = QString::fromUtf8(R"PY(
import base64, json, pathlib, re, sys
models = []
root = pathlib.Path('finished_models')
for directory in sorted(root.iterdir()) if root.is_dir() else []:
    if not directory.is_dir():
        continue
    for variant, config, weights in [('best', 'train_config.json', 'best.pth'),
                                     ('tuned', 'tuning/tuned_config.json', 'best_tuned.pth')]:
        if not (directory / config).is_file() or not (directory / weights).is_file():
            continue
        try:
            name = json.loads((directory / config).read_text(encoding='utf-8')).get('modelname', '')
            if not isinstance(name, str) or not re.fullmatch(r'[A-Za-z0-9._-]+', name):
                raise ValueError('invalid modelname')
            models.append(dict(name=name, directory=directory.as_posix(), variant=variant))
        except (OSError, ValueError, AttributeError) as error:
            print(f'Skipping {directory / config}: {error}', file=sys.stderr)
print('__JCG_FINISHED_MODELS__=' + base64.b64encode(json.dumps(models).encode()).decode())
)PY");
  return QStringLiteral("python3 -c %1").arg(posixShellQuote(script));
}

QList<FinishedModel> parseFinishedModels(const QByteArray &output) {
  QList<FinishedModel> result;
  for (const auto &line : output.split('\n')) {
    const QByteArray prefix = "__JCG_FINISHED_MODELS__=";
    if (!line.startsWith(prefix)) continue;
    const auto entries = QJsonDocument::fromJson(
        QByteArray::fromBase64(line.mid(prefix.size()))).array();
    for (const auto &entry : entries) {
      const auto object = entry.toObject();
      FinishedModel model{object.value("name").toString(),
                          object.value("directory").toString(),
                          object.value("variant").toString()};
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

QString reexportModelCommand(const DiagnosticRuntimeOptions &options) {
  auto complete = options;
  complete.preparationMode = QStringLiteral("complete");
  return QStringLiteral(
             "service_state=\"$(systemctl is-active %1.service 2>/dev/null || true)\"; "
             "case \"$service_state\" in active|activating|reloading|deactivating) "
             "printf 'Stop the service before re-exporting a model.\\n' >&2; exit 3;; esac; "
             "%2; if test -s \"$pid_file\" && "
             "kill -0 \"$(cat -- \"$pid_file\")\" 2>/dev/null; then "
             "printf 'Stop direct inference before re-exporting a model.\\n' >&2; exit 3; fi; "
             "%3")
      .arg(QString::fromLatin1(kServiceName), stateVariables(), prepareModelCommand(complete));
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

  QString command = QStringLiteral("printf 'SSH connection successful\\n'; ");
  command +=
      QStringLiteral("repo_root=\"$(pwd -P)\"; %1; ").arg(stateVariables());
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
          "grep -Fqx -- 'ExecStart=/opt/dkfz-live/build_native/"
          "inference/native_uart_live_runtime --config "
          "/opt/dkfz-live/config/native_uart_live.conf' %3 && "
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
          "test \"$active_source\" = %4; then active_selection=yes; fi; ")
          .arg(posixShellQuote(activeManifest), modelName, variant,
               modelDirectory);
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
  return command;
}

QString rebuildRuntimeCommand() {
  return QStringLiteral(
      "if test -f build_native/CMakeCache.txt; then "
      "bash run/rebuild_native.sh --target native_uart_live_runtime; "
      "else bash run/build_native.sh --tensorrt on --test; fi");
}
