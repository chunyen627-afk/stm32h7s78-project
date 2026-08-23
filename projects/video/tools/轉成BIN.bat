@echo off
setlocal
set PYTHONIOENCODING=utf-8

rem ---------------------------------------------------------------
rem  ASCII-only on purpose.  cmd parses a .bat using whatever
rem  codepage is active at parse time, so Chinese put directly in
rem  here gets chopped in half.  All human-facing text is printed
rem  by the Python script instead.
rem ---------------------------------------------------------------

if not exist "%~dp0video2bin.py" (
  echo.
  echo   ERROR: video2bin.py not found.
  echo.
  echo   This .bat only looks next to itself, so video2bin.py and
  echo   this .bat must stay in the SAME folder.  Move them together
  echo   anywhere you like, just do not split them.
  echo.
  echo   This .bat is in: %~dp0
  echo.
  pause
  exit /b 1
)

if "%~1"=="" (
  python "%~dp0video2bin.py" --dropinfo
  echo.
  pause
  exit /b
)

python "%~dp0video2bin.py" %*

echo.
pause
