@echo off
set /p "RIFT_HOST=Host IP address (127.0.0.1 for this PC): "
if not defined RIFT_HOST set RIFT_HOST=127.0.0.1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Scripts\Play.ps1" -Mode Join -Address "%RIFT_HOST%"
