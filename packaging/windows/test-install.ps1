# Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Developed at the German Cancer Research Center (DKFZ).

param(
    [Parameter(Mandatory)]
    [string]$InstallerPath
)

$ErrorActionPreference = 'Stop'
$installer = (Resolve-Path -LiteralPath $InstallerPath).Path
$installDir = Join-Path $env:ProgramFiles 'Jetson Control'
$startMenu = Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'Jetson Control.lnk'
$desktop = Join-Path ([Environment]::GetFolderPath('CommonDesktopDirectory')) 'Jetson Control.lnk'
$uninstallKey = 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{23B365E7-7611-4F73-B355-A92B8397E055}_is1'
$appPathKey = 'HKLM:\Software\Microsoft\Windows\CurrentVersion\App Paths\jetson-control.exe'

# This test installs and removes the app, so only run it on a clean machine.
foreach ($path in @($installDir, $startMenu, $desktop, $uninstallKey, $appPathKey)) {
    if (Test-Path -LiteralPath $path) {
        throw "Installer smoke test requires a clean machine; $path already exists"
    }
}

try {
    $setup = Start-Process -FilePath $installer -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/TASKS=desktopicon' -Wait -PassThru
    if ($setup.ExitCode -ne 0) {
        throw "Installer failed with code $($setup.ExitCode)"
    }

    foreach ($path in @("$installDir\jetson-control.exe", "$installDir\LICENSE", "$installDir\platforms\qwindows.dll", $startMenu, $desktop, $uninstallKey, $appPathKey)) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "Installation is missing $path"
        }
    }
    $entry = Get-ItemProperty -LiteralPath $uninstallKey
    if ($entry.DisplayName -ne 'Jetson Control' -or -not $entry.UninstallString) {
        throw 'Jetson Control is not correctly registered in installed apps'
    }
    if ((Get-Item -LiteralPath $appPathKey).GetValue('') -ne "$installDir\jetson-control.exe") {
        throw 'App Paths registration points to the wrong executable'
    }
    $shell = New-Object -ComObject WScript.Shell
    foreach ($link in @($startMenu, $desktop)) {
        $shortcut = $shell.CreateShortcut($link)
        if ($shortcut.TargetPath -ne "$installDir\jetson-control.exe" -or $shortcut.WorkingDirectory -ne $installDir) {
            throw "Shortcut points to the wrong application or working directory: $link"
        }
    }

    # Remove build-time Qt paths and launch outside the source and install trees.
    $originalPath = $env:PATH
    $originalPluginPath = $env:QT_PLUGIN_PATH
    $originalPlatformPluginPath = $env:QT_QPA_PLATFORM_PLUGIN_PATH
    $process = $null
    try {
        $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\OpenSSH"
        $env:QT_PLUGIN_PATH = $null
        $env:QT_QPA_PLATFORM_PLUGIN_PATH = $null
        $process = Start-Process -FilePath "$installDir\jetson-control.exe" -WorkingDirectory $env:TEMP -PassThru
        Start-Sleep -Seconds 5
        if ($process.HasExited) {
            throw "Installed Jetson Control exited unexpectedly with code $($process.ExitCode)"
        }
    } finally {
        $env:PATH = $originalPath
        $env:QT_PLUGIN_PATH = $originalPluginPath
        $env:QT_QPA_PLATFORM_PLUGIN_PATH = $originalPlatformPluginPath
        if ($process -and -not $process.HasExited) {
            Stop-Process -Id $process.Id
            $process.WaitForExit()
        }
    }
} finally {
    $uninstaller = Join-Path $installDir 'unins000.exe'
    if (Test-Path -LiteralPath $uninstaller) {
        $uninstall = Start-Process -FilePath $uninstaller -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait -PassThru
        if ($uninstall.ExitCode -ne 0) {
            throw "Uninstaller failed with code $($uninstall.ExitCode)"
        }
    }
}

foreach ($path in @("$installDir\jetson-control.exe", $startMenu, $desktop, $uninstallKey, $appPathKey)) {
    if (Test-Path -LiteralPath $path) {
        throw "Uninstall left behind $path"
    }
}
