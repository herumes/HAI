@echo off
rem Builds and runs the auto-farm engine tests: the real farmroute.cpp driving a
rem simulated tractor round a simulated farm. No Hytale needed.
setlocal
set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (echo vcvars64.bat not found at "%VCVARS%" & exit /b 1)
call "%VCVARS%" >nul
set OUT=%TEMP%\hyai_farmtest_build
if not exist "%OUT%" mkdir "%OUT%"
set ROOT=%~dp0..\..
cl /nologo /std:c++20 /EHsc /O2 /W3 /MD /DHYAI_ENGINE_TEST /I"%ROOT%" ^
   "%~dp0farm_test.cpp" "%ROOT%\farmroute.cpp" ^
   /Fo"%OUT%\\" /Fe"%OUT%\farmtest.exe" /link user32.lib
if errorlevel 1 exit /b 1
"%OUT%\farmtest.exe"
