# smtc_probe.ps1 - what Windows' media sessions (SMTC, the "now playing"
# overlay) report, polled every half second.
#
# The question this answers (ROADMAP, automatic shared output): while
# NexExclusive holds the device exclusively, another application cannot open
# it, and the audio session layer gives no notice when an application that
# played before tries again. Does that application still publish
# "Playing" here? If a browser tab with a video reports Playing even though it
# has no sound, the host can use this as the signal that someone wants the
# device.
#
# Run it with Windows PowerShell 5.1 (it needs the WinRT projection), then
# start playback in NetEase so it holds the device, and play a video in the
# browser:
#     powershell -NoProfile -ExecutionPolicy Bypass -File smtc_probe.ps1 -Seconds 60
# Only changes are printed. Nothing is sent anywhere.
param([int]$Seconds = 60)

Add-Type -AssemblyName System.Runtime.WindowsRuntime
$asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() |
    Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
                   $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
function Await($op, [Type]$type) {
    $task = $asTask.MakeGenericMethod($type).Invoke($null, @($op))
    $task.Wait(-1) | Out-Null
    $task.Result
}
[Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager, Windows.Media.Control, ContentType = WindowsRuntime] | Out-Null
$manager = Await ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager]::RequestAsync()) `
                 ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager])

$last = @{}
$end = (Get-Date).AddSeconds($Seconds)
"{0}  polling SMTC for {1} s" -f (Get-Date -Format 'HH:mm:ss.fff'), $Seconds
while ((Get-Date) -lt $end) {
    $now = @{}
    foreach ($session in @($manager.GetSessions())) {
        $info = $session.GetPlaybackInfo()
        $now[$session.SourceAppUserModelId] = [string]$info.PlaybackStatus
    }
    foreach ($app in $now.Keys) {
        if ($last[$app] -ne $now[$app]) {
            "{0}  {1,-40} {2}" -f (Get-Date -Format 'HH:mm:ss.fff'), $app, $now[$app]
        }
    }
    foreach ($app in $last.Keys) {
        if (-not $now.ContainsKey($app)) {
            "{0}  {1,-40} (session gone)" -f (Get-Date -Format 'HH:mm:ss.fff'), $app
        }
    }
    $last = $now
    Start-Sleep -Milliseconds 500
}
