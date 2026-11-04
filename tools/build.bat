@echo off
REM 用法: build.bat <project_dir> [idf.py args...]
call "%~dp0env.bat"
cd /d "%~1"
shift
python "%IDF_PATH%\tools\idf.py" %1 %2 %3 %4 %5 %6 %7 %8 %9
