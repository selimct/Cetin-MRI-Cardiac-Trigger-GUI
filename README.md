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

A local Qt 6 desktop application for running commands on the Jetson over SSH
and viewing stdout, stderr, exit status, and service logs. Nothing from this
directory runs on the Jetson; commands are executed through its existing SSH
server.

This repository contains only the desktop GUI. Keep or clone it independently
from the live-stack repository. The desktop does not need a local live-stack
checkout: live-stack is required only on the Jetson, and `remote_directory`
always refers to that remote checkout.

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
- select an exact source directory such as `finished_models/target_r` or
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

The GUI does not poll the Jetson in the background. Editing fields or selecting
a model sends no requests; Check and Refresh buttons explicitly retrieve status.
Threshold actions save only when clicked; use **Refresh deployed thresholds**
afterward to confirm application. Pending confirmation does not block Refresh
or the rest of the GUI. Explicitly started inference and log streams remain
attached until stopped.

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
host=192.168.1.50
user=orin
port=22
identity_file=jetson_ed25519
remote_directory=/home/orin/work/live_stack
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
    HostName 10.0.0.25
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

This Software includes contributions by OpenAI's Codex.

## Disclaimer

Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>

This software is provided "as is", without any warranty or condition. To the
extent permitted by law, the copyright holder will not be liable for damages
arising from the software, its use, or its nature. See the [LICENSE](LICENSE)
file for the complete PolyForm Noncommercial License 1.0.0 terms.
