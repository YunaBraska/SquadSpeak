param([string]$BuildRoot = "$PSScriptRoot/../build/windows-ci")
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
if ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_ENVIRONMENT -ne 'github-hosted') {
    throw 'Virtual audio setup is restricted to disposable GitHub-hosted runners'
}

# VB-CABLE is donationware by VB-Audio: https://vb-audio.com/Cable/
# Evaluation terms: https://vb-audio.com/Services/licensing.htm
# Downloaded for CI only; never installed by or bundled with SquadSpeak.
New-Item -ItemType Directory -Force $BuildRoot | Out-Null
Start-Transcript -Path "$BuildRoot/audio-setup.log" -Force
try {
    $directory = Join-Path $env:RUNNER_TEMP 'squadspeak-vbcable'
    $archive = "$directory.zip"
    curl.exe --fail --location --silent --show-error --connect-timeout 20 --max-time 180 --retry 2 `
        --output $archive 'https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack45.zip'
    if ($LASTEXITCODE -ne 0) { throw "VB-CABLE download failed: $LASTEXITCODE" }
    if ((Get-FileHash $archive -Algorithm SHA256).Hash -ne 'B950E39F01AF1D04EA623C8F6D8EB9B6EA5C477C637295FABF20631C85116BFB') {
        throw 'VB-CABLE archive checksum mismatch'
    }
    Expand-Archive $archive -DestinationPath $directory -Force
    $catalog = Join-Path $directory 'vbaudio_cable64_win10.cat'
    $signature = Get-AuthenticodeSignature $catalog
    $signature | Format-List Status, StatusMessage, SignerCertificate, TimeStamperCertificate
    if ($signature.Status -ne 'Valid') { throw "VB-CABLE catalog signature is not valid: $($signature.Status)" }
    $signTool = Get-ChildItem "${env:ProgramFiles(x86)}/Windows Kits/10/bin/*/x64/signtool.exe" |
        Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $signTool) { throw 'Windows SDK signature verifier is unavailable' }
    foreach ($file in @('vbMmeCable64_win10.inf', 'vbaudio_cable64_win10.sys')) {
        & $signTool.FullName verify /kp /v /c $catalog (Join-Path $directory $file)
        if ($LASTEXITCODE -ne 0) { throw "Kernel signing verification failed for $file" }
    }
    Start-Service AudioEndpointBuilder
    Start-Service Audiosrv
    $setup = Start-Process "$directory/VBCABLE_Setup_x64.exe" -ArgumentList '-i', '-h' `
        -WorkingDirectory $directory -PassThru
    if (-not $setup.WaitForExit(60000)) {
        Stop-Process -Id $setup.Id -Force
        throw 'VB-CABLE installer exceeded 60 seconds'
    }
    $setup.Refresh()
    Write-Host "VB-CABLE installer exit code: $($setup.ExitCode)"
    if ($setup.ExitCode -ne 0) { throw "VB-CABLE installation failed: $($setup.ExitCode)" }
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        $device = Get-CimInstance Win32_SoundDevice | Where-Object { $_.Name -like '*VB-Audio*' -and $_.Status -eq 'OK' }
        if ($device) { break }
        Start-Sleep -Milliseconds 500
    } while ([DateTime]::UtcNow -lt $deadline)
    Get-CimInstance Win32_SoundDevice | Select-Object Name, Status, ConfigManagerErrorCode | Format-Table
    Get-PnpDevice -Class AudioEndpoint | Select-Object FriendlyName, Status, InstanceId | Format-Table
    if (-not $device) { throw 'VB-CABLE did not expose a working audio device without reboot' }
} finally {
    if (Test-Path "$env:windir/INF/setupapi.dev.log") {
        Get-Content "$env:windir/INF/setupapi.dev.log" -Tail 300 | Set-Content "$BuildRoot/audio-driver.log"
    }
    Stop-Transcript
}
