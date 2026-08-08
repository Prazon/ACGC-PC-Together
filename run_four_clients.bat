@echo off
setlocal

rem Launch the dedicated server and four visible Windows clients.
rem Expected layout: pc\build64\manual-two-client-test\
rem Clients 3 and 4 are staged automatically from client1 on first run,
rem each with its OWN empty save folder so new characters roll a fresh
rem random appearance. Their exe + SDL2.dll are re-synced from client1
rem every run, so restaging client1 after a build covers them too.
rem
rem Usage: run_four_clients.bat [fresh]
rem   fresh  - wipe client3/client4 saves before launch to re-roll
rem            appearances (never touches client1/client2 saves).

set "SESSION=%~dp0pc\build64\manual-two-client-test"
set "SERVER=%SESSION%\AnimalCrossingServer.exe"
set "CLIENT1=%SESSION%\client1\AnimalCrossing.exe"
set "CLIENT2=%SESSION%\client2\AnimalCrossing.exe"
set "CLIENT3=%SESSION%\client3\AnimalCrossing.exe"
set "CLIENT4=%SESSION%\client4\AnimalCrossing.exe"
set "CONFIG=%SESSION%\server.ini"
set "PORT=24680"
set "INVITE_KEY=local-two-client-test"

if not exist "%SERVER%" goto :missing
if not exist "%CLIENT1%" goto :missing
if not exist "%CLIENT2%" goto :missing
if not exist "%CONFIG%" goto :missing

call :stage "%SESSION%\client3"
call :stage "%SESSION%\client4"

if /i "%~1"=="fresh" (
    echo Wiping client3/client4 saves for a fresh appearance roll...
    if exist "%SESSION%\client3\save" rmdir /s /q "%SESSION%\client3\save"
    if exist "%SESSION%\client4\save" rmdir /s /q "%SESSION%\client4\save"
    mkdir "%SESSION%\client3\save"
    mkdir "%SESSION%\client4\save"
)

echo Starting dedicated server on port %PORT%...
start "ACGC Dedicated Server" /D "%SESSION%" "%SERVER%" --config server.ini --port %PORT%

rem Give the server time to bind its UDP socket before the clients connect.
timeout /t 2 /nobreak >nul

echo Starting patterned-player client (account 1001)...
start "ACGC Pattern Client" /D "%SESSION%\client1" "%CLIENT1%" --verbose --online 127.0.0.1:%PORT% --town 1 --account 1001 --invite-key %INVITE_KEY% --quickstart Pattern --quickstart-gender male

timeout /t 1 /nobreak >nul

echo Starting observer client (account 1002)...
start "ACGC Observer Client" /D "%SESSION%\client2" "%CLIENT2%" --verbose --online 127.0.0.1:%PORT% --town 1 --account 1002 --invite-key %INVITE_KEY% --quickstart Observer --quickstart-gender female

timeout /t 1 /nobreak >nul

echo Starting third client (account 1003, own save)...
start "ACGC Client 3" /D "%SESSION%\client3" "%CLIENT3%" --verbose --online 127.0.0.1:%PORT% --town 1 --account 1003 --invite-key %INVITE_KEY% --quickstart Rando3 --quickstart-gender male

timeout /t 1 /nobreak >nul

echo Starting fourth client (account 1004, own save)...
start "ACGC Client 4" /D "%SESSION%\client4" "%CLIENT4%" --verbose --online 127.0.0.1:%PORT% --town 1 --account 1004 --invite-key %INVITE_KEY% --quickstart Rando4 --quickstart-gender female

echo.
echo Five windows have been launched. Close the server window when finished.
exit /b 0

:stage
rem Stage a client folder from client1. Binaries are always re-synced;
rem configs, rom, shaders are copied only once; save stays untouched.
set "DST=%~1"
if not exist "%DST%" mkdir "%DST%"
copy /y "%SESSION%\client1\AnimalCrossing.exe" "%DST%\" >nul
copy /y "%SESSION%\client1\SDL2.dll" "%DST%\" >nul
if not exist "%DST%\keybindings.ini" copy "%SESSION%\client1\keybindings.ini" "%DST%\" >nul
if not exist "%DST%\network.ini" copy "%SESSION%\client1\network.ini" "%DST%\" >nul
if not exist "%DST%\settings.ini" copy "%SESSION%\client1\settings.ini" "%DST%\" >nul
if not exist "%DST%\rom" robocopy "%SESSION%\client1\rom" "%DST%\rom" /e >nul
if not exist "%DST%\shaders" robocopy "%SESSION%\client1\shaders" "%DST%\shaders" /e >nul
if not exist "%DST%\texture_pack" robocopy "%SESSION%\client1\texture_pack" "%DST%\texture_pack" /e >nul
if not exist "%DST%\save" mkdir "%DST%\save"
exit /b 0
