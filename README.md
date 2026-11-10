<!--
Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
Developed at the German Cancer Research Center (DKFZ).
-->

# CMR Cardiac Trigger User Interface deployed on an NVIDIA Jetson Orin Nano

[![Source Code](https://raw.githubusercontent.com/selimct/DKFZ-MRI-Cardiac-Trigger-GUI/refs/heads/main/packaging/source-badge.svg)](https://github.com/selimct/Cetin-Cardiac-Trigger-SourceCode.git)
[![ECG/POX Data](https://raw.githubusercontent.com/selimct/DKFZ-MRI-Cardiac-Trigger-GUI/refs/heads/main/packaging/data-badge.svg)](https://github.com/selimct/Cetin-UHF-ECGPOX-Data.git)
[![ParquetPlot](https://raw.githubusercontent.com/selimct/ParquetPlot/refs/heads/main/resources/parquetplot-gitlab-badge.svg)](https://github.com/selimct/ParquetPlot)

## Jetson Control GUI

A local Qt 6 desktop application for controlling the existing live stack on the
Jetson over SSH and viewing command output and service logs. Install the normal
GUI on the desktop; no recovery add-on, helper package, Python installation or
GUI-managed software update is required on the Jetson. The desktop needs no
local live-stack checkout. `remote_directory` is the existing remote project root.

For normal use:

1. Open **Connection**, enter the SSH settings, and press **Check** or **Enter**.
2. Select a published model from **Finished models**.
3. Choose **Persistent startup recovery**: **Use saved recovery settings**, or
   **Recovery off — current output only**.
4. Review the existing hardware checkbox and click **Deploy and start**. This
   sends the existing stop → prepare → validate → start commands over SSH.
5. Press **Check**, **Refresh**, or **Enter** when you want updated information.

To persist recovery off and back on for the **same two-output model**, keep that
model selected, change only Startup recovery, and deploy/start it again. Off
forwards `--no-recovery` to preparation. Saved settings omit that flag and restore
the source policy. Both keep `recovery_head=true`; no retraining or new checkpoint
is needed. Restoring saved settings enables recovery only if the source policy
enables it and has a valid threshold; a null threshold or disabled source policy
stays off. This works without a live recovery switching API, provided the runtime
already supports two-output recovery models.

For a live change on a service that supports the existing native recovery API,
use **Detector controls → Live recovery override**. Leave **Keep selected
threshold** selected, edit the recovery checkbox, and click **Apply recovery**.
This override expires with the service invocation; see the persistence table below.

Selecting a model or editing a draft sends no SSH commands. Apply, preparation,
service start and restart do not schedule status refreshes. Check and Enter read
the current deployment plus recovery and primary policies once; the individual
Refresh buttons read their respective state. There is no periodic polling.
Deploy and start performs the validation needed to start safely, then leaves
runtime policy confirmation to your next Check/Refresh/Enter action.

The GUI does not copy or install runtime binaries. It uses only the remote
stack's existing commands and configuration. Live controls are available only
when that stack already exposes a supported recovery API and matching readback;
otherwise use the startup choice and stop/prepare/start cycle. Missing live
support is never presented as a requirement to install something through the GUI.

Model preparation retains the existing backend's export/parity environment
requirements described below. Failed or cancelled deployment stops the sequence;
the GUI does not start an unvalidated model. It checks the deployment,
configuration and runtime revisions again before starting. Individual preparation
and service actions remain available for manual operation.

| Computer | Required checkout | Purpose |
| --- | --- | --- |
| Desktop/workstation | This GUI repository | Builds and runs the Qt application and stores ignored local SSH settings. |
| Jetson | live-stack repository | Supplies the runtime, service unit, configuration, models, and deployment scripts invoked by the GUI. |

## Operating modes

The application uses the system OpenSSH client with key-based,
non-interactive authentication and the user's normal `known_hosts` file. It
streams output with severity-aware colors and also supports arbitrary
non-interactive commands. Ordinary unclassified output uses the normal text
color. Structured `debug` and `info` records are green, while structured
`warning`, `error`, and `fatal` records are red. The expected `input_paused`
warning is amber because it represents a healthy process waiting for usable
signal, not a runtime failure. Unclassified output remains neutral unless it
contains an explicit elevated-severity keyword.

Follow Log parses the service's structured diagnostics in the desktop GUI and
shows compact operational summaries instead of the full JSON records. The
Jetson still sends the original journal stream unchanged. Journal metadata and
diagnostic bookkeeping timestamps are removed; runtime status retains input,
throughput, inference, queue, output, latency, reset, and nonzero data-quality
information. Non-JSON lines such as boot separators remain visible.

### Installed service

Controls the checkout-linked `dkfz-native-uart-live` systemd unit. An
availability check must find the checkout's deploy unit before **Install /
repair unit link** is enabled, and must find the installed unit before Status,
Stop, Reload Unit, or Follow Log are enabled. Start and Restart additionally
require a non-I/O deployment preflight and explicit hardware confirmation.

The preflight follows the finalized deployment contract. It verifies that the
installed fragment resolves to `deploy/dkfz-native-uart-live.service` in the
selected checkout, that systemd does not have a stale unit definition, and
that the unit's read-only bind source matches the selected remote project
root. It also checks the service account and groups, `/var/lib/dkfz-live`, the
fixed `config/native_uart_live.conf`, the native runtime, and both files in the
stable `deploy/model` slot. While inactive, it runs
`--validate-only --check-engine`; while active, it avoids loading a second
engine and performs configuration-only validation. Capture paths in the
service config must stay under `/var/lib/dkfz-live`.

The preflight also requires the input pause/recovery contract: automatic input
reset, stuck-signal detection, recovery hysteresis, and nonzero UART/decoder
deadlines must be enabled. For an active service, Check availability reports
whether systemd's runtime status says input is running or paused. A paused
service remains healthy and should not be restarted merely because no usable
signal is present.

Install / repair unit link follows the live-stack deployment contract: while
the service is stopped, it links the selected checkout's
`deploy/dkfz-native-uart-live.service` into `/etc/systemd/system`, reloads
systemd, and verifies the resolved link. Select **Check** afterward to refresh
availability. It deliberately does not enable or start the service. It also does not
create the service account, groups, or state directory, or install the
namespaced journald policy; those remain one-time host provisioning steps.

Reload Unit runs `systemctl daemon-reload`. It is needed after editing the unit
file, but runtime, config, manifest, and engine changes need only a service
restart. The GUI cannot prove electrical safety.

The first tab, **Installed service**, also contains the **Finished models**
selector, **Refresh models**, and **Re-export weights + activate** button.
Model selection stays local until you select **Check availability**.

### Diagnostic / direct

Operates the current checkout beneath the selected remote project root. It can:

- build the TensorRT-enabled native runtime initially or rebuild it
  incrementally;
- prepare a selected best/tuned model on the Jetson in either of two modes:
  TensorRT from an existing ONNX (`--jetson-only`), or the complete
  PTH-to-ONNX-to-TensorRT pipeline;
- select an exact source directory such as `finished_models/target_r`, `finished_models/target_r/recovery` or
  `models_noCV/target_r` and pass it through `--model-dir`;
- atomically activate the resulting engine and manifest as
  `deploy/model/model.engine` and `deploy/model/model.conf`;
- validate the configuration and deserialize/check the TensorRT engine;
- start and stop the integrated runtime directly;
- select `none`, `jsonl`, `gpio`, or `gpio+jsonl` trigger output;
- optionally emit probability records;
- append stdout and stderr to a session log beneath the project root; and
- optionally capture raw UART bytes beneath the project root.

Both the service and direct runtime consume the stable `deploy/model` slot.
**Re-export weights + activate** runs the full saved-PTH-to-ONNX-to-TensorRT
pipeline in one action, including parity checks and activation. It works
without an existing ONNX file regardless of the preparation mode selection.
After success, select **Check** to refresh availability and validate readiness
before starting the model again. Stop the service and direct inference
first; the native runtime and full Jetson Python environment must be installed.
Selecting an entry in the dropdown fills the declared model name, exact source
directory, and variant locally. Select **Check** to query the Jetson for that selection.

The GUI reads its model name, variant, and source directory and prevents direct
Start when they do not match the current selection. Model preparation is
disabled while the service is active, activating, reloading, or deactivating,
matching `run/deploy_model.sh`; use Stop, prepare and activate the selected
model, select **Check**, then Start the service.

Direct inference defaults to `none`. GPIO-capable modes cannot start until the
hardware safety checkbox is selected. An empty raw-capture field explicitly
overrides and disables any capture path inherited from the committed config.
Direct Start stays disabled until the selected configuration and engine pass
validation, and direct/service starts are mutually locked to avoid competing
for the same UART or GPIO resources.

Both preparation modes require the selected variant's training configuration
and checkpoint on the Jetson because the standard TensorRT parity step uses
PyTorch. Existing-ONNX mode additionally requires the correctly named ONNX
artifact. Complete mode creates that ONNX artifact and runs PyTorch/ONNX parity
before continuing with TensorRT. It therefore requires the full configured
Jetson Python environment, not just `trtexec`.

The direct runtime remains attached to SSH for live output. The GUI records its
PID in the remote user's runtime directory, and Stop verifies that PID's
command line before sending `SIGTERM`. It will not use broad `pkill` matching
or stop a runtime it cannot identify.

During direct inference, missing, undecodable, frozen, sentinel, or baseline
input changes the GUI status to paused while leaving the managed process and
Stop action active. `input_paused` is shown in amber. Once stable moving input
returns, the status changes back to running; the runtime has cleared its old
temporal state and rebuilds a completely fresh model window before inference.

The GUI does not poll the Jetson in the background. Check, Refresh, and Enter
are explicit read requests. Primary or recovery Apply saves a request only;
use Check/Refresh/Enter to read confirmation. Input paused by the runtime can
leave a request pending until another completed frame. Explicitly started
inference and log streams remain attached until stopped.

Availability checks are always manual. After changing the connection or remote
project root, select **Check** in the Connection dialog. After changing the
model source directory, model, variant, or configuration, select **Check** on
the Model deployment / direct tab. **Check availability** in the main window
performs the same check from anywhere. Recheck after successful service or
model mutations. Operations whose executable, script,
configuration, active-slot files, or service unit is unavailable stay
disabled.

This is a command console, not a terminal emulator. General interactive
password prompts, full-screen terminal programs, and shell job control are
deliberately unsupported. For the built-in service actions, an optional
**Remote sudo password** can be entered once in the Connection section. It is
kept only in application memory, sent to `sudo` over the encrypted SSH
connection's standard input, and never saved to `connection.ini` or placed in
a command line. It is cleared when the SSH host, user, port, or identity changes
and when the application exits. With an empty password field, service actions
continue to use `sudo -n`, which supports a narrow passwordless sudoers rule.
Model preparation and initial TensorRT builds can be long-running; cancelling
their SSH process is not guaranteed to terminate every remote child.

### Recovery detector controls

**Detector controls** retains the primary threshold presets/custom value,
automatic/manual selection and early emission. Recovery has its own edited,
saved/requested and confirmed-active settings. All discovery, configuration
reads and operations use SSH beneath the configured remote project directory;
the desktop needs no local live-stack checkout. Discovery and read-only
recovery metadata use remote shell/coreutils and local Qt JSON parsing, so a
runtime-only target needs no Python.

The finished-model selector labels **Normal (single output)**, **Joint-recovery**,
**Frozen-recovery**, and **LOPO-recovery** from saved `recovery_head`,
`recovery_only`, and `cv_method` metadata. Missing training provenance is shown
as unknown. The directory or model name alone never establishes capability or
training mode. Known single-output selections disable startup recovery controls;
single-output deployments disable all live recovery edits.

| Control | What is saved | What applies after a service restart |
| --- | --- | --- |
| Primary custom/preset | Only `manual_threshold` in the deployed manifest; the runtime reloads it | The saved primary selection |
| Restore calibrated primary | Clears `manual_threshold`, first restoring the fallback to the manifest's calibrated automatic threshold | Calibrated automatic primary threshold |
| Early emission | Separate primary fallback/timing override, preserving the primary selection | Saved fallback/timing policy |
| Startup recovery | Local draft until preparation; forwards `--no-recovery` or uses source defaults | The prepared manifest's recovery policy |
| Live recovery Apply/Restore | Native recovery override bound to the engine and service invocation | Manifest recovery policy; the last selected threshold remains available for an explicit new request |

Primary thresholds accept finite values in `[0, 1]`; recovery thresholds accept
finite values in `(0, 1]`. An empty/null recovery threshold means disabled, and
a single-output manifest cannot contain a recovery threshold. The GUI validates
these rules before submitting values. Primary restore leaves the selected
early-emission policy intact. Neither head's controls rewrite source training
JSON, calibrated thresholds, preprocessing, holdoff, or the other head's policy.
Saved files and successful requests are never treated as active confirmation.

Choose **Use saved recovery settings** or **Recovery off — current output only** in the recovery deployment section. This is a preparation
setting beside model selection: both preparation modes forward `--no-recovery`
when selected. Restoring the saved deployment selection only edits that draft.
Use **Deploy and start** to apply it, or the individual service/preparation actions. Omitting the flag
respects disabled source defaults and null calibration; there is no preparation
flag to force recovery on, and no training JSON is rewritten. Preparation/export
itself retains the backend's existing environment requirements described above.

Targets without the native recovery API support deployment control only. **Refresh recovery state**
checks the deployed manifest/source and the actual remote native API version.
When already provided by the remote stack, live **Apply recovery** and
**Restore calibrated recovery** use
`build_native/inference/native_live_recovery_control` with schema
`dkfz-live-recovery-control-v1`, `api_version=1`, plus fresh engine- and
invocation-bound diagnostics. The GUI never installs this interface or its runtime.
A similarly named script does not establish support. Direct processes are unsupported by v1;
service journal status cannot confirm them.

On supported services, **Keep selected threshold** is the initial live selection:
enable/disable sends only `--enable`/`--disable`, preserving the backend's last
selected threshold rather than substituting calibration. Select calibrated recovery or explicitly choose
**Advanced: custom threshold**, enter a finite number in `(0, 1]`, edit the
checkbox and click Apply. Disabling retains the saved selected threshold;
null active threshold means off. A null calibrated threshold is shown as
**Recovery disabled by calibration** and never replaced with an arbitrary
value. Restore honors the source's disabled default. Scalar deployments show
**Recovery unavailable: single-output model**. Recovery requests preserve all
primary settings, and primary writers preserve recovery via a separate override.

A successful request means saved/pending. Click Refresh explicitly to obtain
native application readback; paused input can wait for the next completed
frame. Old primary-only runtime status is insufficient confirmation. Connection,
deployment or service changes invalidate confirmation; stale revisions reject
mutations. Live requests belong to one service invocation. After restart,
the deployed policy applies; the last selection remains available for a new
explicit request with matching engine identity. Use deployment-time current-only
for current-only startup across restarts.

The collapsed **Model and calibration details** section uses the *deployed* `source_model_dir` and variant, never the
newly selected GUI model. Best uses `train_config.json`; tuned uses
`tuning/tuned_config.json`. Source metadata is a saved snapshot, not active
runtime confirmation. Its original byte identity is unverified because existing
manifests do not pin a source-config hash. Missing, mismatched, changed-during-read
or newer source files are unavailable/unconfirmed; restoration refuses an
unusable source. Published recovery artifacts under `finished_models/MODEL/recovery/`,
`frozen_recovery/` and `lopo_recovery/` (plus legacy flat copies) are
discovered as best from `best.pth` + `train_config.json`; intermediate tuning/fold
artifacts outside finished_models are excluded. Capability/provenance never
comes from a name suffix.

Recovery models share one LSTM: current predicts the current frame and previous
predicts the previous frame with current context. Recovered events alone add
one frame of lookahead, displayed as `1000 * frame_size_raw / raw_hz` ms. Recovery
only training declares the primary weights frozen; verified-unchanged provenance
is shown separately. Missing provenance is unknown, and joint training does not
guarantee equivalence to an independent single-output model. Starting
current-only reproduces a frozen primary detector only with identical
preprocessing, thresholds and runtime settings. Switching recovery off live
cannot undo earlier emissions or their shared holdoff.

The panel displays calibration FP allowance, saved event metrics and the native
early-emission audit when present, including emission threshold, FP budget and
baseline FP/FN counts. Missing baseline counts stay unavailable. These are
calibration results, not outer-test or live measurements; a runtime threshold
does not enforce the FP allowance. Audit applicability is unconfirmed and the
policy is marked customized when primary threshold, early emission, holdoff or
recovery policy differs. Nothing silently recalibrates. Recovery-head nested
LOPO is labeled conditional on the frozen pretrained base: outer patients were
excluded from head training/selection, but base/preprocessing may already have
seen them. The entire detector is not established as patient-unseen. A selected
fixed epoch is shown only when supplied; training length is not substituted.
Frozen-base SHA256 is distinct from engine CRC32 or recovery-checkpoint identity.

Probability records label `probability` as the current-head score and
`recovery_prob` as the previous-frame score; `recovery_emitted_now` identifies
an emission on the current record. Trigger-event probability is the accepted
event score from either head; without explicit origin/matching probability
telemetry its head is unknown. Original emission timestamps remain intact and
no recovery event counts are inferred from timing. These fields are separate
from input-pause recovery/hysteresis diagnostics.

The optional training wrapper `./train_finished_models_recovery.sh --lopo
--num-epochs 35` is an explicit backend training job; no runtime control launches
it. Reports and fold/epoch checkpoints remain in the training workflow's model
directory, with published final artifacts discovered only under finished_models.

## Download a release

Each published GitHub Release is built and tested automatically for 64-bit
Linux and Windows. When the release workflow finishes, its **Assets** section
contains:

- `jetson-control-<tag>-linux-x86_64.AppImage` for Linux;
- `jetson-control-<tag>-linux-x86_64.AppImage.asc` containing its detached GPG signature;
- `jetson-control-release-signing-key.asc` containing the public GPG key;
- `jetson-control-<tag>-windows-x86_64-setup.exe` for Windows; and
- `SHA256SUMS.txt` for download verification.

On Linux, make the downloaded AppImage executable and start it:

```bash
chmod +x jetson-control-*-linux-x86_64.AppImage
./jetson-control-*-linux-x86_64.AppImage
```

The AppImage carries an embedded signature. Its detached signature can also be
checked with standard GnuPG tooling:

```bash
gpg --import jetson-control-release-signing-key.asc
gpg --verify jetson-control-*-linux-x86_64.AppImage.asc jetson-control-*-linux-x86_64.AppImage
```

On Windows, run the downloaded setup executable and approve the administrator
prompt to install **Jetson Control** for all users in Program Files. Launch it
from the Start menu or Windows Search; the installer also offers a desktop
shortcut. Remove it through **Settings > Apps > Installed apps > Jetson Control
> Uninstall**. Run a newer setup executable to update the system-wide
installation. If you used an older per-user installer, uninstall that copy
first to avoid having two installations.

The installer includes Qt and installs the required Visual C++ runtime when
needed. The application still requires an OpenSSH `ssh` client on the desktop
because all Jetson commands are executed through it. The Windows application
and installer are currently unsigned, so Windows SmartScreen may show an
unrecognized-app warning. The release workflow checks system-wide installation,
shortcuts, launching the installed application, and uninstallation before
uploading the Windows package.

To publish a new version, first make sure the release workflow is present on
the default branch. In GitHub, create a release for a tag such as `v0.1.0` and
publish it. Publishing either a normal release or a pre-release starts the
workflow; packages are attached only after both platform builds and all tests
pass. To rerun packaging manually, open **Actions → Release packages → Run
workflow** and enter the tag of an existing GitHub Release. The separate CI
workflow is also manual-only; neither workflow runs for ordinary pushes.

## Build on Linux

Clone this GUI repository wherever you keep desktop applications, then run the
following from its root:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
./build/jetson-control
```

Qt 6.5 or newer, CMake 3.21 or newer, a C++20 compiler, and OpenSSH are
required. The build directory remains local and is ignored by Git.

The focused `recovery`, `threshold-state`, `threshold-controller`, and
`model-deployment` tests use temporary files and a fake SSH/native-control host.
They cover all four model kinds, independent policy writes and calibration
preservation, threshold bounds, enable/disable without threshold replacement,
restore eligibility, restart selection versus startup policy, and rejection of
stale or mismatched readback. Tests need no Jetson, UART, GPIO, or running service.

## Local connection file

The connection file and private key belong to this GUI checkout, not to the
live-stack checkout. Copy the committed template and place the Jetson private
key beside it:

```bash
cp connection.ini.example connection.ini
cp /secure/path/to/jetson_ed25519 ./jetson_ed25519
chmod 600 jetson_ed25519
```

Then edit `connection.ini`:

```ini
[ssh]
host=256.256.256.256
user=orin
port=22
identity_file=jetson_ed25519
remote_directory=/path/to/software_stack
```

The path in `remote_directory` is evaluated on the Jetson. It does not need to
exist on the desktop.

`connection.ini` and the common private-key names in `.gitignore` are local
only. Do not force-add them to Git. A relative `identity_file` is resolved
relative to `connection.ini`, not relative to the shell's working directory.
The Connection dialog also has a file chooser that records an absolute path.
The identity may be a private key, or a public key whose corresponding private
key is loaded in `ssh-agent`. The GUI passes it to OpenSSH with `-i` and enables
`IdentitiesOnly=yes`.

At startup the GUI looks for `connection.ini` in the current directory, beside
the executable, and one directory above the executable (the normal development
layout). An explicit file can be selected from any location:

```bash
./build/jetson-control --connection-file /path/to/connection.ini
```

Without a connection file, the fields remain editable and normal
`~/.ssh/config` host aliases continue to work.

## Temporary SSH jump host

Keep temporary network routing in the desktop's normal `~/.ssh/config`. Give
the jump host and Jetson aliases:

```sshconfig
Host dkfz-jump
    HostName jump.example.org
    User YOUR_JUMP_USER
    IdentityFile /absolute/path/to/dkfz-gui/jump_key
    IdentitiesOnly yes

Host jetson-via-jump
    HostName 256.0.0.256
    User orin
    ProxyJump dkfz-jump
```

Then select that target alias in the GUI's ignored `connection.ini`:

```ini
[ssh]
host=jetson-via-jump
port=22
identity_file=jetson_key
remote_directory=/home/orin/work/live_stack
```

The jump host uses the key configured under `Host dkfz-jump`; the Jetson uses
the `identity_file` beside `connection.ini`. Use an absolute path for the jump
key because OpenSSH reads it from `~/.ssh/config`. Before starting the GUI,
accept both host keys and verify non-interactive access:

```bash
ssh dkfz-jump
ssh jetson-via-jump
ssh -o BatchMode=yes jetson-via-jump true
```

When direct access becomes available, remove `ProxyJump dkfz-jump` and change
the alias's `HostName` to the directly reachable Jetson address. No GUI rebuild
or source change is necessary.

## First connection

The application sets `BatchMode=yes` and normally uses
`StrictHostKeyChecking=yes`, so password and unknown-host prompts cannot block
it. For a first connection, open the Connection dialog, verify the Host, and
select **Trust and save a new host key on the next connection**, then select
**Check**. **Check availability** in the main window performs the same action.
This uses OpenSSH's `accept-new` mode for that attempt: an unknown key is saved
in the normal `known_hosts` file, but a changed key is still rejected. The
option resets after starting the connection.

An entry from `~/.ssh/config` can be entered directly into the Host field. The
GUI's explicit User, Port, and SSH key values take precedence over values in
SSH config.
Set Remote project root to the actual checkout containing `prepare_model.sh`,
`run/`, `config/`, `finished_models/`, and `build_native/`; do not set it to the
service-only `/opt/dkfz-live` mount. With the provided unit this is
`/home/orin/work/live_stack`.

## Cancellation

Cancel terminates the local SSH process for ordinary commands. Direct inference
uses its dedicated Stop action so the verified remote runtime receives
`SIGTERM`. The installed production runtime should continue to be managed
through systemd once its deployment has been reviewed and configured.

## Notice

This Software includes contributions by OpenAI's Codex, and images created using AcademicCloud's image creation tool.

## Disclaimer

Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>

This software is provided "as is", without any warranty or condition. To the
extent permitted by law, the copyright holder will not be liable for damages
arising from the software, its use, or its nature. See the [LICENSE](LICENSE)
file for the complete PolyForm Noncommercial License 1.0.0 terms.
