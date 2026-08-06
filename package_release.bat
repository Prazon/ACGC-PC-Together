@echo off
setlocal
call "%~dp0build_pc.bat"
if errorlevel 1 exit /b %errorlevel%
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\package_windows.ps1" %*
exit /b %errorlevel%
