@echo off
rem Run on a Windows computer that reaches the device over SSH, with the OpenSSH
rem client that comes with Windows 10 and later: installs this interface and
rem switches the device's screen to it. uninstall.bat switches back.
rem Usage: install.bat [address|user@host]   asks for the device when not given
setlocal
set "here=%~dp0"
where ssh >nul 2>nul || (
    echo ssh not found: add the OpenSSH client in Settings, Optional features
    goto failed
)

rem Next to this script in a release, under out\ in a source checkout.
set "binary=%here%e5-infoscreen-ui"
if not exist "%binary%" set "binary=%here%..\out\e5-infoscreen-ui"
if not exist "%binary%" (
    echo missing e5-infoscreen-ui: download a release or run python build.py
    goto failed
)

rem Enter alone takes the usual address; the device signs in as root, so an
rem address alone gets that user.
set "target=%~1"
if "%target%"=="" set /p "target=Device address [192.168.9.1]: "
if "%target%"=="" set "target=192.168.9.1"
if "%target:@=%"=="%target%" set "target=root@%target%"

rem The files go through ssh itself: the device has no SFTP server for scp. Without
rem a key, ssh asks for the password once per step.
echo Step 1/2: uploading the program to %target%. If ssh asks, enter the device's password.
ssh "%target%" "cat > /usr/libexec/e5-infoscreen/ui.new" < "%binary%" || goto failed
echo Step 2/2: installing it and restarting the screen. If ssh asks, enter the password again.
rem switch-ui stays on the device for the screens' own switches between interfaces.
ssh "%target%" "cd /usr/libexec/e5-infoscreen && cat > switch-ui.new && chmod 755 switch-ui.new && mv switch-ui.new switch-ui && mv ui.new ui && ./switch-ui lvgl" < "%here%switch-ui.sh" || goto failed
echo done: installed on %target%
set "status=0"
goto finish

:failed
echo failed: see the messages above
set "status=1"

rem A double-clicked window would close before its messages could be read.
:finish
pause
exit /b %status%
