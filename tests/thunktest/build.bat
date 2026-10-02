@echo off
rem Builds and runs the hook-thunk tests: hookthunk.cpp called the way a
rem detoured game function would call it. No Hytale needed.
setlocal
set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (echo vcvars64.bat not found at "%VCVARS%" & exit /b 1)
call "%VCVARS%" >nul
set OUT=%TEMP%\hyai_thunktest_build
if not exist "%OUT%" mkdir "%OUT%"
set ROOT=%~dp0..\..
cl /nologo /std:c++20 /EHsc /O2 /W3 /MD /DHYAI_ENGINE_TEST /I"%ROOT%" ^
   "%~dp0thunk_test.cpp" "%ROOT%\hookthunk.cpp" ^
   /Fo"%OUT%\\" /Fe"%OUT%\thunktest.exe" /link user32.lib winhttp.lib psapi.lib
if errorlevel 1 exit /b 1
"%OUT%\thunktest.exe"
