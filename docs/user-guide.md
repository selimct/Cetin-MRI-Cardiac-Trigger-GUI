<!--
Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
Developed at the German Cancer Research Center (DKFZ).
-->

# User guide

[README: build, connection and quick start](../README.md) ·
[Runtime contracts and validation limits](runtime-integration.md)

## Find the right control

The application has three tabs: **Run**, **Detector settings**, and **Advanced**.
Use **Run** for normal operation. Read **Next step** for suggested follow-up
actions and **Installed model** for the model that Start will actually run.
Model selection alone does not install or activate anything.

Hover over an action for its purpose and prerequisites. Optional build controls,
calibration details and remote commands are collapsed. Dropdowns and numeric
controls ignore mouse-wheel changes, including when focused; use clicks or the
keyboard to change them. The wheel can still scroll pages and open lists.

## Run a model

| Action | What it does | What is needed |
| --- | --- | --- |
| **Check Jetson** | Read connection, model catalogs, installed-model readiness and supported policy readbacks. | SSH connection settings and access to the selected remote checkout. |
| **Refresh models** | Read published model entries and saved engines. | Read access to the remote checkout. |
| **Refresh saved engines** | Find complete prior installations and update the matching entries. | Select the desired model and recovery policy to see relevant entries. |
| **Use saved engine** | Verify and install the selected prior engine; check readiness. | Detector/test run stopped, matching source artifacts, usable saved report and Jetson Python/parity tools. |
| **Build and install new engine** | Export ONNX, build TensorRT, validate, install and check readiness. | Detector/test run stopped, saved weights/config, ECG3 confirmation, validation recordings and target build tools. |
| **Start detector** | Start the installed systemd service. | Successful readiness check and hardware confirmation. |
| **Stop detector** | Stop that service; retain the installed model. | An observed active service. Click Check Jetson afterward to refresh readiness. |
| **Restart detector** | Stop/start the installed service using saved startup settings. | Ready, active service and hardware confirmation. Live recovery overrides expire. |
| **Show service status** | Print a single systemd status result in the output pane. | An installed service found by Check Jetson. |
| **View live logs** | Stream recent and new service journal records. | An installed service found by Check Jetson. Close the stream before another main action. |

Build and reuse never start or stop the detector. After either succeeds with
**Installed — ready to start**, review the hardware confirmation and click
**Start detector**. No extra Check is required for that combined workflow.
After a separate Start, Stop, Restart or service-setup action, use **Check Jetson**
to obtain a new readiness snapshot. Checks and Refresh actions are manual;
explicitly opened runtime/log streams continue to deliver events.

### Build a new engine

1. Stop the detector or test run, then **Check Jetson**.
2. Select the **Model** and **Recovery after restart** policy.
3. Open **Show new-engine build** and confirm that the trained ECG3 input is
   **ECG2 − ECG1**.
4. If necessary, expand **Build options and manual steps** to set **Jetson Python**
   or **Validation recordings**.
5. Click **Build and install new engine** and inspect the streamed output.
6. Once ready, review the hardware confirmation and click **Start detector**.

Empty recording settings use whichever of these files exist on the Jetson:

```text
data/live/p12/p12_out.parquet
data/live/p13/p13_out.parquet
```

At least one is required for a new build. Overrides are project-relative parquet
paths, one per line. The backend replays recorded ECG to check numerical and
event-coordinate equivalence; it does not retrain or tune the model. A conversion
pass is not an accuracy or sustained real-time qualification.

The optional manual buttons split the workflow: **Build files only** creates and
validates a new bundle; **Install files built this session** activates that bundle.
Changing model/build settings clears manual activation eligibility. After a manual
installation, **Check Jetson** before starting. The main build button performs
preparation, installation and readiness verification together.

## Saved engines

Previous installations live on the Jetson under:

```text
<remote_project_root>/build_native/prepared_models/gui-<model>-<UTC-time>-<uuid>/
```

They are separate from `finished_models/`, which holds published source weights
and training configuration. `deploy/model/` is the single active installation
slot and changes when an engine is installed.

