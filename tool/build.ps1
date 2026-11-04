# Builds DongleConfig.exe straight into the firmware's FAT image folder (gets flashed into the MSC partition).
$out = "$PSScriptRoot\..\firmware\fatfs_image\DongleConfig.exe"
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
& "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /nologo /target:winexe /optimize+ /codepage:65001 /r:System.Management.dll /out:$out "$PSScriptRoot\DongleConfig.cs" "$PSScriptRoot\KeyCodes.cs"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
Get-Item $out | Select-Object Name, Length
