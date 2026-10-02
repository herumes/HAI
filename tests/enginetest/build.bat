@echo off
rem Builds and runs the build-engine tests: the real prefab.cpp and
rem buildsession.cpp against stand-ins for the game (stubs.cpp). No Hytale
rem needed. Everything it writes goes to %TEMP%\hyai_enginetest.
setlocal
set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (echo vcvars64.bat not found at "%VCVARS%" & exit /b 1)
call "%VCVARS%" >nul
set OUT=%TEMP%\hyai_enginetest_build
if not exist "%OUT%" mkdir "%OUT%"
set ROOT=%~dp0..\..
cl /nologo /std:c++20 /EHsc /O1 /W3 /MD /DHYAI_ENGINE_TEST /I"%ROOT%" ^
   "%~dp0test_main.cpp" "%~dp0stubs.cpp" "%ROOT%\prefab.cpp" "%ROOT%\buildsession.cpp" ^
   /Fo"%OUT%\\" /Fe"%OUT%\enginetest.exe" /link user32.lib winhttp.lib psapi.lib
if errorlevel 1 exit /b 1
"%OUT%\enginetest.exe"
