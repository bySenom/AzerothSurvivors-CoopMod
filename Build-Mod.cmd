@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build-Mod.ps1" %*
pause
