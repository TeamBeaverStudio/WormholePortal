@echo off
setlocal
cd /d "%~dp0"

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\PreviewDocs.ps1"

if errorlevel 1 (
  echo.
  echo The documentation preview could not be started.
  echo See the error above, then press any key to close this window.
  pause >nul
)
