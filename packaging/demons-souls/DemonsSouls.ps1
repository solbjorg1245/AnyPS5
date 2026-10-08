<#
.SYNOPSIS
Demon's Souls (PPSA01341) launcher for AnyPS5 on Windows.

.DESCRIPTION
First start: pass your own decrypted dump with -Dump. The launcher then
  1. runs tools\relinker.exe on <dump>\eboot.bin and writes DemonsSouls.exe here
     (the relinker also writes the converted game modules into app0\sce_module),
  2. patches DemonsSouls.exe with tools\ds_patch.py (needs Python 3; skipped with a warning if
     Python is missing; refuses an unsupported game version),
  3. links every folder of the dump into app0\ (directory junctions, nothing is copied or
     modified in the dump; the few small top-level files are copied),
  4. starts the game.
Later starts need no arguments. Run with -Dump again after updating AnyPS5 or the dump.

Runtime settings (an already set environment variable always wins):
  APS5_HOST_IMPORT_MIB  guest memory the GPU may read in place; chosen from installed RAM
  APS5_BINDLESS_SLOTS   48 (textures one bindless material table may reference)

.EXAMPLE
.\DemonsSouls.ps1 -Dump "D:\Games\PPSA01341-app0"

.EXAMPLE
.\DemonsSouls.ps1
#>
[CmdletBinding()]
param(
    [string]$Dump,
    [switch]$SetupOnly,
    [switch]$NoPatch,
    [switch]$ToIntel,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$GameArgs
)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$exe = Join-Path $here "DemonsSouls.exe"
$app0 = Join-Path $here "app0"
$supportedBuild = "2025-10-15.877562"

function Find-Python {
    foreach ($candidate in @(@("py", "-3"), @("python3"), @("python"))) {
        $command = Get-Command $candidate[0] -ErrorAction SilentlyContinue
        if (-not $command) { continue }
        # Skip the Microsoft Store alias, which opens the Store instead of running Python.
        if ($command.Source -like "*\WindowsApps\*") { continue }
        $prefix = @($candidate | Select-Object -Skip 1)
        & $command.Source @prefix -c "import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)" 2>$null
        if ($LASTEXITCODE -eq 0) { return , (@($command.Source) + $prefix) }
    }
    return $null
}

function Install-Game([string]$dumpPath) {
    $dumpPath = (Resolve-Path -LiteralPath $dumpPath).Path
    $eboot = Join-Path $dumpPath "eboot.bin"
    foreach ($required in @("eboot.bin", "sce_module", "sce_sys")) {
        if (-not (Test-Path -LiteralPath (Join-Path $dumpPath $required))) {
            throw "$dumpPath does not look like a PPSA01341 app0 dump: $required is missing"
        }
    }
    if ((Resolve-Path -LiteralPath $here).Path -eq $dumpPath) { throw "Install AnyPS5 into its own folder, not into the dump" }
    $versionFile = Join-Path $dumpPath "gameversion.txt"
    if (Test-Path -LiteralPath $versionFile) {
        $version = (Get-Content -LiteralPath $versionFile -Raw) -replace '(?s).*BuildVersion=([^\r\n]+).*', '$1'
        if ($version.Trim() -ne $supportedBuild) {
            Write-Warning "dump BuildVersion is '$($version.Trim())'; this release supports $supportedBuild"
        }
    }

    Write-Host "== relinking $eboot"
    New-Item -ItemType Directory -Force -Path $app0 | Out-Null
    $relinkArgs = @("--windows")
    if ($ToIntel) {
        $relinkArgs += "--to-intel"
    } elseif ((Get-CimInstance Win32_Processor | Select-Object -First 1).Manufacturer -eq "GenuineIntel") {
        Write-Warning "Intel CPU: the game uses AMD-only instructions (emulated, slower); if it crashes with an illegal instruction, run again with -Dump ... -ToIntel"
    }
    & (Join-Path $here "tools\relinker.exe") @relinkArgs $eboot $exe
    if ($LASTEXITCODE -ne 0) { throw "relinker failed with exit code $LASTEXITCODE" }

    if ($NoPatch) {
        Write-Warning "skipping the executable patch (-NoPatch): job workers will spin and starve the game threads"
    } else {
        $python = Find-Python
        if ($python) {
            Write-Host "== patching DemonsSouls.exe"
            $pythonArgs = @($python | Select-Object -Skip 1)
            & $python[0] @pythonArgs -I (Join-Path $here "tools\ds_patch.py") $eboot $exe
            if ($LASTEXITCODE -ne 0) {
                Remove-Item -LiteralPath $exe -Force
                throw "ds_patch.py refused this dump (exit code $LASTEXITCODE); DemonsSouls.exe was removed"
            }
        } else {
            Write-Warning "Python 3.8+ not found: DemonsSouls.exe is NOT patched and will run much slower. Install Python (python.org or 'winget install Python.Python.3.12') and run this script with -Dump again."
        }
    }

    Write-Host "== linking the dump into app0"
    foreach ($item in Get-ChildItem -LiteralPath $dumpPath -Force) {
        # sce_module: the relinker writes the converted modules there. logs: the game writes there.
        if ($item.Name -in @("eboot.bin", "sce_module") -or $item.Name.StartsWith(".")) { continue }
        if ($item.Name -eq "logs") { New-Item -ItemType Directory -Force -Path (Join-Path $app0 "logs") | Out-Null; continue }
        $target = Join-Path $app0 $item.Name
        if (Test-Path -LiteralPath $target) {
            $existing = Get-Item -LiteralPath $target -Force
            if ($existing.LinkType) { $existing.Delete() } elseif ($item.PSIsContainer) { continue }
        }
        if ($item.PSIsContainer) {
            New-Item -ItemType Junction -Path $target -Target $item.FullName | Out-Null
        } else {
            Copy-Item -LiteralPath $item.FullName -Destination $target -Force
        }
    }
    Write-Host "== setup done"
}

if ($Dump) {
    Install-Game $Dump
} elseif (-not (Test-Path -LiteralPath $exe)) {
    Write-Host "DemonsSouls.exe is not set up yet. Run once with your own decrypted dump:"
    Write-Host "  .\DemonsSouls.ps1 -Dump `"D:\path\to\PPSA01341-app0`""
    exit 1
}
if ($SetupOnly) { exit 0 }

$os = Get-CimInstance Win32_OperatingSystem
$ramGiB = [math]::Round($os.TotalVisibleMemorySize / 1MB)
$commitGiB = [math]::Round($os.TotalVirtualMemorySize / 1MB)
if ($commitGiB -lt 64) {
    Write-Warning "RAM + page file is $commitGiB GiB; the game commits about 60 GiB. Enlarge the page file (System > About > Advanced system settings > Performance > Virtual memory) or the game may crash with out-of-memory errors."
}
if (-not $env:APS5_HOST_IMPORT_MIB) {
    $env:APS5_HOST_IMPORT_MIB = if ($ramGiB -ge 48) { "16384" } elseif ($ramGiB -ge 24) { "8192" } else { "4096" }
}
if (-not $env:APS5_BINDLESS_SLOTS) { $env:APS5_BINDLESS_SLOTS = "48" }

Set-Location -LiteralPath $here
& $exe @GameArgs
exit $LASTEXITCODE
