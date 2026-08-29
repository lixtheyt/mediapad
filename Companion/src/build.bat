@echo off
REM Builds a single portable mediapad_tray.exe. Nothing to install afterwards.
REM Requires: pip install pystray pillow winrt-runtime winrt-Windows.Media winrt-Windows.Media.Control pyinstaller
setlocal
cd /d "%~dp0"

pyinstaller --onefile --windowed --clean --name mediapad_tray ^
  --collect-all winrt ^
  mediapad_tray.py

echo.
echo Done. The portable exe is: %~dp0dist\mediapad_tray.exe
endlocal