To switch back, select the original Model entry and matching **Recovery after
restart** policy, refresh saved engines, choose a bundle, then **Use saved engine**
with the detector stopped. This skips ONNX export and TensorRT generation.
The backend still runs fingerprint, parity and engine/activation checks, so reuse
is not instantaneous and still needs its Jetson Python/parity environment.

The saved-engine selector filters by model name, variant and startup recovery
policy. Entries show model, **Best**/**Fine-tuned**, local preparation time,
precision, frame rate and recovery policy. Timezone text is omitted; folder
timestamps remain UTC. Missing metadata is marked unavailable. Hover for the
full folder path. Identical display labels get numbered suffixes.

Discovery excludes incomplete/failed preparations and reports without the required
fingerprints, source confirmation or successful recorded stream validation.
On reuse, the GUI checks the report revision and compares the selected source
configuration and weights byte-for-byte with the bundle's copies. The backend
then verifies the saved artifacts. A stale report, different weights/config,
policy mismatch or unusable engine is rejected; reuse never silently rebuilds.

Keep the complete bundle, including `model/train_config.json`, `model/best.pth`,
`model/model.engine` and `validation_report.json`, plus the matching published
source files. Tuned bundles normalize their source copies to those same filenames.
Reuse uses the report's previous ECG-source confirmation and recording evidence;
it does not require the new-build checkbox or fresh recording paths.

If the intended model is already installed, use **Check Jetson** and **Start
detector**. Reinstalling or regenerating it is unnecessary. Check can recognize a
GUI-installed model after reopening the application by comparing the bundle's
config/weights to the selected published model.

## Detector settings

These controls use the running installed service. **Apply** sends a request;
**Refresh** reads confirmation. A successful write alone does not prove the
running detector has applied it. Paused input can delay application until a
completed frame is available.

### Primary detection threshold

- Enter a finite value from **0 through 1**, then **Apply threshold**. This saves
  the primary selection for future starts and requests a live change.
- **Restore model default** returns to the calibrated automatic primary threshold,
  preserving the chosen early-emission timing policy.
- **Emit qualifying peaks earlier (applies immediately)** saves/requests its timing
  policy when clicked; it has no separate Apply button.
- **Refresh active settings** reads saved state and native confirmation.
- **Show presets, calibration and timing** exposes the saved DER/FP-first presets
  and detailed model/timing readouts. Using a preset saves a manual selection;
  it does not recalibrate the model.

Early emission allows qualifying peaks that reach the reference level to emit
earlier on the falling edge. Smaller peaks wait for the selected detection
threshold. Changes clear an unfinished primary candidate and can alter trigger
timing; saved calibration results do not establish live performance under edits.

### Recovery after restart

This selector is in **Run**, beside model selection:

- **Use saved recovery settings** respects the source model's enabled default
  and calibration. A disabled default or null calibrated threshold remains off.
- **Recovery off — current output only** disables recovery events while retaining
  the two-output model. It corresponds to a distinct saved-bundle policy.
- **Reset selection** only resets the local draft. It sends no request.

Install a matching saved engine or build/install a new one to persist that policy,
then start separately. The controls are disabled for known single-output models.

### Recovery during this run

Live recovery requires a two-output model and the backend's supported native
service API. Direct test processes are not supported by that API.

1. **Refresh recovery state** to read the installed model and current request.
2. Edit **Enable recovery (press Apply to send)**.
3. Leave **Keep selected threshold**, choose **Use calibrated threshold**, or
   select **Advanced: custom threshold** and enter a finite value **greater than
   0 and at most 1**.
4. Click **Apply recovery for this run**, then **Refresh recovery state** to
   confirm application.

Keeping the selected threshold preserves its last numeric value across disable
requests. **Restore saved recovery policy** requests the source's enabled state
and calibration for the current run; it can restore disabled recovery too. It
requires usable, unchanged source metadata. A null calibration is not replaced
with an arbitrary threshold. Neither checkbox edits nor threshold drafts send a
request until Apply is clicked.

| Setting/action | After restarting the detector |
| --- | --- |
| Primary custom or preset threshold | Saved primary selection applies. |
| Restore model default | Calibrated automatic primary threshold applies. |
| Early-emission checkbox | Saved timing policy applies. |
| Installed startup recovery policy | Installed manifest policy applies. |
| Live recovery Apply/Restore | Override expires; installed startup policy applies. The last numeric selection can be retained for a new explicit request. |

**Model and calibration details** describes the deployed model's saved source,
not a newly selected draft. Missing training provenance is unknown. Joint,
frozen and LOPO recovery labels come from saved metadata, not folder names.
Calibration audits and FP allowances are historical results; live edits do not
recalibrate the detector or enforce those allowances.

## Output and cancellation

- Drag the divider to resize the pane. **Expand output** uses the working area;
  **Restore controls** returns to the previous layout.
- The pane shows remote stdout/stderr. GUI button-click banners, echoed shell
  commands and completion summaries are omitted. Follow-up guidance and operation
  results remain in **Next step** and the status area. Backend tools may print
  their own commands as part of their output.
- New-engine builds stream retained conversion/parity `.log` files as they grow,
  including final bytes after a failure. Installation readiness output is visible.
  Internal GUI protocol records stay out of the display.
- **View live logs** formats structured service diagnostics into compact summaries.
  The original remote journal is unchanged. Info/debug is green, elevated
  severity red, and expected `input_paused` warnings amber; ordinary text is neutral.
- **Clear output** clears the display only. Full build logs stay in the bundle;
  a configured test-session log stays on the Jetson. Visible history is bounded
  to 20,000 lines.
- **Close live logs** closes the stream while leaving the detector running.
  **Stop test run** sends a targeted stop request to the tracked direct process.
- **Cancel operation** terminates the local SSH operation. Preparation forwards
  received cancellation signals to its process group, but SSH cancellation cannot
  guarantee every remote child has stopped. Check the Jetson before retrying.

Successful service Start can produce no console text. Use **Check Jetson**,
**Show service status** or **View live logs** to inspect the detector afterward.

## Advanced tools

| Action/field | Purpose |
| --- | --- |
| **Check test setup** | Run the readiness/model probe using the selected source and test settings. |
| **Validate test setup** | Validate the selected configuration and deserialize/check the installed engine, without opening UART/GPIO or starting inference. |
| **Start test run** / **Stop test run** | Run the installed engine temporarily over SSH with the test configuration; stop the tracked process when finished. |
| **Rebuild detector software** | Compile native runtime software on the Jetson. This does not train weights or generate model engines. |
| **Repair service setup** | Link the checkout's existing systemd unit and reload systemd while the detector is stopped. Does not enable/start it or provision host accounts/groups. |
| **Reload service definition** | Run systemd daemon-reload after editing the unit. Does not restart the detector. |
| Model name/source/variant | Set an exact source, such as `finished_models/target_r/recovery` or `models_noCV/target_r`. Normally filled by the Run selector. |
| Trigger output | `none`: no trigger output; `jsonl`: event records; `gpio`: physical pulses; `gpio+jsonl`: both. GPIO modes require hardware confirmation. |
| Runtime config | Project-relative configuration for the temporary test. The installed service has its own fixed config. |
| Combined session log | Append test stdout/stderr to a project-relative Jetson path. |
| Raw UART capture | Optional project-relative byte capture. Empty explicitly disables inherited capture. |
| Emit probability records | Per-frame records during JSONL-capable test output; can produce substantial output. |

Both installed and test runs consume `deploy/model/`. A test requires the selected
model to match that slot and successful config/engine validation. Test starts
recheck service inactivity under the shared deployment lock and publish a tracked
PID. Stop verifies the process before signalling it. Stop the background detector
before a test; external tools must cooperate with the same guards/locks.

**Show remote command** opens an optional non-interactive SSH command field.
**Run command** executes it on the Jetson and can change remote files/settings.
This console does not support interactive passwords, full-screen terminal
programs or terminal job control.

## Connection and jump hosts

Host aliases use the desktop's normal `~/.ssh/config`. The GUI always passes its
Port value, passes User when filled, and passes the selected identity when set;
those explicit values take precedence over the corresponding SSH configuration.

For a temporary jump host:

```sshconfig
Host dkfz-jump
    HostName jump.example.org
    User YOUR_JUMP_USER
    IdentityFile /absolute/path/to/jump_key
    IdentitiesOnly yes

Host jetson-via-jump
    HostName 192.168.1.50
    User orin
    IdentityFile /absolute/path/to/jetson_ed25519
    IdentitiesOnly yes
    ProxyJump dkfz-jump
```

Set the GUI Host to `jetson-via-jump` and its Port to the target's SSH port.
Verify the host keys and non-interactive access in the desktop terminal first:

```bash
ssh dkfz-jump true
ssh jetson-via-jump true
ssh -o BatchMode=yes jetson-via-jump true
```

When direct access is available, remove `ProxyJump` and update the alias's
`HostName`. No GUI rebuild is needed. Connection-dialog edits are session-only;
edit `connection.ini` for future launch defaults.

## Troubleshooting

| Symptom | What to check or do |
| --- | --- |
| Start is disabled | Check Jetson, inspect **Show readiness and timing details**, and review the hardware confirmation. The detector may already be active, a test may be running, or readiness may be unconfirmed. |
| Another action is disabled while viewing logs | Use **Close live logs** to release the main command runner. |
| Model list is empty | Refresh models and check the remote checkout path. Published entries need a matching saved training config and checkpoint. |
| No matching saved engines | Check model, variant and startup policy; refresh the catalog. Failed/incomplete bundles are excluded. Retain the complete bundle and matching source files, or build a new engine. |
| Reuse rejects source or report changes | The selected source differs from the saved bundle, or its report changed. Refresh and select the correct bundle; use a new build when the source changed intentionally. |
| A new build cannot find recordings | Set existing project-relative raw parquet paths under **Build options and manual steps → Validation recordings**, or provide the standard p12/p13 files. |
| Permission denied / public-key authentication | Check desktop key readability, the SSH user/port, loaded agent key and alias settings. The sudo password field does not provide SSH login authentication. |
| Unknown host key | Verify the target, allow a new key for the next connection in Connection, then Check Jetson. Changed keys remain rejected. |
| A sudo action fails | Supply **Remote sudo password** or configure the required non-interactive sudo permissions on the Jetson. |
| Recovery edits are disabled | Single-output models have no recovery head. Live edits additionally require the running service and a supported native recovery API; otherwise use startup policy plus installation. |
| Apply is saved/pending | Refresh the relevant settings. Paused input may defer application until another completed frame. A restart/deployment change requires fresh readback. |
| `recovery_rejected`: override belongs to another deployment/invocation | A leftover live override does not match the engine/model or current service run. It is ignored; the current policy is preserved, normally startup policy after restart. Refresh recovery state to verify the active policy. This warning alone does not imply a failed model installation or disabled recovery. |
| `input_paused` | The runtime is waiting for usable input. The process can remain healthy; inspect signal/communication status rather than restarting solely because it is paused. |
| Output stays unchanged after Start | Start may succeed silently. Check status or open live logs; GUI click/command banners are intentionally omitted. |
| A build or activation was cancelled/failed | Keep its evidence, check remote activity and installed readiness, then retry deliberately. A failed/cancelled SSH request leaves the remote result unconfirmed. |

## Model timing

The optional `delayed_100hz` model is separately trained for four raw samples
per frame at 400 Hz, predicting the previous frame with 10 ms of subsequent
context. It cannot be enabled by changing a GUI switch on existing 50 Hz weights.
The selector and model details expose the trained target, frame rate, window and
holdoff when metadata is available. Single-output delayed models have no recovery
controls. For training and target qualification, use the backend's
`docs/100hz-delayed-target.md` and the [integration notes](runtime-integration.md#models-and-coordinates).
