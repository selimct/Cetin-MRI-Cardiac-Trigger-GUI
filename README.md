<!--
Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
Developed at the German Cancer Research Center (DKFZ).
-->

# CMR Cardiac Trigger — Jetson Control GUI

[![Source Code](https://raw.githubusercontent.com/selimct/DKFZ-MRI-Cardiac-Trigger-GUI/refs/heads/main/packaging/source-badge.svg)](https://github.com/selimct/Cetin-Cardiac-Trigger-SourceCode.git)
[![ECG/POX Data](https://raw.githubusercontent.com/selimct/DKFZ-MRI-Cardiac-Trigger-GUI/refs/heads/main/packaging/data-badge.svg)](https://github.com/selimct/Cetin-UHF-ECGPOX-Data.git)
[![ParquetPlot](https://raw.githubusercontent.com/selimct/ParquetPlot/refs/heads/main/resources/parquetplot-gitlab-badge.svg)](https://github.com/selimct/ParquetPlot)

A Qt 6 desktop application for controlling the DKFZ cardiac-trigger backend on
an NVIDIA Jetson Orin Nano over SSH. It installs or reuses model engines,
starts/stops the detector, adjusts supported live settings, and displays remote
output and service logs.

| Computer | What runs there |
| --- | --- |
| Desktop | This GUI and the system OpenSSH client. No local backend checkout is required for normal use. |
| Jetson | The existing `live_stack` checkout, trained models, TensorRT engines, runtime and systemd service. |

The GUI calls the backend's existing tools. New engine builds and saved-engine
activation require its Jetson Python/parity environment. The GUI does not upload
desktop binaries or install backend helper packages.

- [Build and run](#build-and-run)
- [Connect to the Jetson](#connect-to-the-jetson)
- [Quick start](#quick-start)
- [Download a release](#download-a-release)
- [User guide: controls, saved engines and troubleshooting](docs/user-guide.md)
- [Runtime integration: contracts, implementation and validation limits](docs/runtime-integration.md)

## Build and run

On Linux, install Qt 6.5+ development libraries, CMake 3.21+, a C++20 compiler,
and the OpenSSH client. Ninja is optional. Tests additionally use Qt Test; the
Linux fake-host integration fixtures use Bash, Python 3 and standard Linux tools.

From this checkout:

```bash
./build.sh
./build/jetson-control
```

`build.sh` configures and compiles the GUI in Release mode. It does not launch
the application or install dependencies.

| Option | Effect |
| --- | --- |
| `--test` | Build the tests too, then run CTest with failure output. |
| `--debug` | Build Debug instead of Release. |
| `--jobs N` or `-j N` | Limit parallel build/test jobs. |
| `--build-dir DIR` | Use another build directory; relative paths start at the caller's working directory. |
| `--help` | Show usage and requirements. |

```bash
./build.sh --test --jobs 2
```

The default directory is `build/` beside the script, even when the script is
called from elsewhere. New builds use Ninja when available; existing builds
keep their CMake generator. Parallelism defaults to at most four CPU cores,
or the value of `CMAKE_BUILD_PARALLEL_LEVEL` when set. For a custom Qt
installation, set `CMAKE_PREFIX_PATH` to its installation prefix.

The equivalent manual Debug/test workflow is:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --parallel 2
```

The ignored `build/` directory stays on the desktop. Windows source builds use
CMake and a matching Qt/MSVC toolchain; see the [CI workflow](.github/workflows/ci.yml)
for the configured build commands. The Bash convenience script targets Linux.

## Connect to the Jetson

You can enter settings in **Connection**, or create local startup defaults:

```bash
cp connection.ini.example connection.ini
```

Edit `connection.ini`, using the desktop's key path and the Jetson's checkout path:

```ini
[ssh]
host=192.168.1.50
user=orin
port=22
identity_file=/absolute/path/to/jetson_ed25519
remote_directory=/home/orin/work/live_stack
```

- `identity_file` is local to the desktop. A relative path is resolved beside
  `connection.ini`; use an absolute path instead of `~`. An SSH-agent public key
  is also supported when its matching private key is loaded in the agent.
- `remote_directory` is the actual checkout on the Jetson containing
  `prepare_model.sh`, `run/`, `config/`, `finished_models/` and `build_native/`.
  With the supplied service this is `/home/orin/work/live_stack`. The service's
  `/opt/dkfz-live` mount is not the host checkout path.
- **Remote sudo password** is optional and kept in memory for privileged actions.
  With an empty field the GUI uses non-interactive `sudo -n`. It is not an SSH
  login password, is never saved to the INI, and is cleared when SSH credentials
  change or the application exits.

The application uses key-based, non-interactive SSH. For an unknown host, verify
the target and select **Trust and save a new host key on the next connection**,
then **Check Jetson**. This accepts a new host key for that attempt; changed
keys remain rejected. Normal connections require a known host key.

Connection-dialog edits apply to the current session. Edit the INI to retain
defaults for future launches. The application searches for `connection.ini` in
the current directory, beside the executable, and one directory above it. To
choose another file:

```bash
./build/jetson-control --connection-file /path/to/connection.ini
```

`connection.ini` and common key filenames are ignored by Git. Keep private keys
out of commits. SSH aliases and jump hosts can be configured in the desktop's
`~/.ssh/config`; see [connection troubleshooting](docs/user-guide.md#connection-and-jump-hosts).

## Quick start

1. Open **Connection**, enter the Jetson settings, then click **Check Jetson**.
2. In **Run**, read **Installed model** and **Next step**. If the intended model
   is already installed, review the hardware confirmation and use **Start detector**;
   no installation is needed. To switch models, stop the detector, **Check Jetson**,
   select a **Model** and choose **Recovery after restart**.
3. Choose **one** installation path: select a matching saved engine and click
   **Use saved engine**, or open **Show new-engine build**, confirm
   **ECG3 = ECG2 − ECG1**, and click **Build and install new engine** for a fresh
   conversion. **Refresh saved engines**, **Refresh models**, and **Check Jetson**
   load the saved-engine catalog.
4. When **Installed — ready to start** appears, review the hardware confirmation
   and click **Start detector**. Build/reuse already performs its readiness check.

Selecting a model only edits a draft. **Start detector** always runs the installed
model shown in the Run tab. Installing or reusing an engine requires an idle
detector and leaves it stopped; starting it is a separate action.

| Tab | Use it for |
| --- | --- |
| **Run** | Choose/install an engine, read the installed-model summary, start/stop/restart the detector, and view live logs. |
| **Detector settings** | Apply primary thresholds and supported live recovery changes, then refresh to confirm application. |
| **Advanced** | Validate or run a temporary test, inspect source/configuration fields, rebuild detector software, or repair service setup. |

Hover over buttons for their effect and prerequisites. **Next step** and the
status area explain the current operation and follow-up actions. Dropdowns and
numeric controls ignore mouse-wheel changes; page/list scrolling still works.

Checks are explicit. **Check Jetson**, **Check test setup** and Enter within the
tabs/Connection dialog request the readiness/model probe and policy readbacks.
The individual Refresh buttons read their own state. There is no periodic status
polling; explicitly opened output/log streams continue until stopped.

## Engines and output

Previous installations are stored **on the Jetson**, relative to its checkout:

```text
finished_models/                         # Published training config and weights
build_native/prepared_models/
  gui-<model>-<UTC-time>-<uuid>/          # Retained conversion/validation bundle
    model/train_config.json
    model/best.pth
    model/model.engine
    validation_report.json
    logs/
deploy/model/                           # Currently installed engine and manifest
```

**Use saved engine** verifies and activates a matching bundle without ONNX
export or TensorRT generation. It still runs the backend's activation checks;
reuse can take time. **Build and install new engine** always performs a fresh
conversion. Keep the complete bundle and matching source config/weights for reuse.
See [saved engines](docs/user-guide.md#saved-engines) for eligibility and failure cases.

Drag the divider above the output pane to resize it, or use **Expand output** /
**Restore controls**. The pane shows remote stdout/stderr and streamed build logs.
Button presses, the shell commands sent by the GUI, and GUI completion messages
are not echoed there; they belong to the status area. A successful silent command
can leave the pane unchanged. Backend tools may print their own commands in logs.

Installation streams conversion/parity log files and readiness validation output,
including final log bytes after a failed step. Full per-step logs remain in the
bundle on the Jetson. The visible console retains its latest 20,000 lines.
**View live logs** opens the installed service's journal stream; **Close live logs**
closes the stream while leaving the detector running.

## Download a release

The [release workflow](.github/workflows/release.yml) builds and tests 64-bit
Linux and Windows packages for published releases, including pre-releases. When
packaging succeeds, the release assets include:

- `jetson-control-<tag>-linux-x86_64.AppImage` and its detached `.asc` signature;
- `jetson-control-release-signing-key.asc`;
- `jetson-control-<tag>-windows-x86_64-setup.exe`;
- `SHA256SUMS.txt`.

For a downloaded Linux AppImage:

```bash
chmod +x jetson-control-*-linux-x86_64.AppImage
./jetson-control-*-linux-x86_64.AppImage
```

The AppImage also carries an embedded signature. To check the detached signature:

```bash
gpg --import jetson-control-release-signing-key.asc
gpg --verify jetson-control-*-linux-x86_64.AppImage.asc jetson-control-*-linux-x86_64.AppImage
```

On Windows, run the setup executable and approve the administrator prompt. It
installs **Jetson Control** for all users, includes Qt, and installs the required
Visual C++ runtime when needed. Launch from the Start menu or Windows Search;
uninstall through **Settings → Apps → Installed apps**. Run a newer installer to
update. Remove an older per-user installation first if you have one. The Windows
application/installer are unsigned, so SmartScreen may display a warning.
The desktop still needs OpenSSH.

To publish packages, create and publish a GitHub Release for the intended tag.
For a packaging rerun, use **Actions → Release packages → Run workflow** with an
existing release tag. The separate CI workflow is manual-only. Ordinary pushes
do not trigger either workflow.

## Validation and further reading

The local build and ten CTest suites cover fake SSH/systemd workflows, deployment
guards, saved-engine reuse, policy controls, console output and parsing. An
optional backend report check uses synthetic fingerprints. These checks do not
establish model accuracy, sustained Jetson timing or physical UART/GPIO behavior.

- [User guide and troubleshooting](docs/user-guide.md)
- [Runtime contract and remaining target validation](docs/runtime-integration.md)

## Notice

This Software includes contributions by OpenAI's Codex, and images created using AcademicCloud's image creation tool.

## Disclaimer

Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>

This software is provided "as is", without any warranty or condition. To the
extent permitted by law, the copyright holder will not be liable for damages
arising from the software, its use, or its nature. See the [LICENSE](LICENSE)
file for the complete PolyForm Noncommercial License 1.0.0 terms.
