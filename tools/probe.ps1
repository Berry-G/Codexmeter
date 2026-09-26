param([switch]$ShowValues, [string]$CodexPath)
$ErrorActionPreference = 'Stop'
$info = [Diagnostics.ProcessStartInfo]::new()
if (-not $CodexPath) { $CodexPath = (Get-Command codex.exe -ErrorAction Stop).Source }
$info.FileName = $CodexPath
$info.Arguments = 'app-server --listen stdio://'
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$info.RedirectStandardInput = $true
$info.RedirectStandardOutput = $true
$info.RedirectStandardError = $true
$process = [Diagnostics.Process]::Start($info)
$errors = $process.StandardError.ReadToEndAsync()
try {
    $process.StandardInput.WriteLine('{"id":1,"method":"initialize","params":{"clientInfo":{"name":"codexmeter","version":"0.1.0"}}}')
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    while ([DateTime]::UtcNow -lt $deadline) {
        $line = $process.StandardOutput.ReadLineAsync()
        if (-not $line.Wait(20000)) { throw 'App Server timeout' }
        if ($null -eq $line.Result) { throw 'App Server closed stdout' }
        $message = $line.Result | ConvertFrom-Json
        if ($message.id -eq 1) {
            if ($message.error) { throw 'Initialization failed' }
            $process.StandardInput.WriteLine('{"method":"initialized"}')
            $process.StandardInput.WriteLine('{"id":2,"method":"account/rateLimits/read"}')
        }
        if ($message.id -eq 2) {
            if ($message.error) { Write-Output ('rateLimits error: ' + $message.error.code); break }
            # Only quota fields, never credentials or account identifiers.
            $result = $message.result
            Write-Output 'Rate-limit read succeeded. Account values are hidden by default.'
            if ($ShowValues) {
                $result.rateLimits | Select-Object limitId,primary,secondary | ConvertTo-Json -Depth 4
            }
            break
        }
    }
} finally {
    $process.StandardInput.Close()
    if (-not $process.WaitForExit(2000)) { $process.Kill($true) }
    $process.Dispose()
}
