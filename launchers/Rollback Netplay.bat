@echo off
rem [rollback research build: build\ssbu_rollback.exe - see patches\rollback\README.md and TWO_PC_SETUP.md]
rem Opens the Host / Join window (rollback_netplay.ps1, with buttons; no command prompt). "Rollback Netplay (command
rem prompt).bat" does the same in text, for tests.
start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0rollback_netplay.ps1"
