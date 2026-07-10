# Mata qualquer tray/daemon do Clawdmeter e sobe um limpo.
# Chamado por reset-daemon.bat, ou direto: powershell -ExecutionPolicy Bypass -File .\reset-daemon.ps1
#
# NOTA: mantido em ASCII puro. Windows PowerShell 5.1 le UTF-8 sem BOM como
# cp1252 e caracteres tipo ->, "--" (em dash) etc. bagunca o parser.

$ErrorActionPreference = 'Continue'

$tray = Join-Path $PSScriptRoot "daemon\tray_windows.py"
if (-not (Test-Path $tray)) {
    Write-Host "Nao achei $tray" -ForegroundColor Red
    exit 1
}

# Localiza pythonw. Ordem: env var, candidatos conhecidos, PATH (fallback).
# PATH pode apontar pra pythonw de outro venv (ex: hermes-agent) que nao tem
# bleak/pystray/Pillow instalado; o tray sobe, quebra no import, e morre
# silenciosamente. Prefer o pythoncore-global com as deps corretas.
$pythonw = $env:CLAWDMETER_PYTHONW
if (-not $pythonw) {
    $candidates = @(
        (Join-Path $env:LOCALAPPDATA "Python\pythoncore-3.14-64\pythonw.exe"),
        (Join-Path $env:LOCALAPPDATA "Python\pythoncore-3.13-64\pythonw.exe"),
        (Join-Path $env:LOCALAPPDATA "Programs\Python\Python312\pythonw.exe"),
        (Join-Path $env:LOCALAPPDATA "Programs\Python\Python311\pythonw.exe")
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $pythonw = $c; break }
    }
}
if (-not $pythonw) {
    $cmd = Get-Command pythonw -ErrorAction SilentlyContinue
    if ($cmd) {
        $pythonw = $cmd.Path
        Write-Host "Aviso: usando pythonw do PATH ($pythonw) - pode nao ter as deps do tray." -ForegroundColor Yellow
    }
}
if (-not $pythonw -or -not (Test-Path $pythonw)) {
    Write-Host "pythonw.exe nao encontrado. Defina `$env:CLAWDMETER_PYTHONW ou coloque no PATH." -ForegroundColor Red
    exit 1
}

Write-Host "Matando tray/daemon existentes..." -ForegroundColor Cyan
$victims = Get-CimInstance Win32_Process |
    Where-Object { $_.CommandLine -match 'tray_windows|claude_usage_daemon_windows' }
if ($victims) {
    foreach ($v in $victims) {
        Write-Host "  kill PID $($v.ProcessId)"
        try {
            Stop-Process -Id $v.ProcessId -Force -ErrorAction Stop
        } catch {
            Write-Host "  (falhou killar $($v.ProcessId): $($_.Exception.Message))" -ForegroundColor Yellow
        }
    }
    Start-Sleep -Milliseconds 700
} else {
    Write-Host "  (nada rodando)"
}

Write-Host "Subindo tray novo..." -ForegroundColor Cyan
Write-Host "  pythonw: $pythonw"
Write-Host "  script : $tray"

try {
    $proc = Start-Process -FilePath $pythonw -ArgumentList "`"$tray`"" -WindowStyle Hidden -PassThru -ErrorAction Stop
    Write-Host "  Start-Process retornou PID $($proc.Id)"
} catch {
    Write-Host "Start-Process falhou: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

Start-Sleep -Seconds 2

$p = Get-CimInstance Win32_Process |
     Where-Object { $_.CommandLine -match 'tray_windows' } |
     Select-Object -First 1
if ($p) {
    Write-Host "Daemon reiniciado. PID: $($p.ProcessId)" -ForegroundColor Green
} else {
    Write-Host "Tray subiu mas ja morreu. Rode manualmente pra ver o erro:" -ForegroundColor Red
    Write-Host ("  {0} {1}" -f $pythonw, $tray) -ForegroundColor Yellow
    exit 1
}
