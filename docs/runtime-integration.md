<!--
Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
Developed at the German Cancer Research Center (DKFZ).
-->

# Runtime integration and validation

[README](../README.md) · [User guide and troubleshooting](user-guide.md)

These notes describe the GUI's contract with the existing Jetson `live_stack`
checkout. The user-facing tabs are **Run**, **Detector settings**, and **Advanced**.
Backend capabilities and deployment revisions are read from the selected remote
checkout; a folder name or a successful SSH connection alone does not establish
runtime compatibility. This GUI does not install backend helper packages.

## Remote interface

| Operation | Existing remote interface |
| --- | --- |
| Transport | System OpenSSH client, `BatchMode=yes`, no PTY, remote Bash under the configured checkout. |
| New preparation | `prepare_model.sh --jetson-only --prepare-only --output-dir ...`. |
| Report verification | `run/check_prepared_model.py` with report/config/checkpoint/engine, variant and recovery policy. |
| Activation/reuse | `run/deploy_model.sh --engine ... --validated-report ...`, exact source files and selected variant. |
| Native validation/test | `build_native/inference/native_uart_live_runtime`. |
| Installed detector | `dkfz-native-uart-live.service`, backed by `deploy/dkfz-native-uart-live.service`. |
| Live recovery | `build_native/inference/native_live_recovery_control`, schema `dkfz-live-recovery-control-v1`, API version 1. |
| Live primary | Checked atomic manifest/fallback writes and native journal readback. |
| Logs | `journalctl --namespace=dkfz-live`; preparation logs remain in the bundle. |

Model and bundle discovery use remote shell/coreutils and local Qt JSON parsing.
They do not require a local backend checkout or remote Python. Preparation and
activation use the selected Jetson Python environment and the backend's existing
export/parity dependencies. Advanced runtime rebuilds compile on the target;
no desktop-built runtime binary is transferred.

## Preparation, activation and reuse

**Build and install new engine** performs preparation, activation and a final
readiness probe under `.jetson-control-deployment.lock`. The detector must already
be idle and remains stopped. A failed stage prevents subsequent stages. Successful
combined installation needs no extra Check before a separately requested
**Start detector**; the hardware confirmation still applies. Failure after
activation leaves readiness unconfirmed.

New bundles have unique paths:

```text
build_native/prepared_models/gui-<model>-<UTC-time>-<uuid>/
  model/train_config.json
  model/best.pth
  model/model.engine
  model/model.conf
  validation_report.json
  logs/
```

Preparation always exports from the saved checkpoint and matching training
configuration, even if an ONNX file already exists. The GUI passes
`--max-stream-samples 0`, `--ecg3-source ecg2_minus_ecg1`, and repeated
`--validation-input` arguments. Empty recording settings select existing
`data/live/p12/p12_out.parquet` and `data/live/p13/p13_out.parquet`; at least one
must exist. Overrides are project-relative paths. Both frame rates require the
new-build ECG confirmation and recordings. The GUI never falls back to the old
implicit prepare-and-activate workflow.

The optional **Build files only** / **Install files built this session** controls
split preparation and activation. Editing model/build settings clears that
manual activation eligibility. A manual activation requires Check before Start.

**Use saved engine** discovers retained bundles independently of desktop session
state. Discovery requires the report, copied config, checkpoint and engine to be
readable inside the bundle. Eligibility checks include:

- schema `dkfz-prepared-model-v1`, successful validation and unchanged source;
- explicit best/tuned variant and recovery-policy metadata;
- recorded ECG2-minus-ECG1 confirmation and successful nonempty stream validation;
- SHA256 fingerprints for configuration, checkpoint and engine.

The UI filters candidates by model name, variant and startup recovery policy.
On reuse it checks idle state, canonical path containment, the report revision
read during discovery, and byte equality between the selected source files and
the bundle's copies. The existing backend report validator and activation path
then recheck fingerprints, parity and engine validity. Reuse skips export and
engine generation; it does not skip backend validation. A failed check never
triggers an automatic rebuild or service start. Original recording paths and a
new ECG checkbox are unnecessary because reuse pins existing report evidence.

Bundles are retained after both success and failure; cleanup is manual. Keep the
complete bundle for reuse and deployed-source readback. The GUI changes display
labels only: preparation timestamps are parsed from automatic UTC names and
shown in desktop local time without timezone text. Precision/rate come from the
report. Unknown metadata stays unavailable, and tooltips retain the exact path.

For tuned preparation, copied source files are normalized to
`model/train_config.json` and `model/best.pth`, while activation retains
`model_variant=tuned`. The GUI adds the relative
`model/tuning/tuned_config.json -> ../train_config.json` alias expected by the
backend's tuned recovery-source lookup. It does not alter the copied config or
its report fingerprint. Alias-creation failure leaves preparation unconfirmed.

