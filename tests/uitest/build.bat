@echo off
rem Renders the chat UI offscreen with synthetic input and saves screenshots.
rem Needs the Release DLL built first (it links those object files, minus
rem dllmain.obj). Output goes to %TEMP%\hyai_uitest.
setlocal
set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (echo vcvars64.bat not found at "%VCVARS%" & exit /b 1)
call "%VCVARS%" >nul
set ROOT=%~dp0..\..
set OBJ=%ROOT%\HytaleChatLogger\x64\Release
set INC=%ROOT%\vcpkg_installed\x64-windows-static\x64-windows-static
set OUT=%TEMP%\hyai_uitest
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJ%\chatview.obj" (echo Build the Release DLL first. & exit /b 1)
cl /nologo /c /std:c++20 /EHsc /MT /O2 /GL /DNDEBUG /D_WINDOWS /DUNICODE /D_UNICODE /I"%ROOT%" /I"%INC%\include" ^
   "%~dp0ui_test.cpp" /Fo"%OUT%\ui_test.obj" || exit /b 1
set OBJS=
for %%f in ("%OBJ%\*.obj") do if /I not "%%~nxf"=="dllmain.obj" call set OBJS=%%OBJS%% "%%f"
link /nologo /LTCG /SUBSYSTEM:CONSOLE /OUT:"%OUT%\ui_test.exe" "%OUT%\ui_test.obj" %OBJS% ^
   "%INC%\lib\imgui.lib" "%INC%\lib\minhook.x64.lib" d3d11.lib dxgi.lib user32.lib gdi32.lib shell32.lib ^
   ole32.lib comdlg32.lib winmm.lib winhttp.lib psapi.lib bcrypt.lib crypt32.lib advapi32.lib dwmapi.lib imm32.lib ws2_32.lib || exit /b 1
"%OUT%\ui_test.exe" "%OUT%" "%~dp0..\.."
echo Screenshots: %OUT%
