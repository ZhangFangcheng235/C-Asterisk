@echo off
rem ============================================================
rem  build.bat -- builds the C* (C Asterisk) toolchain
rem  Needs: gcc + g++ in PATH (MinGW-w64, MSYS2, w64devkit or Dev-C++)
rem ============================================================
setlocal
cd /d "%~dp0"

where gcc >nul 2>&1
if errorlevel 1 (
    echo [build] ERROR: gcc was not found in PATH.
    echo         Install MinGW-w64 / MSYS2 / w64devkit / Dev-C++
    echo         and make sure its bin folder is in PATH.
    exit /b 1
)
where g++ >nul 2>&1
if errorlevel 1 (
    echo [build] ERROR: g++ was not found in PATH. It ships with the same
    echo         packages as gcc - please reinstall one of them.
    exit /b 1
)

if not exist bin   mkdir bin
if not exist build mkdir build

echo [build] compiler  -^> bin\caskc.exe
gcc -O2 -Wall -Wextra -o bin\caskc.exe compiler\caskc.c
if errorlevel 1 (
    echo [build] FAILED to build the compiler.
    exit /b 1
)

if exist editor\webide.c (
    echo [build] editor    -^> bin\caskide.exe
    rem  C host (window + local web server) and the C++ WebView2 glue,
    rem  then link with g++ so the C++ runtime comes along.
    gcc -O2 -Wall -c editor\webide.c -o build\webide.o
    if errorlevel 1 (
        echo [build] FAILED to build editor\webide.c.
        exit /b 1
    )
    g++ -O2 -Wall -c editor\wv2.cpp -I editor\webview2\include -o build\wv2.o
    if errorlevel 1 (
        echo [build] FAILED to build editor\wv2.cpp.
        exit /b 1
    )
    g++ -o bin\caskide.exe build\webide.o build\wv2.o -lws2_32 -lole32 -lcomdlg32 -lshell32 -ldwmapi -luuid -mwindows
    if errorlevel 1 (
        echo [build] FAILED to link the editor.
        exit /b 1
    )
    rem  the page the editor shows, and the loader WebView2 needs
    if not exist bin\web mkdir bin\web
    copy /y web\index.html bin\web\index.html >nul
    copy /y web\style.css  bin\web\style.css  >nul
    copy /y web\editor.js  bin\web\editor.js  >nul
    copy /y web\app.js     bin\web\app.js     >nul
    copy /y web\blocks.js  bin\web\blocks.js  >nul
    copy /y web\syntax.js bin\web\syntax.js >nul
    copy /y web\header-info.json bin\web\header-info.json >nul
    if exist editor\webview2\x64\WebView2Loader.dll (
        copy /y editor\webview2\x64\WebView2Loader.dll bin\WebView2Loader.dll >nul
    )
)

echo [build] runtime   -^> bin\cask_rt.h
copy /y compiler\cask_rt.h bin\cask_rt.h >nul
echo [build] header    -^> bin\C_OPED.cash
copy /y compiler\C_OPED.cash bin\C_OPED.cash >nul
echo [build] header    -^> bin\C_CSS.cash
copy /y compiler\C_CSS.cash bin\C_CSS.cash >nul
if exist build\C_OPED.cash del /q build\C_OPED.cash >nul
if exist build\C_CSS.cash del /q build\C_CSS.cash >nul
if exist examples\C_OPED.cash del /q examples\C_OPED.cash >nul
if exist examples\C_CSS.cash del /q examples\C_CSS.cash >nul

echo [build] done.  Try:  bin\caskc.exe examples\hello.cask --run
echo         or the editor:  bin\caskide.exe
endlocal
