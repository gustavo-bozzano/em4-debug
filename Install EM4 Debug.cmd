@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0Install-EM4Debug.ps1"
if errorlevel 1 pause
