@echo off
setlocal
if not exist server.ini (
  echo server.ini is missing. Restore it from the release package.
  exit /b 2
)
AnimalCrossingServer.exe --config server.ini
if errorlevel 1 (
  echo.
  echo The server stopped with an error. Review the message above and server.ini.
  echo If invite_required is true, set invite_key in server.ini or ACGC_INVITE_KEY.
  pause
)
endlocal
