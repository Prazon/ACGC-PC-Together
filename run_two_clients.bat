@echo off
setlocal EnableDelayedExpansion

rem Launch the dedicated server and two visible Windows clients.
rem Self-assembling: builds/refreshes pc\build64\manual-two-client-test\ from
rem the normal build output (pc\build64\bin) on every run, so it stays
rem current after rebuilds. Disc images are hardlinked, not copied.

set "ROOT=%~dp0"
set "BIN=%ROOT%pc\build64\bin"
set "SESSION=%ROOT%pc\build64\manual-two-client-test"
set "PORT=24680"
set "INVITE_KEY=local-two-client-test"

if not exist "%BIN%\AnimalCrossing.exe" goto :notbuilt
if not exist "%BIN%\AnimalCrossingServer.exe" goto :notbuilt

rem --- assemble session layout ------------------------------------------
for %%D in ("%SESSION%" "%SESSION%\towns" "%SESSION%\client1\rom" "%SESSION%\client1\shaders" "%SESSION%\client1\save" "%SESSION%\client2\rom" "%SESSION%\client2\shaders" "%SESSION%\client2\save") do (
    if not exist %%D mkdir %%D
)

copy /Y "%BIN%\AnimalCrossingServer.exe" "%SESSION%\" >nul
copy /Y "%BIN%\libsqlite3-0.dll" "%SESSION%\" >nul

if not exist "%SESSION%\server.ini" (
    powershell -NoProfile -Command "(Get-Content -Raw '%ROOT%packaging\server.ini') -replace 'invite_key = \"\"', 'invite_key = \"%INVITE_KEY%\"' | Set-Content '%SESSION%\server.ini'"
)

rem First disc image in the main rom folder (any name, iso/gcm/ciso)
set "DISC="
set "DISCNAME="
for %%F in ("%BIN%\rom\*.iso" "%BIN%\rom\*.gcm" "%BIN%\rom\*.ciso") do (
    if not defined DISC (
        set "DISC=%%~fF"
        set "DISCNAME=%%~nxF"
    )
)

for %%C in (client1 client2) do (
    copy /Y "%BIN%\AnimalCrossing.exe" "%SESSION%\%%C\" >nul
    copy /Y "%BIN%\SDL2.dll" "%SESSION%\%%C\" >nul
    copy /Y "%BIN%\shaders\default.vert" "%SESSION%\%%C\shaders\" >nul
    copy /Y "%BIN%\shaders\default.frag" "%SESSION%\%%C\shaders\" >nul
    if exist "%BIN%\cores" (
        if not exist "%SESSION%\%%C\cores" mkdir "%SESSION%\%%C\cores"
        copy /Y "%BIN%\cores\*" "%SESSION%\%%C\cores\" >nul
    )
    if exist "%BIN%\settings.ini" if not exist "%SESSION%\%%C\settings.ini" copy "%BIN%\settings.ini" "%SESSION%\%%C\" >nul
    if exist "%BIN%\keybindings.ini" if not exist "%SESSION%\%%C\keybindings.ini" copy "%BIN%\keybindings.ini" "%SESSION%\%%C\" >nul

    rem Share the texture pack via directory junction (no copy) and seed the
    rem shader cache so first boot doesn't stall recompiling all variants.
    if exist "%BIN%\texture_pack" if not exist "%SESSION%\%%C\texture_pack" mklink /J "%SESSION%\%%C\texture_pack" "%BIN%\texture_pack" >nul
    if exist "%BIN%\shader_cache.bin" if not exist "%SESSION%\%%C\shader_cache.bin" copy "%BIN%\shader_cache.bin" "%SESSION%\%%C\" >nul

    rem Hardlink the disc image if this client has none yet (same volume, no copy)
    set "HAVE_DISC="
    for %%G in ("%SESSION%\%%C\rom\*.iso" "%SESSION%\%%C\rom\*.gcm" "%SESSION%\%%C\rom\*.ciso") do set "HAVE_DISC=1"
    if not defined HAVE_DISC (
        if defined DISC (
            mklink /H "%SESSION%\%%C\rom\!DISCNAME!" "!DISC!" >nul 2>&1
            if not exist "%SESSION%\%%C\rom\!DISCNAME!" copy "!DISC!" "%SESSION%\%%C\rom\" >nul
        )
    )
)

if not defined DISC (
    echo WARNING: no disc image found in %BIN%\rom - clients will not boot.
)

rem --- launch ------------------------------------------------------------
echo Starting dedicated server on port %PORT%...
start "ACGC Dedicated Server" /D "%SESSION%" "%SESSION%\AnimalCrossingServer.exe" --config server.ini --port %PORT%

rem Give the server time to bind its UDP socket before the clients connect.
timeout /t 2 /nobreak >nul

echo Starting patterned-player client (account 1001)...
start "ACGC Pattern Client" /D "%SESSION%\client1" "%SESSION%\client1\AnimalCrossing.exe" --verbose --online 127.0.0.1:%PORT% --town 1 --account 1001 --invite-key %INVITE_KEY% --quickstart Pattern --quickstart-gender male

echo Starting observer client (account 1002)...
start "ACGC Observer Client" /D "%SESSION%\client2" "%SESSION%\client2\AnimalCrossing.exe" --verbose --online 127.0.0.1:%PORT% --town 1 --account 1002 --invite-key %INVITE_KEY% --quickstart Observer --quickstart-gender female

echo.
echo Three windows have been launched. Close the server window when finished.
exit /b 0

:notbuilt
echo Build outputs not found under:
echo   %BIN%
echo Run build_pc.bat first, then run this file again.
pause
exit /b 1