## Service guards and readiness

The readiness probe checks the checkout unit, installed fragment resolution,
`NeedDaemonReload`, the selected checkout bind source, fixed working directory,
service account/group/state-directory presence, runtime/configuration and stable
`deploy/model/` files. Capture paths in the service config must remain under
`/var/lib/dkfz-live`. Required input reset, stuck-signal detection, recovery
hysteresis and nonzero communication deadlines are checked.

For an inactive service, preflight runs `--validate-only --check-engine`. For an
active service it uses configuration-only validation to avoid loading a second
service engine. `StatusText` distinguishes running input from paused input;
paused input can be healthy and is not by itself a reason to restart.

**Start detector** and **Restart detector** verify the loaded service mapping and
manifest/config/runtime SHA256 revisions from the last readiness snapshot under
the deployment lock. Changes require a fresh Check. Advanced **Repair service
setup** links the checkout's existing unit, reloads systemd and verifies the link
while idle. It does not enable/start the unit, provision accounts/groups/state,
or install the namespaced journald policy. Those are host provisioning tasks.
**Reload service definition** runs daemon-reload without restarting the detector.

Temporary test runs use the stable installed slot with explicit output/capture
CLI settings. Direct Start takes the deployment lock while verifying service
inactivity and publishing its PID, then releases it for the session. Other
cooperating starts/writers check that PID; cleanup takes the lock and removes
only its own PID entry. Stop validates the tracked command line before SIGTERM.
The PID state is in the remote SSH user's runtime directory. These guards do not
serialize unrelated users or external tools that ignore the contract.

## Live policy and confirmation

Primary writes preserve source training JSON and all manifest fields except
`manual_threshold`. Restore first writes the calibrated fallback and then clears
that manual selection, preserving early-emission policy. Fallback/timing updates
use the engine identity. Writes check deployment/config/policy revisions and the
service invocation, use the deployment lock, replace files atomically and sync
them. A local `threshold-actions.json` audit under Qt's application-local-data
location records requests/results. Audit or partial-write failures are reported;
file replacement alone is never active confirmation.

Recovery requests use the native API version plus model/engine identity,
expected revision and systemd invocation ID. The runtime uses a separate file
at `threshold_override_path + ".recovery"` (normally
`/var/lib/dkfz-live/threshold_override.conf.recovery`). Disabling retains the
selected numeric threshold; an effective null threshold means off. A service
restart restores the manifest startup policy, while a retained numeric selection
can be used by a future explicit request. Stale files may produce
`recovery_rejected`; the rejected update does not replace the current policy.

Both controllers distinguish saved/requested from confirmed active state. Check,
Enter and explicit Refresh actions read confirmation; there is no periodic
status polling. A paused runtime can defer a request until another completed
frame. Readbacks require matching engine/service identity and fresh diagnostics.
The recovery API supports the installed service, not arbitrary direct processes.
Absent API/readback support leaves live controls unavailable; startup policy can
still be chosen for installation. The GUI never installs an API to enable them.

Source calibration readback uses the deployed `source_model_dir` and variant.
It is a saved snapshot, not proof of runtime application. Existing manifests do
not pin original source-config byte identity. Missing, mismatched, changed-during-read
or newer source files leave restoration unavailable/unconfirmed.

## Output transport

The output pane has a draggable divider and **Expand output** / **Restore
controls** actions. GUI command banners, connection summaries and completion
messages are omitted from that pane; status labels retain feedback. Backend
programs can print their own command lines.

Preparation subprocesses write into retained backend `.log` files. A foreground
supervisor, using the selected Jetson Python interpreter, forwards newly written
log bytes and performs a final drain after the child exits. Child stdout/stderr
remain attached and Python output is unbuffered. The supervisor preserves exit
codes and forwards received cancellation signals to the operation's process
group. No helper file is installed. Terminating local SSH still cannot guarantee
that every remote child has stopped; cancellation leaves results unconfirmed.

Install/reuse exposes runtime validation output during its final readiness check.
Ordinary discovery keeps its machine-readable response private. Installation
captures private `__JCG_*` records separately from displayed logs, including
fragmented records larger than a display chunk. Protocol framing keeps a final
non-newline log fragment separate from the following stage record.

The console is limited to 20,000 blocks. Live/custom-command stdout/stderr capture
retains a 64 KiB tail per channel, and long partial display lines are chunked.
Installation retains protocol data without duplicating all its logs in another
buffer. Full build logs and configured direct-session logs remain on the Jetson.
Service journals are transmitted unchanged; the desktop formats structured
records into compact summaries while retaining operational fields and non-JSON
lines. Original decision/emission coordinates are not changed.

