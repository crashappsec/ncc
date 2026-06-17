param(
    [string]$BuildDir = "build-msvc",
    [string]$BuildType = "debug",
    [string]$Target = "ncc:executable",
    [string]$TestName = "",
    [string]$LogPath = "",
    [switch]$SetupOnly,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$MesonArgs
)

$ErrorActionPreference = "Stop"

$Root = $PSScriptRoot
if ($Root -eq "") {
    $Root = (Get-Location).Path
}

$ResolvedLogPath = ""
if ($LogPath -ne "") {
    if ([System.IO.Path]::IsPathRooted($LogPath)) {
        $ResolvedLogPath = $LogPath
    } else {
        $ResolvedLogPath = Join-Path $Root $LogPath
    }

    $LogDir = Split-Path -Parent $ResolvedLogPath
    if ($LogDir -ne "" -and -not (Test-Path $LogDir)) {
        New-Item -ItemType Directory -Path $LogDir | Out-Null
    }

    Set-Content -Path $ResolvedLogPath -Value "build.ps1: log started $(Get-Date -Format o)" -Encoding utf8
}

function Write-BuildMessage {
    param([string]$Message)

    Write-Host $Message
    if ($script:ResolvedLogPath -ne "") {
        Add-Content -Path $script:ResolvedLogPath -Value $Message -Encoding utf8
    }
}

function Invoke-BuildNative {
    param(
        [string]$Command,
        [string[]]$Arguments
    )

    Write-BuildMessage ("build.ps1: running {0} {1}" -f $Command, ($Arguments -join " "))
    & $Command @Arguments 2>&1 | ForEach-Object {
        Write-Host $_
        if ($script:ResolvedLogPath -ne "") {
            Add-Content -Path $script:ResolvedLogPath -Value $_ -Encoding utf8
        }
    }

    return $LASTEXITCODE
}

function Test-HasMesonOption {
    param(
        [string[]]$Arguments,
        [string]$OptionName
    )

    foreach ($Argument in $Arguments) {
        if ($Argument -match "^-D$([regex]::Escape($OptionName))=") {
            return $true
        }
    }

    return $false
}

function Convert-MesonTargetToNinjaTarget {
    param([string]$MesonTarget)

    if ($MesonTarget -eq "ncc:executable") {
        return "ncc.exe"
    }

    if ($MesonTarget -match "^(.+):executable$") {
        return "$($Matches[1]).exe"
    }

    return $MesonTarget
}

$OriginalPythonPath = $env:PYTHONPATH

function Enable-WindowsPythonTempfileAclWorkaround {
    if ($env:OS -ne "Windows_NT") {
        return
    }

    $SiteDir = Join-Path $Root "scripts\windows-python-site"
    if (-not (Test-Path (Join-Path $SiteDir "sitecustomize.py"))) {
        return
    }

    if ($env:PYTHONPATH) {
        $env:PYTHONPATH = "$SiteDir;$env:PYTHONPATH"
    } else {
        $env:PYTHONPATH = $SiteDir
    }

    Write-BuildMessage "build.ps1: enabled Python tempfile ACL workaround for Meson"
}

if ($env:OS -eq "Windows_NT") {
    if (-not ("NccBuildErrorMode" -as [type])) {
        Add-Type -TypeDefinition @"
using System.Runtime.InteropServices;

public static class NccBuildErrorMode {
    [DllImport("kernel32.dll")]
    public static extern uint SetErrorMode(uint uMode);
}
"@
    }

    $NoPopupErrorMode = 0x0001 -bor 0x0002 -bor 0x0004 -bor 0x8000
    [void][NccBuildErrorMode]::SetErrorMode($NoPopupErrorMode)
    Write-BuildMessage "build.ps1: disabled native Windows crash popups for child tools"
}

Enable-WindowsPythonTempfileAclWorkaround

if ($ResolvedLogPath -ne "") {
    Write-BuildMessage "build.ps1: writing build log to $ResolvedLogPath"
}

$ExitCode = 0
Push-Location $Root
try {
    $SetupArgs = @("setup")
    if (Test-Path (Join-Path $BuildDir "meson-private\coredata.dat")) {
        $SetupArgs += "--reconfigure"
    }
    $SetupArgs += @(
        $BuildDir,
        "--buildtype=$BuildType"
    )
    if ($env:OS -eq "Windows_NT" -and -not (Test-HasMesonOption $MesonArgs "b_vscrt")) {
        $SetupArgs += "-Db_vscrt=md"
    }
    $SetupArgs += $MesonArgs

    if ($ResolvedLogPath -ne "") {
        $ExitCode = Invoke-BuildNative "meson" $SetupArgs
    } else {
        Write-BuildMessage ("build.ps1: running meson {0}" -f ($SetupArgs -join " "))
        & meson @SetupArgs
        $ExitCode = $LASTEXITCODE
    }
    if ($ExitCode -ne 0) {
        Write-BuildMessage "build.ps1: meson exited with code $ExitCode"
    }

    if ($ExitCode -eq 0 -and -not $SetupOnly) {
        $NinjaArgs = @("-C", $BuildDir)
        if ($env:NCC_JOBS) {
            $NinjaArgs += @("-j", $env:NCC_JOBS)
        } else {
            $NinjaArgs += @("-j", [Environment]::ProcessorCount)
        }
        if ($Target -ne "") {
            $NinjaArgs += (Convert-MesonTargetToNinjaTarget $Target)
        }

        if ($ResolvedLogPath -ne "") {
            $ExitCode = Invoke-BuildNative "ninja" $NinjaArgs
        } else {
            Write-BuildMessage ("build.ps1: running ninja {0}" -f ($NinjaArgs -join " "))
            & ninja @NinjaArgs
            $ExitCode = $LASTEXITCODE
        }
        if ($ExitCode -ne 0) {
            Write-BuildMessage "build.ps1: ninja exited with code $ExitCode"
        }
    }

    if ($ExitCode -eq 0 -and $TestName -ne "") {
        $TestArgs = @("test", "-C", $BuildDir, "--print-errorlogs", $TestName)

        if ($ResolvedLogPath -ne "") {
            $ExitCode = Invoke-BuildNative "meson" $TestArgs
        } else {
            Write-BuildMessage ("build.ps1: running meson {0}" -f ($TestArgs -join " "))
            & meson @TestArgs
            $ExitCode = $LASTEXITCODE
        }
        if ($ExitCode -ne 0) {
            Write-BuildMessage "build.ps1: meson test exited with code $ExitCode"
        }
    }
} finally {
    Pop-Location
    if ($null -eq $OriginalPythonPath) {
        Remove-Item Env:PYTHONPATH -ErrorAction SilentlyContinue
    } else {
        $env:PYTHONPATH = $OriginalPythonPath
    }
    if ($ResolvedLogPath -ne "") {
        Write-BuildMessage "build.ps1: log ended $(Get-Date -Format o)"
    }
}

if ($ExitCode -ne 0) {
    exit $ExitCode
}
