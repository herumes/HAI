@echo off
rem End-to-end test of the AI flow: the real ai.cpp / aiflow.cpp / aicontext.cpp talking to a fake
rem Ollama server on localhost. Covers auto send, review (approve / edit / retry / discard),
rem tool questions, limits, dry run, clean-up and the prompt stack. No Hytale, no model needed.
rem Needs the Release object files first (build the DLL project once; a failed link is fine).
setlocal
set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (echo vcvars64.bat not found at "%VCVARS%" & exit /b 1)
call "%VCVARS%" >nul
set ROOT=%~dp0..\..
set OBJ=%ROOT%\HytaleChatLogger\x64\Release
set INC=%ROOT%\vcpkg_installed\x64-windows-static\x64-windows-static
set OUT=%TEMP%\hyai_flowtest
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJ%\aiflow.obj" (echo Build the Release DLL project first. & exit /b 1)
cl /nologo /c /std:c++20 /EHsc /MT /O2 /GL /DNDEBUG /D_WINDOWS /DUNICODE /D_UNICODE /I"%ROOT%" /I"%INC%\include" ^
   "%~dp0flow_test.cpp" /Fo"%OUT%\flow_test.obj" || exit /b 1
set OBJS=
for %%f in ("%OBJ%\*.obj") do if /I not "%%~nxf"=="dllmain.obj" call set OBJS=%%OBJS%% "%%f"
link /nologo /LTCG /SUBSYSTEM:CONSOLE /OUT:"%OUT%\flow_test.exe" "%OUT%\flow_test.obj" %OBJS% ^
   "%INC%\lib\imgui.lib" "%INC%\lib\minhook.x64.lib" d3d11.lib dxgi.lib user32.lib gdi32.lib shell32.lib ^
   ole32.lib comdlg32.lib winmm.lib winhttp.lib psapi.lib bcrypt.lib crypt32.lib advapi32.lib dwmapi.lib imm32.lib ws2_32.lib || exit /b 1
"%OUT%\flow_test.exe"
