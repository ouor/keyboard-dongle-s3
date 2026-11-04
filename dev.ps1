<#
  One entry point for the repetitive stuff.   .\dev.ps1 <cmd> [-Port COMx]

  build      keycodes -> DongleConfig.exe -> firmware
  flash      build + flash everything (auto: finds the stick, sends `dfu` if our firmware is running)
  monitor    serial terminal on the dongle CDC (or -Port for a UART0 adapter on GPIO43/44)
  all        flash + monitor
  test       host unit tests for keymap.c (clang)
  ports      list ESP32-S3 USB ports
  erase-nvs  wipe BLE bonds + saved keymap only
  storage    rewrite only the FAT partition (DongleConfig.exe) - no app reflash
  coredump   decode the last crash stored in flash
  dist       merged single .bin in dist\ (flash at 0x0 with esptool or https://espressif.github.io/esptool-js/)
  menuconfig idf.py menuconfig
#>
param(
    [Parameter(Position = 0)][ValidateSet('build', 'flash', 'monitor', 'all', 'test', 'ports', 'erase-nvs', 'storage', 'coredump', 'dist', 'menuconfig')]
    [string]$Cmd = 'build',
    [string]$Port
)
$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
$Fw = "$Root\firmware"
$IdfPath = if ($env:IDF_PATH) { $env:IDF_PATH } else { "$HOME\esp\esp-idf-v6.1" }  # set IDF_PATH if installed elsewhere
$PidJtag = '1001'  # USB-Serial/JTAG: ROM download mode, or any app that hasn't started TinyUSB
$PidApp = '4007'   # our TinyUSB composite (firmware/main/usb.c USB_PID)

function Use-Idf {
    if (Get-Command idf.py -ErrorAction SilentlyContinue) { return }
    if (-not (Test-Path "$IdfPath\export.ps1")) { throw "ESP-IDF not found at $IdfPath. Set `$env:IDF_PATH (see README: Setup)." }
    . "$IdfPath\export.ps1" *> $null
}

function Invoke-Checked { & $args[0] @($args | Select-Object -Skip 1); if ($LASTEXITCODE) { throw "$($args[0]) failed ($LASTEXITCODE)" } }

function Get-EspPorts {
    Get-CimInstance Win32_PnPEntity -Filter "PNPDeviceID LIKE 'USB\\VID_303A%'" | ForEach-Object {
        if ($_.Name -match '\((COM\d+)\)') {
            [pscustomobject]@{ Port = $Matches[1]; Pid = ($_.PNPDeviceID -replace '.*PID_([0-9A-F]{4}).*', '$1'); Name = $_.Name }
        }
    }
}

function Send-Line($p, $line) {
    $sp = New-Object System.IO.Ports.SerialPort $p, 115200
    $sp.DtrEnable = $true  # firmware only talks when DTR is set
    $sp.Open()
    try { Start-Sleep -Milliseconds 100; $sp.Write("$line`n"); Start-Sleep -Milliseconds 300 } finally { try { $sp.Close() } catch {} }
}

# Port that esptool can talk to. Kicks our firmware into download mode when needed.
function Get-FlashPort {
    if ($Port) { return $Port }
    $all = @(Get-EspPorts)
    $jtag = $all | Where-Object Pid -eq $PidJtag | Select-Object -First 1
    if ($jtag) { return $jtag.Port }
    $app = $all | Where-Object Pid -eq $PidApp | Select-Object -First 1
    if ($app) {
        Write-Host "dongle firmware on $($app.Port) -> sending 'dfu'"
        Send-Line $app.Port 'dfu'
        foreach ($i in 1..40) {
            Start-Sleep -Milliseconds 250
            $jtag = Get-EspPorts | Where-Object Pid -eq $PidJtag | Select-Object -First 1
            if ($jtag) { Start-Sleep -Milliseconds 500; return $jtag.Port }
        }
        throw "no download port after 'dfu'. Hold BOOT (K1) while plugging in, then retry."
    }
    throw "ESP32-S3 not found. Plug it in (hold BOOT/K1 while plugging if it doesn't show up)."
}

function Get-MonitorPort {
    if ($Port) { return $Port }
    $p = Get-EspPorts | Sort-Object { $_.Pid -ne $PidApp } | Select-Object -First 1
    if (-not $p) { throw 'ESP32-S3 not found.' }
    $p.Port
}

function Build {
    Invoke-Checked python "$Root\tool\gen_keycodes.py"
    & "$Root\tool\build.ps1" | Out-Host
    if ($LASTEXITCODE) { throw 'DongleConfig build failed' }
    Use-Idf
    Push-Location $Fw; try { Invoke-Checked idf.py build } finally { Pop-Location }
}

# parttool runs esptool several times; a reset in between boots the app and the next call loses the download port.
function Parttool {
    Use-Idf
    $p = Get-FlashPort
    Invoke-Checked python "$IdfPath\components\partition_table\parttool.py" --port $p --esptool-args after=no-reset @args
    Invoke-Checked python -m esptool --port $p read-mac | Out-Null  # default --after hard-reset: back to the app
}

switch ($Cmd) {
    'build' { Build }
    'flash' { Build; Push-Location $Fw; try { Invoke-Checked idf.py -p (Get-FlashPort) flash } finally { Pop-Location } }
    'monitor' { Use-Idf; Push-Location $Fw; try { idf.py -p (Get-MonitorPort) monitor --no-reset } finally { Pop-Location } }
    'all' { & $PSCommandPath flash -Port $Port; Start-Sleep 3; & $PSCommandPath monitor }
    'test' {
        New-Item -ItemType Directory -Force "$Fw\build-host" | Out-Null
        Invoke-Checked clang -std=c11 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS "$Fw\test\test_keymap.c" "$Fw\main\keymap.c" "$Fw\main\hid_parse.c" -o "$Fw\build-host\test_keymap.exe"
        Invoke-Checked "$Fw\build-host\test_keymap.exe"
    }
    'ports' { Get-EspPorts | Format-Table Port, Pid, Name }
    'erase-nvs' { Parttool erase_partition --partition-name=nvs }
    'storage' { & "$Root\tool\build.ps1" | Out-Host; Use-Idf; Push-Location $Fw; try { Invoke-Checked idf.py build } finally { Pop-Location }; Parttool write_partition --partition-name=storage --input "$Fw\build\storage.bin" }
    'coredump' { Use-Idf; Push-Location $Fw; try { Invoke-Checked idf.py -p (Get-FlashPort) coredump-info } finally { Pop-Location } }
    'dist' {
        Build
        Push-Location $Fw; try { Invoke-Checked idf.py merge-bin -o merged.bin } finally { Pop-Location }
        New-Item -ItemType Directory -Force "$Root\dist" | Out-Null
        $out = "$Root\dist\esp-dongle-$(Get-Date -Format yyyyMMdd-HHmm).bin"
        Copy-Item "$Fw\build\merged.bin" $out
        Get-Item $out | Select-Object Name, Length
        Write-Host 'flash at 0x0:  esptool --chip esp32s3 write-flash 0x0 <file>   or https://espressif.github.io/esptool-js/'
    }
    'menuconfig' { Use-Idf; Push-Location $Fw; try { idf.py menuconfig } finally { Pop-Location } }
}
