@echo off
rem Multivoid friend installer launcher: runs Install-Multivoid.ps1 with a one-off execution policy.
rem PSModulePath is cleared so a launch from PowerShell 7 does not hand Windows PowerShell 5.1 the wrong modules.
setlocal
set "PSModulePath="
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-Multivoid.ps1" %*
set "RC=%ERRORLEVEL%"
if "%~1"=="" pause
exit /b %RC%
