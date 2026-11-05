@echo off
rem Run on a Windows computer that reaches the device over SSH, with the OpenSSH
rem client that comes with Windows 10 and later.
rem Usage: install.bat [user@host]          install this interface and switch to it
rem        install.bat [user@host] webkit   switch the device back to the web interface
rem The host defaults to root@192.168.9.1.
setlocal
set "here=%~dp0"
set "target=%~1"
if "%target%"=="" set "target=root@192.168.9.1"
set "mode=%~2"
if "%mode%"=="" set "mode=lvgl"
if not "%mode%"=="lvgl" if not "%mode%"=="webkit" (
    echo usage: %~nx0 [user@host] [lvgl ^| webkit]
    exit /b 1
)
where ssh >nul 2>nul || (
    echo ssh not found: add the OpenSSH client in Settings, Optional features
    exit /b 1
)

if "%mode%"=="webkit" (
    echo Switching %target% to the web interface. If ssh asks, enter the device's password.
    ssh "%target%" "sh -s webkit" < "%here%switch-ui.sh" || exit /b 1
    goto done
)

rem Next to this script in a release, under out\ in a source checkout.
set "binary=%here%e5-infoscreen-ui"
if not exist "%binary%" set "binary=%here%..\out\e5-infoscreen-ui"
if not exist "%binary%" (
    echo missing e5-infoscreen-ui: download a release or run python build.py
    exit /b 1
)

rem The files go through ssh itself: the device has no SFTP server for scp. Without
rem a key, ssh asks for the password once per step.
echo Step 1/2: uploading the program to %target%. If ssh asks, enter the device's password.
ssh "%target%" "cat > /usr/libexec/e5-infoscreen/ui.new" < "%binary%" || exit /b 1
echo Step 2/2: installing it and restarting the screen. If ssh asks, enter the password again.
ssh "%target%" "mv /usr/libexec/e5-infoscreen/ui.new /usr/libexec/e5-infoscreen/ui && sh -s lvgl" < "%here%switch-ui.sh" || exit /b 1

:done
echo done: %mode% interface on %target%
