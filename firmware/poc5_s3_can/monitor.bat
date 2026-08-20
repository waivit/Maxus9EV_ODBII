@echo off
REM Serial monitor for the Phase 3 native-CAN POC (Waveshare ESP32-S3-RS485-CAN).
REM
REM Usage:  monitor.bat COM10
REM Exit the monitor with Ctrl+]
REM
REM IDF_PYTHON_ENV_PATH is pinned on purpose: export.bat picks the Python it
REM finds on PATH (3.12 here) and then looks for an "idf5.3_py3.12_env" that
REM does not exist - ESP-IDF was installed with Python 3.11. Setting the path
REM explicitly skips that guess.

cd /d "%~dp0"

if "%~1"=="" (
    echo Usage: monitor.bat ^<PORT^>     e.g.  monitor.bat COM10
    exit /b 1
)

set IDF_PYTHON_ENV_PATH=C:\Users\Waivit\.espressif\python_env\idf5.3_py3.11_env
call C:\esp\esp-idf\export.bat
if errorlevel 1 (
    echo.
    echo ESP-IDF export failed - check that C:\esp\esp-idf exists.
    exit /b 1
)

idf.py -p %1 monitor
