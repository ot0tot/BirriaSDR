@echo off
setlocal
if not defined BIRRIA_PY set "BIRRIA_PY=python"
"%BIRRIA_PY%" "%~dp0integrations\standalone\app.py" %*
if errorlevel 1 pause
