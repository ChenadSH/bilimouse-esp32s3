@echo off
chcp 65001 >nul
set "MSYSTEM="
set "MSYS2_PATH_TYPE="
set "MSYS_NO_PATHCONV=1"
set "MINGW_PREFIX="
call "%~dp0env.bat"
set "ESP_ROM_ELF_DIR=C:/Espressif/tools/esp-rom-elfs/20241011"
if not defined IDF_PROJECT_DIR set "IDF_PROJECT_DIR=%~dp0.."
cd /d "%IDF_PROJECT_DIR%"
python "%IDF_PATH%\tools\idf.py" %*
exit /b %ERRORLEVEL%
