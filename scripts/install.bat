@echo off
rem Run on a Windows computer that reaches the device over SSH, with the OpenSSH
rem client and tar that come with Windows 10 and later.
rem Usage: install.bat [user@host]          install this interface and switch to it
rem        install.bat [user@host] webkit   switch the device back to the web interface
rem The host defaults to root@192.168.8.1.
setlocal
set "here=%~dp0"
set "target=%~1"
if "%target%"=="" set "target=root@192.168.8.1"
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
for %%F in ("%binary%") do set "bindir=%%~dpF."

rem Both files travel as one tar stream through ssh: one connection, one password,
rem and the device has no SFTP server for scp.
echo uploading to %target%
tar -cf - -C "%bindir%" e5-infoscreen-ui -C "%here%." switch-ui.sh | ssh "%target%" "set -e; t=/tmp/e5-infoscreen-install; rm -rf $t; mkdir $t; tar -xf - -C $t; d=/usr/libexec/e5-infoscreen; cp $t/e5-infoscreen-ui $d/ui.new; mv $d/ui.new $d/ui; sh $t/switch-ui.sh lvgl; rm -rf $t" || exit /b 1

:done
echo done: %mode% interface on %target%
