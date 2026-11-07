@echo off
rem Run on a Windows computer that reaches the device over SSH: switches the
rem device's screen back to the web interface and removes what this interface
rem left on it.
rem Usage: uninstall.bat [address|user@host] asks for the device when not given
setlocal
set "here=%~dp0"
where ssh >nul 2>nul || (
    echo ssh not found: add the OpenSSH client in Settings, Optional features
    goto failed
)

rem Enter alone takes the usual address; the device signs in as root, so an
rem address alone gets that user.
set "target=%~1"
if "%target%"=="" set /p "target=Device address [192.168.9.1]: "
if "%target%"=="" set "target=192.168.9.1"
if "%target:@=%"=="%target%" set "target=root@%target%"

rem Without a key, ssh asks for the password once per step.
echo Step 1/2: switching %target% to the web interface. If ssh asks, enter the device's password.
ssh "%target%" "sh -s webkit" < "%here%switch-ui.sh" || goto failed
echo Step 2/2: removing this interface. If ssh asks, enter the password again.
ssh "%target%" "sh -s" < "%here%remove-ui.sh" || goto failed
echo done: uninstalled from %target%
set "status=0"
goto finish

:failed
echo failed: see the messages above
set "status=1"

rem A double-clicked window would close before its messages could be read.
:finish
pause
exit /b %status%
