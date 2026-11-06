@echo off
setlocal
echo Save your work first. This tool collects diagnostics and restarts Windows.
echo Close games and automation tools; disconnect capture/control hardware before testing.
echo No drivers or services will be disabled. No EDID or ACE settings will be changed.
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Collect-Diagnostics.ps1"
if errorlevel 1 goto failed
echo Windows will restart in 60 seconds. Press C to cancel, or R to restart now.
choice /c RC /n /t 60 /d R /m "[R] Restart  [C] Cancel: "
if errorlevel 2 goto cancelled
if errorlevel 1 goto restart
goto cancelled
:restart
rem A zero timeout without /f avoids forcing applications closed.
shutdown.exe /r /t 0
if errorlevel 1 goto failed
exit /b 0
:cancelled
echo Restart cancelled. Reports have been kept.
pause
exit /b 0
:failed
echo Operation failed. No further restart will be attempted.
pause
exit /b 1
