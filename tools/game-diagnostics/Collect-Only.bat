@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Collect-Diagnostics.ps1"
if errorlevel 1 echo Report collection failed. Check the error above.
pause