## Models and coordinates

The GUI supports current-frame 50 Hz models and two-output recovery models.
Separately trained `delayed_100hz` variants are discovered under
`finished_models/<family>/delayed_100hz/`. Four samples at 400 Hz produce 100
frames/s; the primary output targets the previous frame using 10 ms of subsequent
context. Existing 50 Hz weights cannot acquire this target through a GUI switch.
Single-output delayed models disable recovery controls.

Frame rate determines displayed window seconds and holdoff milliseconds.
`holdoff_frames` takes precedence; catalog metadata otherwise uses the backend's
ceiling of `holdoff_ms` (default 200 ms) to whole frames. Missing deployed fields
remain unavailable. Native validation is authoritative for model compatibility.

Historical JSON keys ending in `_50hz` remain frame indices at 100 Hz. Trigger
summaries separate candidate coordinates from actual emission coordinates.
Delayed probability telemetry identifies the previous-frame primary. For
current/recovery models, `probability` is the current-head score and
`recovery_prob` is the previous-frame score; `recovery_emitted_now` describes that
probability record. A trigger score alone does not establish head origin.
The GUI never subtracts prediction delay from emission times or invents a missing
timestamp.

Recovery models share an LSTM. Joint/frozen/LOPO provenance is read from saved
metadata, not a name suffix. Missing provenance is unknown. Frozen/unchanged
primary-weight declarations, base checkpoint SHA256 and deployed engine identity
are distinct evidence. Recovery-head nested LOPO is conditional on the frozen
base: outer patients may have been seen by the base/preprocessing. The entire
detector is not thereby established as patient-unseen. Fixed selected epochs
are shown only when supplied; training length is not substituted.

Saved calibration audits, FP allowances and early-emission metrics are not live
performance guarantees. Primary/timing/holdoff/recovery edits can invalidate their
applicability. Live disabling cannot undo prior emissions or shared holdoff.
The GUI does not run training, recalibration or annotation editing. Backend
training and target measurement procedures remain in its `docs/recovery-head.md`
and `docs/100hz-delayed-target.md`.

## Scheduling and target qualification

Preflight accepts the original unit profile and the exact bounded worker RR/20
profile. The latter requires matching loaded hard/soft `LimitRTPRIO=20`,
`LimitRTTIME=200000` microseconds, normal main-thread scheduling, `CPUWeight=1000`,
`TimerSlackNSec=1000`, and runtime CLI support for the requested realtime flags.
Unknown profiles/incompatible limits are rejected. These are configuration checks;
they do not promote SSH workers or change host power, clocks or global scheduling.

`worker_scheduling` records report observed policy/priority, requested priority,
applied/degraded state and errors. View live logs includes history, so the GUI
labels observations as current-invocation unconfirmed. Runtime health exposes
`missed_frame_deadlines` and maximum host-UART-decode-to-completion delay, including
when probability output is disabled. That anchor is not physical acquisition.
RR scheduling and a 200 ms realtime CPU-use limit do not bound worst-case frame delay.

Conversion validation checks numerical/causal-stream equivalence and inference-only
timing. It does not establish accuracy, sustained 50/100 Hz operation under load
and thermal conditions, or physical UART/GPIO timing. Positive event coverage,
model-specific accuracy, current worker scheduling and physical timing require
separate target qualification. Linux offscreen layout checks do not validate
Windows packaging or actual Jetson hardware operation.

## Local verification

From the GUI checkout:

```bash
./build.sh --test --jobs 2
```

The ten CTest suites use temporary fake SSH/systemd/runtime endpoints. Coverage
includes model metadata, 50/100 Hz timing, quoting, policy validation/readback,
revision/idle/locking guards, combined build/reuse, rejected reports/engines,
changed source files, stale service/direct state, console capture, streamed logs
on success/failure and partial final lines. They do not run real weights or
TensorRT, operate UART/GPIO, or start/stop a real service. The Linux preparation
stream fixtures require Python 3.

An optional read-only check exercises the actual backend report-validation CLI
with synthetic artifact bytes. Set the path to your local backend checkout:

```bash
runtime=/absolute/path/to/live_stack
"$runtime/.venv/bin/python" tests/check_runtime_validation_report.py "$runtime"
```

It checks 20 current/delayed report cases: matching evidence and rejection of
changed fingerprints, identity, variant, recovery policy, failed validation,
changed source, incorrect delayed ECG source and missing delayed stream evidence.
It invokes `check_prepared_model.py` only, without preparing/activating a model.
The local CTest suites and this report check have passed; target qualification
remains separate. The release workflow performs its Windows installer smoke
check separately from these local Linux checks.
