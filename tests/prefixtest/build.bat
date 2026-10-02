@echo off
rem Builds and runs the chat-prefix tests: the real chatprefix.cpp on the chat
rem lines from the server this was written for, plus a fuzz pass. No Hytale needed.
setlocal
set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (echo vcvars64.bat not found at "%VCVARS%" & exit /b 1)
call "%VCVARS%" >nul
set ROOT=%~dp0..\..
set INC=%ROOT%\vcpkg_installed\x64-windows-static\x64-windows-static
set OUT=%TEMP%\hyai_prefixtest_build
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++20 /EHsc /O2 /W3 /MT /DNDEBUG /D_WINDOWS /DUNICODE /D_UNICODE /utf-8 /I"%ROOT%" /I"%INC%\include" ^
   "%~dp0prefix_test.cpp" "%ROOT%\chatprefix.cpp" ^
   /Fo"%OUT%\\" /Fe"%OUT%\prefixtest.exe" /link user32.lib
if errorlevel 1 exit /b 1
"%OUT%\prefixtest.exe"
