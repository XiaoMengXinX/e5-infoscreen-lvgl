@echo off
rem Run on a Windows computer that reaches the device over SSH: switches the
rem device's screen back to the web interface and removes what this interface
rem left on it.
rem Usage: uninstall.bat [user@host]        asks for the device when not given
setlocal
set "here=%~dp0"
where ssh >nul 2>nul || (
    echo ssh not found: add the OpenSSH client in Settings, Optional features
    exit /b 1
)

rem Enter alone takes the usual address.
set "target=%~1"
if "%target%"=="" set /p "target=SSH target [root@192.168.9.1]: "
if "%target%"=="" set "target=root@192.168.9.1"

rem Without a key, ssh asks for the password once per step.
echo Step 1/2: switching %target% to the web interface. If ssh asks, enter the device's password.
ssh "%target%" "sh -s webkit" < "%here%switch-ui.sh" || exit /b 1
echo Step 2/2: removing this interface. If ssh asks, enter the password again.
ssh "%target%" "sh -s" < "%here%remove-ui.sh" || exit /b 1
echo done: uninstalled from %target%
