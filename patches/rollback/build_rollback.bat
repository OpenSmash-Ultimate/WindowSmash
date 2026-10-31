@echo off
rem Links build\ssbu_rollback.exe (the rollback research build) beside build\ssbu.exe and
rem build\ssbu_nvfix.exe. runtime\ is not modified: the sources are assembled in build\rb_root\runtime, main in build\main_rb.
rem See patches\rollback\README.md.
setlocal
cd /d "%~dp0..\.."
call tools\find_python.bat || exit /b 1
%PY% patches\rollback\build_rollback.py
set "result=%ERRORLEVEL%"
if "%result%"=="0" echo Done: build\ssbu_rollback.exe. Start it with launchers\Play SSBU Offline (rollback).bat.
exit /b %result%
