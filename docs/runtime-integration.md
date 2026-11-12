# Runtime integration and remaining validation

The GUI targets the isolated preparation/report contract in live_stack commit
`d290c82d1a6c2dac80a1397b3f27e96b49e235e5` (reviewed 2026-11-11).
The runtime checkout was read only during development.

## Models and coordinates

Current-frame 50 Hz models and two-output recovery models remain supported.
Separately trained delayed models are discovered under
`finished_models/<family>/delayed_100hz/`; their declared model identity is used,
not a name inferred from the folder. Four samples at 400 Hz produce 100 frames/s.
The latest recurrent state predicts the previous frame with 10 ms context.
Window seconds and holdoff milliseconds use the selected/deployed frame rate.
An explicit `holdoff_frames` takes precedence; catalog metadata otherwise uses
the backend's ceiling of `holdoff_ms` (default 200 ms) to whole frames. Missing
deployed holdoff/window fields remain unavailable rather than displaying zero.
Native validation remains authoritative for preprocessing/model compatibility.

The runtime retains historical JSON keys ending in `_50hz` for frame indices
even at 100 Hz. They are indices, not a conversion instruction. Trigger summaries
separate candidate peak coordinates from actual emission coordinates. Original
records, decision timestamps and GPIO-assertion timestamps remain intact. The
GUI never subtracts prediction delay from emission time or infers head origin
from a trigger's probability alone. A null timestamp stays unavailable.

## Deployment contract

The normal action is **Install selected model**: prepare, activate and verify
service readiness as one command sequence under the GUI deployment lock. The
service must already be idle and remains stopped. Success needs no additional
Check before Start (the existing hardware confirmation still applies). A failed
stage prevents subsequent stages; readiness failure after activation is reported
as unconfirmed rather than claiming the service is ready.

The GUI names a fresh working directory under `build_native/prepared_models/`
using model identity, UTC time and a UUID. Preparation uses
`--jetson-only --prepare-only`, repeated `--validation-input`, full recordings via
`--max-stream-samples 0`, and confirmed `--ecg3-source ecg2_minus_ecg1`.
Both rates retain the confirmation and recording checks. Empty advanced recording
settings automatically use existing standard raw fixtures at
`data/live/p12/p12_out.parquet` and `data/live/p13/p13_out.parquet`. At least one
must exist; there is no silent omission of stream validation. If the host uses a
different layout, configure explicit paths in the collapsed advanced section.
Overrides apply in the current session. Paths are relative to the remote checkout;
shell quoting preserves spaces and apostrophes. The backend validates file
contents and refuses an existing/deploy output tree. No fallback to the old
implicit prepare/activate command occurs.

Preparation checks numerical and causal-stream equivalence and inference-only
timing. It does not establish accuracy, sustained performance, physical UART
transport or GPIO timing. The GUI does not interpret a pass as those results.
Inspect event counts: probability coverage without emitted beats does not prove
positive-event coverage. Accuracy review must use matching annotations and the
appropriate metric schema; no training, tuning or annotation editing occurs here.

Activation runs automatically after successful preparation during installation.
It passes the exact config, checkpoint, engine and `--validated-report` to the
backend, with the selected variant and recovery policy. Fingerprint verification
belongs to the backend and is repeated before replacement. Manual Prepare and
Activate controls remain in the advanced section for diagnostics; editing inputs
clears manual activation eligibility. Neither flow claims accuracy certification,
runs paced qualification or measures hardware.
The GUI keeps successful and failed working directories for evidence/source
readback; cleanup is manual. It never rewrites source weights or annotations.

**Reuse saved engine** discovers prior bundles under
`build_native/prepared_models/`, independent of desktop session state. Discovery
uses shell/coreutils and local Qt JSON parsing, requires the report/config/weights/
engine to remain within the bundle, and excludes failed, incomplete or unsupported
reports. Report eligibility includes successful stream parity, source unchanged,
the ECG2-minus-ECG1 confirmation and config/checkpoint/engine SHA256 fingerprints.
Entries are filtered locally by selected model identity, best/tuned variant and
startup recovery policy. Check, Refresh models and Refresh saved engines load
the catalog; editing selections sends no SSH commands.

Reuse takes the shared deployment lock, checks idle service/direct state, verifies
the exact report revision read during discovery and compares the published config
and weights to the bundle's normalized copies. It invokes the existing backend
`check_prepared_model.py` and `deploy_model.sh --engine --validated-report` path,
then performs the normal readiness probe. It does not export ONNX or build an
engine; backend fingerprint, parity and native engine validation still run.
Fresh validation recordings and a new ECG-source checkbox are unnecessary because
the pinned report records that evidence. A changed source, changed report,
invalid engine, recovery-policy mismatch or incompatible target fails explicitly;
there is no automatic rebuild or service start. Install selected model remains
the explicit full conversion path. No backend files or helper packages are added.

