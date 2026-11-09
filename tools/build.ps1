# Build / flash an ESP32 + RT-Thread project without Git Bash.
#
# The bash route needs three fixes at once (MSYSTEM must be absent, the exported
# PATH entries are in Windows form so bash cannot resolve idf.py, and the
# environment script has LF/UTF-8 assumptions). Setting the same variables in
# PowerShell and invoking python directly sidesteps all three.
#
# Usage:
#   .\build.ps1 build                     # s31-rtt-test, the default project
#   .\build.ps1 -p COM16 flash
#   .\build.ps1 -Project c5-rtt-test build
#   .\build.ps1 -Project c5-rtt-test size
#
# The target is not passed on the command line: each project's
# sdkconfig.defaults carries CONFIG_IDF_TARGET, and the project keeps its own
# build directory. That is what allows the S31/S3/C5 images to coexist here.

param(
    [string]$Project = 's31-rtt-test',
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$IdfArgs = @('build'),
    [switch]$SkipGuardCheck
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$tools = 'C:\Users\yilives\.espressif\tools'

# Pre-build gate: Xtensa-only symbols reached through shared code break the C5
# link, and only at LINK time - several minutes in. Nine such probes have done it
# in this port. The check costs about a second and names the file and line.
if (-not $SkipGuardCheck) {
    $guardPy = 'C:\Users\yilives\.espressif\python_env\idf6.1_py3.13_env\Scripts\python.exe'
    if (Test-Path $guardPy) {
        & $guardPy (Join-Path $PSScriptRoot 'check_xtensa_guards.py') --quiet
        if ($LASTEXITCODE -ne 0) {
            Write-Host ''
            Write-Host 'build.ps1: Xtensa-only references without a guard are present.' -ForegroundColor Red
            Write-Host 'Run tools/check_xtensa_guards.py to see them, or pass -SkipGuardCheck.' -ForegroundColor Red
            exit 1
        }
    }
}

$env:IDF_PATH = Join-Path $root 'esp-idf-rtt'
$env:ESP_IDF_VERSION = '6.1'
$env:IDF_PYTHON_ENV_PATH = 'C:\Users\yilives\.espressif\python_env\idf6.1_py3.13_env'
$env:IDF_CCACHE_ENABLE = '1'
$env:LOCALAPPDATA = 'C:\Users\yilives\AppData\Local'
$env:ESP_ROM_ELF_DIR = 'C:\Users\yilives\.espressif\tools\esp-rom-elfs\20241011\'
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue

$pathParts = @(
    "$tools\riscv32-esp-elf-gdb\17.1_20260402\riscv32-esp-elf-gdb\bin",
    "$tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin",
    "$tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin",
    "$tools\cmake\4.0.3\bin",
    "$tools\ninja\1.12.1",
    "$tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64",
    "$env:IDF_PATH\components\espcoredump",
    "$env:IDF_PATH\components\partition_table",
    "$env:IDF_PATH\components\app_update",
    "$env:IDF_PYTHON_ENV_PATH\Scripts",
    "$env:IDF_PATH\tools"
)
$env:PATH = ($pathParts -join ';') + ';' + $env:PATH

$python = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
$idfPy = Join-Path $env:IDF_PATH 'tools\idf.py'
$projectDir = Join-Path $root $Project

if (-not (Test-Path $projectDir)) {
    Write-Error "no such project: $projectDir"
    exit 2
}

Push-Location $projectDir
try {
    & $python $idfPy @IdfArgs
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
