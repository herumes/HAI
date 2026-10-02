@echo off
rem Builds and runs the AI text tests: prompt variables, preset stack, chat context,
rem reply clean-up, tool rules, rate limiting and the calculator / dice tools.
rem Plain C++ (aitext.cpp and aicalc.cpp have no game or ImGui dependencies).
setlocal
set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (echo vcvars64.bat not found at "%VCVARS%" & exit /b 1)
call "%VCVARS%" >nul
set ROOT=%~dp0..\..
set OUT=%TEMP%\hyai_aitest_build
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++20 /EHsc /O2 /W3 /MT /DNDEBUG /DHYAI_STANDALONE /utf-8 /I"%ROOT%" ^
   "%~dp0ai_test.cpp" "%ROOT%\aitext.cpp" "%ROOT%\aicalc.cpp" ^
   /Fo"%OUT%\\" /Fe"%OUT%\aitest.exe"
if errorlevel 1 exit /b 1
"%OUT%\aitest.exe"
