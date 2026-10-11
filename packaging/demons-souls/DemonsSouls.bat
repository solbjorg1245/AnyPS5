@echo off
rem Demon's Souls launcher for AnyPS5 (Windows). First start: DemonsSouls.bat -Dump "D:\Games\PPSA01341-app0"
rem Later starts: DemonsSouls.bat. See README.md.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0DemonsSouls.ps1" %*
exit /b %ERRORLEVEL%
