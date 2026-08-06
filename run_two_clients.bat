@echo off
setlocal

rem Launch the dedicated server and two visible Windows clients.
rem Expected layout: pc\build64\manual-two-client-test\

set "SESSION=%~dp0pc\build64\manual-two-client-test"
set "SERVER=%SESSION%\AnimalCrossingServer.exe"
set "CLIENT1=%SESSION%\client1\AnimalCrossing.exe"
set "CLIENT2=%SESSION%\client2\AnimalCrossing.exe"
set "CONFIG=%SESSION%\server.ini"
set "PORT=24680"
set "INVITE_KEY=local-two-client-test"

if not exist "%SERVER%" goto :missing
if not exist "%CLIENT1%" goto :missing
if not exist "%CLIENT2%" goto :missing
if not exist "%CONFIG%" goto :missing

echo Starting dedicated server on port %PORT%...
start "ACGC Dedicated Server" /D "%SESSION%" "%SERVER%" --config server.ini --port %PORT%

rem Give the server time to bind its UDP socket before the clients connect.
timeout /t 2 /nobreak >nul

echo Starting patterned-player client (account 1001)...
start "ACGC Pattern Client" /D "%SESSION%\client1" "%CLIENT1%" --verbose --online 127.0.0.1:%PORT% --town 1 --account 1001 --invite-key %INVITE_KEY% --quickstart Pattern --quickstart-gender male

echo Starting observer client (account 1002)...
start "ACGC Observer Client" /D "%SESSION%\client2" "%CLIENT2%" --verbose --online 127.0.0.1:%PORT% --town 1 --account 1002 --invite-key %INVITE_KEY% --quickstart Observer --quickstart-gender female

echo.
echo Three windows have been launched. Close the server window when finished.
exit /b 0

:missing
echo Missing launcher prerequisite under:
echo   %SESSION%
echo Build the Windows client/server first, then run this file again.
pause
exit /b 1
