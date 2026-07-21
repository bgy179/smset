param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Install', 'Uninstall')]
    [string]$Action,

    [string]$ExePath,

    [string]$ServiceName = 'SmsWechatRouterGateway'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Resolve-ExecutablePath {
    param([string]$InputPath)

    if ($InputPath -and $InputPath.Trim().Length -gt 0) {
        return (Resolve-Path -Path $InputPath).Path
    }

    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

    $candidates = @(
        (Join-Path $scriptDir 'sms2wechat-v7.exe'),
        (Join-Path $scriptDir 'smset.exe'),
        (Join-Path $scriptDir 'sms_wechat_router_gateway.exe')
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -Path $candidate -PathType Leaf) {
            return (Resolve-Path -Path $candidate).Path
        }
    }

    throw "Cannot find service executable. Pass -ExePath explicitly."
}

if (-not (Test-IsAdministrator)) {
    throw 'Please run this script in an elevated PowerShell (Run as Administrator).'
}

$exe = Resolve-ExecutablePath -InputPath $ExePath

switch ($Action) {
    'Install' {
        Write-Host "Installing service using: $exe"
        & $exe install
        if ($LASTEXITCODE -ne 0) {
            throw "Install command failed with exit code $LASTEXITCODE"
        }

        $service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
        if ($null -eq $service) {
            throw "Service '$ServiceName' not found after install."
        }

        if ($service.Status -ne 'Running') {
            Start-Service -Name $ServiceName
            Write-Host "Service '$ServiceName' started."
        } else {
            Write-Host "Service '$ServiceName' is already running."
        }

        Write-Host "Install complete. Startup type is managed by the executable (AUTO_START in code)."
    }

    'Uninstall' {
        Write-Host "Uninstalling service using: $exe"
        & $exe uninstall
        if ($LASTEXITCODE -ne 0) {
            throw "Uninstall command failed with exit code $LASTEXITCODE"
        }

        $service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
        if ($null -eq $service) {
            Write-Host "Service '$ServiceName' has been removed."
        } else {
            Write-Host "Service '$ServiceName' still exists. Check service manager permissions/logs."
        }
    }
}
