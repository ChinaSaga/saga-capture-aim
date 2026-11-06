$ErrorActionPreference = 'Stop'
$reportDir = Join-Path $PSScriptRoot ('Reports\' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $reportDir -Force | Out-Null
function Save-Section($Name, [scriptblock]$Action) {
    try {
        & $Action | Out-String -Width 240 | Out-File (Join-Path $reportDir ($Name + '.txt')) -Encoding utf8
    } catch {
        $_.Exception.Message | Out-File (Join-Path $reportDir ($Name + '-unavailable.txt')) -Encoding utf8
    }
}
Save-Section 'Windows' { Get-CimInstance Win32_OperatingSystem | Select-Object Caption,Version,BuildNumber,LastBootUpTime | Format-List }
Save-Section 'Devices' { Get-CimInstance Win32_PnPEntity | Where-Object { $_.PNPClass -in @('Display','Monitor','MEDIA','USB','Ports','Mouse','HIDClass') } | Select-Object Name,PNPClass,Status,ConfigManagerErrorCode,PNPDeviceID | Format-List }
Save-Section 'Drivers' { Get-CimInstance Win32_PnPSignedDriver | Select-Object DeviceName,DriverProviderName,DriverVersion,DriverDate,IsSigned,InfName | Format-Table -AutoSize }
Save-Section 'Services' { Get-CimInstance Win32_Service | Select-Object Name,DisplayName,State,StartMode,PathName | Format-List }
Save-Section 'Startup' { Get-CimInstance Win32_StartupCommand | Select-Object Name,Command,Location | Format-List }
Save-Section 'Processes' { Get-Process | Select-Object ProcessName,Id,Path | Format-Table -AutoSize }
Save-Section 'Defender' { Get-MpComputerStatus | Select-Object AMServiceEnabled,AntivirusEnabled,RealTimeProtectionEnabled,AntivirusSignatureLastUpdated | Format-List }
Save-Section 'SecureBoot' { Confirm-SecureBootUEFI }
Save-Section 'TPM' { Get-Tpm | Select-Object TpmPresent,TpmReady,TpmEnabled | Format-List }
Save-Section 'DisplayRegistry' {
    Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Enum\DISPLAY' -Recurse -ErrorAction Stop | Where-Object { $_.PSChildName -eq 'Device Parameters' } | ForEach-Object {
        $edid = (Get-ItemProperty -LiteralPath $_.PSPath -ErrorAction Stop).EDID
        [pscustomobject]@{ RegistryPath = $_.Name; EDIDHex = $(if ($edid) { [BitConverter]::ToString($edid) }); OverridePresent = (Test-Path (Join-Path $_.PSPath 'EDID_OVERRIDE')) }
    } | Format-List
}
Save-Section 'RecentSystemEvents' {
    Get-WinEvent -FilterHashtable @{ LogName='System'; StartTime=(Get-Date).AddHours(-24); Level=1,2,3 } -MaxEvents 250 | Select-Object TimeCreated,ProviderName,Id,LevelDisplayName,Message | Format-List
}
Save-Section 'RecentApplicationEvents' {
    Get-WinEvent -FilterHashtable @{ LogName='Application'; StartTime=(Get-Date).AddHours(-24); Level=1,2,3 } -MaxEvents 150 | Select-Object TimeCreated,ProviderName,Id,LevelDisplayName,Message | Format-List
}
Write-Host "Report saved: $reportDir"
Write-Host 'Read-only collection complete. No services, drivers, EDID or security settings changed.'
exit 0
