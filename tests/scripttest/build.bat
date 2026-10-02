@echo off
rem Builds and runs the build-script tests (buildscript.cpp, plus the real
rem prefab.cpp to prove its parser reads what is written) and the command line
rem converter bs_cli.exe. No Hytale needed. Files go to %TEMP%\hyai_scripttest.
rem
rem   bs_cli.exe <script-or-reply> --names ..\..\x64\Release\block_ids.txt --layers
setlocal
set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (echo vcvars64.bat not found at "%VCVARS%" & exit /b 1)
call "%VCVARS%" >nul
set OUT=%TEMP%\hyai_scripttest_build
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\cli" mkdir "%OUT%\cli"
set ROOT=%~dp0..\..
cl /nologo /std:c++20 /EHsc /O1 /W3 /MD /DHYAI_ENGINE_TEST /utf-8 /I"%ROOT%" ^
   "%~dp0script_test.cpp" "%ROOT%\buildscript.cpp" "%ROOT%\prefab.cpp" "%~dp0..\enginetest\stubs.cpp" ^
   /Fo"%OUT%\\" /Fe"%OUT%\scripttest.exe" /link user32.lib winhttp.lib psapi.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O1 /W3 /MD /DHYAI_ENGINE_TEST /utf-8 /I"%ROOT%" ^
   "%~dp0bs_cli.cpp" "%ROOT%\buildscript.cpp" ^
   /Fo"%OUT%\cli\\" /Fe"%OUT%\bs_cli.exe" /link user32.lib
if errorlevel 1 exit /b 1
pushd "%ROOT%"
"%OUT%\scripttest.exe"
set RC=%ERRORLEVEL%
popd
exit /b %RC%
