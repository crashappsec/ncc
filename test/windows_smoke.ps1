#Requires -Version 7.0

param(
    [Parameter(Mandatory = $true)]
    [string]$Ncc,

    [string]$Compiler,

    [string]$Transcript
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$BundleDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$NccPath = (Resolve-Path -LiteralPath $Ncc).Path

if ($Transcript) {
    if ([System.IO.Path]::IsPathRooted($Transcript)) {
        $TranscriptPath = $Transcript
    }
    else {
        $TranscriptPath = Join-Path $BundleDir $Transcript
    }
}
else {
    $TranscriptPath = Join-Path $BundleDir 'windows-smoke-transcript.txt'
}

if (-not $Compiler) {
    if ($env:NCC_COMPILER) {
        $Compiler = $env:NCC_COMPILER
    }
    else {
        $Compiler = 'clang'
    }
}

$env:NCC_COMPILER = $Compiler
if ([string]::IsNullOrEmpty($env:NCC_VERBOSE)) {
    $env:NCC_VERBOSE = '1'
}

function Invoke-Step {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Exe,

        [Parameter(ValueFromRemainingArguments = $true)]
        [string[]]$Args
    )

    $Display = @($Exe) + $Args
    Write-Host ""
    Write-Host (">> " + ($Display -join ' '))

    $Output = & $Exe @Args 2>&1
    $ExitCode = $LASTEXITCODE

    foreach ($Line in $Output) {
        Write-Host $Line
    }

    if ($ExitCode -ne 0) {
        throw "command failed with exit code $ExitCode"
    }
}

$WorkDir = Join-Path $BundleDir '.windows-smoke'
$TranscriptStarted = $false
if (Test-Path -LiteralPath $WorkDir) {
    Remove-Item -LiteralPath $WorkDir -Recurse -Force
}
New-Item -ItemType Directory -Path $WorkDir | Out-Null

try {
    Start-Transcript -Path $TranscriptPath -Force | Out-Null
    $TranscriptStarted = $true

    Write-Host "Bundle directory: $BundleDir"
    Write-Host "Transcript path: $TranscriptPath"
    Write-Host "Using NCC_COMPILER=$env:NCC_COMPILER"
    Write-Host "Using NCC_VERBOSE=$env:NCC_VERBOSE"

    $BangSrc = Join-Path $BundleDir 'test_bang.c'
    $OptionSrc = Join-Path $BundleDir 'test_option.c'
    $ConstexprSrc = Join-Path $BundleDir 'test_constexpr.c'
    $ProcessRunExe = Join-Path $BundleDir 'test_process_run.exe'

    $BangExe = Join-Path $WorkDir 'test_bang.exe'
    $OptionExe = Join-Path $WorkDir 'test_option.exe'
    $ConstexprExe = Join-Path $WorkDir 'test_constexpr.exe'

    Invoke-Step $NccPath '--ncc-help'
    Invoke-Step $ProcessRunExe

    Invoke-Step $NccPath '-o' $BangExe $BangSrc
    Invoke-Step $BangExe

    Invoke-Step $NccPath '-o' $OptionExe $OptionSrc
    Invoke-Step $OptionExe

    Invoke-Step $NccPath '-o' $ConstexprExe $ConstexprSrc
    Invoke-Step $ConstexprExe
}
finally {
    if (Test-Path -LiteralPath $WorkDir) {
        Remove-Item -LiteralPath $WorkDir -Recurse -Force
    }

    if ($TranscriptStarted) {
        Stop-Transcript | Out-Null
        Write-Host "Transcript saved to $TranscriptPath"
    }
}
