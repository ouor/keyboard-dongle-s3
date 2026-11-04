# Lists (default) or removes (-Remove, needs admin) stale Windows device entries for Espressif VID 303A.
# Use after changing USB descriptors when the dongle shows up wrong (missing COM port / drive / keyboard).
param([switch]$Remove)

$ghosts = Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_303A' -and -not $_.Present }
if (-not $ghosts) { 'no stale VID_303A devices'; return }
$ghosts | Format-Table Class, FriendlyName, InstanceId -AutoSize

if (-not $Remove) { "$($ghosts.Count) stale device(s). Run with -Remove (as admin) to delete them."; return }
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { throw 'run as administrator to remove devices' }
foreach ($g in $ghosts) { pnputil /remove-device "$($g.InstanceId)" | Select-Object -Last 1 }
