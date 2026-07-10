@echo off
REM Wrapper clicavel do reset-daemon.ps1.
REM Dois cliques neste .bat, ou "reset-daemon" no CMD.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0reset-daemon.ps1"
echo.
pause
