# Windows checks of how kosh-server takes its port. Exits 1 if any fails.
# Usage (PowerShell): tests\windows\port_test.ps1 -Server path\to\kosh-server.exe
#
# Windows's SO_REUSEADDR lets a socket bind a port another one is listening
# on, unlike POSIX's, so these check what the server sets instead:
#   1. A second server on a port in use is refused, and the first keeps serving.
#   2. A server restarts on its port right after closing connections itself
#      (they wait in TIME_WAIT or FIN_WAIT_2), which is what POSIX's
#      SO_REUSEADDR is for.
#   3. A failed bind reports the real error code, not 0.
param([Parameter(Mandatory)][string]$Server)
$ErrorActionPreference = "Stop"
$Server = (Resolve-Path $Server).Path
$work = Join-Path ([IO.Path]::GetTempPath()) ("kosh-port-test-" + [guid]::NewGuid())
New-Item -ItemType Directory $work | Out-Null
$script:failed = 0

function Get-FreePort {
    $l = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $l.Start(); $port = $l.LocalEndpoint.Port; $l.Stop()
    return $port
}

function Start-Kosh([int]$port, [string]$name, [string[]]$extra = @()) {
    $p = Start-Process -FilePath $Server -WorkingDirectory $work -NoNewWindow -PassThru `
        -ArgumentList (@("--port", $port, "--appendonly", "no") + $extra) `
        -RedirectStandardOutput "$work\$name.out" -RedirectStandardError "$work\$name.err"
    $null = $p.Handle   # keeps ExitCode readable after the process exits
    return $p
}

function Wait-Listening([int]$port) {
    $deadline = (Get-Date).AddSeconds(5)
    while ((Get-Date) -lt $deadline) {
        try { $c = [Net.Sockets.TcpClient]::new("127.0.0.1", $port); $c.Close(); return }
        catch { Start-Sleep -Milliseconds 50 }
    }
    throw "no server listening on port $port"
}

# Sends one command as a RESP array and returns the reply, read until the
# server has sent a whole line (or closed the connection)
function Send-Command([int]$port, [string[]]$words) {
    $c = [Net.Sockets.TcpClient]::new("127.0.0.1", $port)
    try {
        $s = $c.GetStream(); $s.ReadTimeout = 2000
        $msg = "*$($words.Count)`r`n" + (($words | ForEach-Object { "`$$($_.Length)`r`n$_`r`n" }) -join "")
        $bytes = [Text.Encoding]::ASCII.GetBytes($msg)
        $s.Write($bytes, 0, $bytes.Length)
        $reply = ""; $buf = New-Object byte[] 512
        do {
            $n = $s.Read($buf, 0, $buf.Length)
            $reply += [Text.Encoding]::ASCII.GetString($buf, 0, $n)
        } while ($n -gt 0 -and -not $reply.EndsWith("`r`n"))
        return $reply.TrimEnd()
    } finally { $c.Close() }
}

function Check([string]$name, [bool]$ok, [string]$detail) {
    if ($ok) { Write-Output "PASS  $name" }
    else { Write-Output "FAIL  $name`n      $detail"; $script:failed++ }
}

$running = @()
try {
    # 1. A second server on a port in use
    $port = Get-FreePort
    $first = Start-Kosh $port "first"; $running += $first
    Wait-Listening $port
    $null = Send-Command $port @("SET", "owner", "first")
    $second = Start-Kosh $port "second"; $running += $second
    $exited = $second.WaitForExit(3000)
    $err = Get-Content "$work\second.err" -Raw
    Check "a second server on a port in use is refused" `
        ($exited -and $second.ExitCode -eq 1 -and $err -match "Cannot listen on 127\.0\.0\.1:$port") `
        "exited: $exited, exit code: $($second.ExitCode), stderr: $err"
    $answers = 1..10 | ForEach-Object { Send-Command $port @("GET", "owner") }
    Check "the first server still answers every new connection" `
        (@($answers | Where-Object { $_ -match "first" }).Count -eq 10) "replies: $($answers -join ' | ')"
    Stop-Process -Id $first.Id -Force

    # 2. Restart right after the server closed connections itself (QUIT). Closed
    #    by both ends, they wait in TIME_WAIT; one whose client keeps its end
    #    open waits in FIN_WAIT_2. Microsoft warns that SO_EXCLUSIVEADDRUSE
    #    can block a restart "until the original connection becomes inactive".
    $port = Get-FreePort
    $old = Start-Kosh $port "old"; $running += $old
    Wait-Listening $port
    1..5 | ForEach-Object { $null = Send-Command $port @("QUIT") }
    $held = [Net.Sockets.TcpClient]::new("127.0.0.1", $port)
    $hs = $held.GetStream(); $hs.ReadTimeout = 2000
    $quit = [Text.Encoding]::ASCII.GetBytes("*1`r`n`$4`r`nQUIT`r`n"); $hs.Write($quit, 0, $quit.Length)
    $null = $hs.Read((New-Object byte[] 64), 0, 64)
    Stop-Process -Id $old.Id -Force; $old.WaitForExit()
    $states = @(Get-NetTCPConnection -LocalPort $port -ErrorAction SilentlyContinue | ForEach-Object { "$($_.State)" })
    $timeWait = @($states | Where-Object { $_ -eq "TimeWait" }).Count
    $finWait2 = @($states | Where-Object { $_ -eq "FinWait2" }).Count
    $new = Start-Kosh $port "new"; $running += $new
    $exited = $new.WaitForExit(1500)
    Check "restarts at once with $timeWait connections in TIME_WAIT and $finWait2 in FIN_WAIT_2" `
        ($timeWait -gt 0 -and $finWait2 -gt 0 -and -not $exited -and (Send-Command $port @("PING")) -eq "+PONG") `
        "the test needs both states; stderr: $(Get-Content "$work\new.err" -Raw)"
    $held.Close()
    Stop-Process -Id $new.Id -Force

    # 3. A failed bind says why. 192.0.2.1 (RFC 5737) is no machine's address.
    $bad = Start-Kosh (Get-FreePort) "bad" @("--bind", "192.0.2.1"); $running += $bad
    $exited = $bad.WaitForExit(3000)
    $err = Get-Content "$work\bad.err" -Raw
    Check "a failed bind reports the real error code" `
        ($exited -and $bad.ExitCode -eq 1 -and $err -match "\(Winsock error 10049\)") "stderr: $err"
} finally {
    foreach ($p in $running) { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
    Start-Sleep -Milliseconds 200
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
if ($script:failed -gt 0) { Write-Output "$($script:failed) failed"; exit 1 }
Write-Output "all passed"
exit 0
