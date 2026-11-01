@echo off
rem Runs a command inside the MSVC x64 build environment with LLVM on PATH.
rem Usage: tools\vsenv.cmd <command> [args...]
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%ProgramFiles%\LLVM\bin;%PATH%"
%*
