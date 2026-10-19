@echo off
rem ============================================================
rem  run_tests.bat -- compiles every tests\*.cask and compares
rem  the program output with tests\*.expected (byte for byte)
rem
rem  conventions:
rem    tests\X.cask     the program
rem    tests\X.expected exact expected output
rem    tests\X.stdin    optional input piped to the program
rem    tests\rtcheck_*  must FAIL at run time (runtime check fired)
rem ============================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"
set /a pass=0
set /a fail=0
if not exist build mkdir build

for %%f in (tests\*.cask) do call :one "%%~nf"

rem ---- the C_OPED.cash window header (tests\native\*) ----
rem  built with --console: a window program hides the console, but the
rem  tests want to see print/show output.
if exist tests\native (
  for %%f in (tests\native\*.cask) do call :natone "%%~nf"
)

echo.
echo ==================================================
echo  passed: !pass!    failed: !fail!
echo ==================================================
if !fail! gtr 0 exit /b 1
exit /b 0

:natone
set "n=%~1"
echo !n!| findstr /b /c:"cerr_" >nul
if not errorlevel 1 goto natcerr
bin\caskc.exe "tests\native\!n!.cask" --console -o "build\nat_!n!.exe" >nul 2>"build\nat_!n!.cerr"
if errorlevel 1 goto natbad
cmd /c ""build\nat_!n!.exe" >"build\nat_!n!.out" 2>&1"
fc /b "build\nat_!n!.out" "tests\native\!n!.expected" >nul 2>&1
if errorlevel 1 goto natoutdiff
echo [ok]   native\!n!
set /a pass+=1
goto :eof
:natbad
echo [FAIL] native\!n!  - could not compile:
type "build\nat_!n!.cerr"
set /a fail+=1
goto :eof
:natoutdiff
echo [FAIL] native\!n!  - output differs:
fc "build\nat_!n!.out" "tests\native\!n!.expected"
set /a fail+=1
goto :eof

:natcerr
bin\caskc.exe "tests\native\!n!.cask" --console -o "build\nat_!n!.exe" >nul 2>"build\nat_!n!.cerr"
fc /b "build\nat_!n!.cerr" "tests\native\!n!.expected" >nul 2>&1
if errorlevel 1 goto natcerrdiff
echo [ok]   native\!n!
set /a pass+=1
goto :eof
:natcerrdiff
echo [FAIL] native\!n!  - the compiler's message differs:
fc "build\nat_!n!.cerr" "tests\native\!n!.expected"
set /a fail+=1
goto :eof

:one
set "n=%~1"
if not exist "tests\!n!.expected" goto skip
echo !n!| findstr /b /c:"cerr_" >nul
if not errorlevel 1 goto wantcerr
bin\caskc.exe "tests\!n!.cask" -o "build\!n!.exe" >nul 2>"build\!n!.cerr"
if errorlevel 1 goto cverr
if exist "tests\!n!.in" goto runin
cmd /c ""build\!n!.exe" >"build\!n!.out" 2>&1"
goto ran
:runin
cmd /c ""build\!n!.exe" <"tests\!n!.in" >"build\!n!.out" 2>&1"
:ran
set "rc=!errorlevel!"
fc /b "build\!n!.out" "tests\!n!.expected" >nul 2>&1
if errorlevel 1 goto outdiff
set "wantfail="
echo !n!| findstr /b /c:"rtcheck_" >nul
if errorlevel 1 goto shouldsucceed
set "wantfail=1"
goto checkfail
:shouldsucceed
if not "!rc!"=="0" goto badrc
goto ok
:checkfail
if "!rc!"=="0" goto expectedfail
goto ok
:ok
echo [ok]   !n!
set /a pass+=1
goto :eof

:skip
echo [skip] !n!  - no expected file
set /a fail+=1
goto :eof
:cverr
echo [FAIL] !n!  - could not compile:
type "build\!n!.cerr"
set /a fail+=1
goto :eof
:outdiff
echo [FAIL] !n!  - output differs:
fc "build\!n!.out" "tests\!n!.expected"
set /a fail+=1
goto :eof
:badrc
echo [FAIL] !n!  - exit code !rc! was not 0
set /a fail+=1
goto :eof
:expectedfail
echo [FAIL] !n!  - a runtime error was expected, but the program succeeded
set /a fail+=1
goto :eof

rem ---- cerr_*: the compiler itself must reject the program ----
:wantcerr
bin\caskc.exe "tests\!n!.cask" -o "build\!n!.exe" >nul 2>"build\!n!.cerr"
if not errorlevel 1 goto cerrnone
fc /b "build\!n!.cerr" "tests\!n!.expected" >nul 2>&1
if errorlevel 1 goto cerrdiff
echo [ok]   !n!
set /a pass+=1
goto :eof
:cerrnone
echo [FAIL] !n!  - the compiler accepted a program it should reject
set /a fail+=1
goto :eof
:cerrdiff
echo [FAIL] !n!  - the compiler's message differs:
fc "build\!n!.cerr" "tests\!n!.expected"
set /a fail+=1
goto :eof
