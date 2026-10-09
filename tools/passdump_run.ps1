# Takes one APS5_PASS_DUMP frame while the camera pans. The game must already run with APS5_PASS_DUMP=1
# (and APS5_CAPTURE_DIR=<Port>\capture, or APS5_PASS_DUMP_DIR=<Port>) and stand where the fault shows
# (Boletaria). Holds the pan key through sendkey.ps1, creates <Port>\PASSDUMP_NOW -TriggerMs into the hold
# (the driver arms at the next present and dumps the frame after it), waits for the dump's "#complete"
# line, then prints passdump_report.py's summary of it.
# usage: passdump_run.ps1 [-Key H] [-HoldMs 3000] [-TriggerMs 1000] [-TimeoutS 600] [-NoReport]
param(
    [string]$Port = "E:\projects\dsd\port",
    [string]$Key = "H",
    [int]$HoldMs = 3000,
    [int]$TriggerMs = 1000,
    [int]$TimeoutS = 600,
    [string]$SendKey = "E:\projects\dsd\tools\sendkey.ps1",
    [switch]$NoReport
)

if (-not (Get-Process DemonsSouls -ErrorAction SilentlyContinue)) { throw "DemonsSouls is not running" }
$dumps = Join-Path $Port "passdump"
$before = @{}
if (Test-Path $dumps) { Get-ChildItem $dumps -Directory | ForEach-Object { $before[$_.Name] = $true } }

$hold = Start-Process powershell -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $SendKey, "-Keys", $Key, "-HoldMs", $HoldMs) -NoNewWindow -PassThru
Start-Sleep -Milliseconds $TriggerMs
New-Item -ItemType File -Force (Join-Path $Port "PASSDUMP_NOW") | Out-Null
Write-Host "PASSDUMP_NOW created $TriggerMs ms into the $Key hold"

$deadline = (Get-Date).AddSeconds($TimeoutS)
$folder = $null
while ((Get-Date) -lt $deadline) {
    if (Test-Path $dumps) {
        $new = Get-ChildItem $dumps -Directory | Where-Object { -not $before.ContainsKey($_.Name) } | Sort-Object LastWriteTime | Select-Object -Last 1
        if ($new) {
            $folder = $new.FullName
            $index = Join-Path $folder "index.csv"
            if ((Test-Path $index) -and (Get-Content $index -Tail 1) -like "#complete*") { break }
        }
    }
    Start-Sleep -Milliseconds 500
}
$hold.WaitForExit()
if ($null -eq $folder) { throw "no dump appeared under $dumps within $TimeoutS s (is APS5_PASS_DUMP=1 set, and is PASSDUMP_NOW still there?)" }
$last = Get-Content (Join-Path $folder "index.csv") -Tail 1
if ($last -notlike "#complete*") { Write-Warning "the dump in $folder is not complete after $TimeoutS s" }
Write-Host "dump: $folder"
Write-Host $last
if (-not $NoReport) { python (Join-Path $PSScriptRoot "passdump_report.py") $folder }
