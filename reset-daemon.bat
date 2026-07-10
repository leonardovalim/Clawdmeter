@echo off
REM Mata qualquer tray/daemon do Clawdmeter e sobe um limpo.
REM Uso: 2 cliques ou "reset-daemon.bat" no CMD.
REM Toda a logica na linha unica abaixo — mistura de escape .bat + PowerShell,
REM que odeia continuacoes com ^ dentro de -Command.

powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$tray = '%~dp0daemon\tray_windows.py'; if (-not (Test-Path $tray)) { Write-Host ('Nao achei ' + $tray) -ForegroundColor Red; exit 1 }; $pw = $env:CLAWDMETER_PYTHONW; if (-not $pw) { $c = Get-Command pythonw -EA SilentlyContinue; if ($c) { $pw = $c.Path } }; if (-not $pw) { foreach ($cand in @(\"$env:LOCALAPPDATA\Python\pythoncore-3.14-64\pythonw.exe\", \"$env:LOCALAPPDATA\Python\pythoncore-3.13-64\pythonw.exe\", \"$env:LOCALAPPDATA\Programs\Python\Python312\pythonw.exe\")) { if (Test-Path $cand) { $pw = $cand; break } } }; if (-not $pw -or -not (Test-Path $pw)) { Write-Host 'pythonw nao encontrado' -ForegroundColor Red; exit 1 }; Write-Host 'Matando processos existentes...' -ForegroundColor Cyan; $v = Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match 'tray_windows|claude_usage_daemon_windows' }; if ($v) { $v | ForEach-Object { Write-Host ('  kill PID ' + $_.ProcessId); Stop-Process -Id $_.ProcessId -Force -EA SilentlyContinue }; Start-Sleep -Milliseconds 700 } else { Write-Host '  (nada rodando)' }; Write-Host 'Subindo tray novo...' -ForegroundColor Cyan; Start-Process -FilePath $pw -ArgumentList ('\"' + $tray + '\"') -WindowStyle Hidden; Start-Sleep -Seconds 2; $p = Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match 'tray_windows' } | Select-Object -First 1; if ($p) { Write-Host ('Daemon reiniciado. PID: ' + $p.ProcessId) -ForegroundColor Green } else { Write-Host 'Falhou subir daemon' -ForegroundColor Red; exit 1 }"

echo.
pause