Preparation and activation recheck idle service/direct-process state at execution
time and share the GUI deployment lock with policy writes. They never stop or
start the service. The backend additionally checks for concurrent GPU work.
External tools must honor the runtime's guards/locks; the GUI cannot serialize
uncooperative external writers. A cancelled SSH command can leave remote work or
partial evidence; check the host before retrying. Each installation chooses a new directory.
Service Start and Restart pin manifest/config/runtime revisions and verify the
current service mapping under the same deployment lock. Direct Start takes that
lock while checking service inactivity and publishing its PID, then releases it
for the runtime session. A second GUI launch checks the published live PID.

Every Check button and Enter runs the complete readiness/model probe. The former
quick/full split is removed. After reopening the GUI, the probe can match a
deployed bundle to its published source by comparing the exact configuration and
checkpoint bytes. It requires both files to remain within the checkout, handles
best/tuned source layouts, and rejects changed artifacts. Model names alone do
not establish this relationship.

Live and custom-command stdout/stderr capture retains a 64 KiB tail per channel;
partial lines are displayed in bounded chunks. The console retains its existing
block limit and remote direct-session logs remain complete. Finite model/probe
responses remain intact for parsing.

Prepared tuned weights retain `model_variant=tuned`, but the bundle normalizes
source copies to `model/train_config.json` and `model/best.pth`. The current
backend recovery metadata helper expects `tuning/tuned_config.json` for a tuned
source. After successful tuned preparation, the GUI adds a relative link at that
path to `../train_config.json` inside the new bundle. This preserves source
readback without changing the canonical config, report fingerprints or weights.
Failure to create this link leaves preparation unconfirmed. Existing deployments
keep their original source layout. Live restoration still needs matching native
API readback and its normal source eligibility checks.

## Scheduling and timing

Preflight supports the original unit and the exact bounded RR/20 worker profile.
For the bounded profile it reads systemd's loaded hard/soft RT limits, normal
main-process policy, CPU weight and timer slack. Unknown flags, unlimited RT CPU
time, process-wide RT profiles and runtimes without the required CLI flags are
rejected. CLI capability comes from read-only `--help`. This checks the configured
profile, not actual workers. It neither raises SSH limits nor installs a unit.

Actual `worker_scheduling` records show thread, policy, priority, applied/degraded
status and errors. Missing records never imply success. Follow log includes
journal history and does not bind records to a current invocation, so the GUI
labels them as last observed and current-invocation unconfirmed. It does not
declare that all current workers are real-time. Runtime timing shows missed
deadlines and maximum host-decode-to-completion delay, including with probability
records disabled. This anchor is not a physical acquisition edge. RR scheduling
and a 200 ms RT CPU-use limit do not bound worst-case frame delay.

Required qualification remains: target TensorRT build and parity with real
recordings; model-specific accuracy review; sustained 50/100 Hz processing under
the requested scheduling profile, load and thermal conditions; current service
worker readback; physical input/output timing. Windows packaging and interactive
desktop layout have not been verified in this development environment.

## Offline checks

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 4
ctest --test-dir build --output-on-failure -j 4
```

Qt tests use temporary fake SSH/systemd/runtime endpoints. They cover model
discovery and identity, 50/100 Hz physical timing, preparation arguments,
automatic directories and standard/override recordings, ECG confirmation,
combined install with separate service start, fingerprinted activation paths,
best/tuned/recovery policies, rejected preparation/reports, failed readiness,
stale activity and reinstallation,
saved-engine discovery, best/tuned/recovery-policy filtering, reuse at both frame
rates, source/report changes, rejected engines, idle guards and readiness failures,
worker scheduling/degradation, health delays and unchanged emission coordinates.
They do not execute target preparation/activation, use real weights, or start or
stop a real service. Existing policy/SSH/configuration tests remain part of CTest.

An additional optional check exercises the actual runtime report-validation CLI
against synthetic temporary bytes (not valid weights or TensorRT engines):

```bash
/home/fedora/work/live_stack/.venv/bin/python tests/check_runtime_validation_report.py /home/fedora/work/live_stack
```

This passed 20 cases for current/delayed reports: matching evidence and rejection
of altered config/engine/checkpoint fingerprints, model identity, variant,
recovery policy, failed validation, changed source, incorrect delayed ECG source
and absent delayed stream evidence. It runs only `check_prepared_model.py`, never
the deployment or preparation scripts. All 10 CTest targets passed locally.
