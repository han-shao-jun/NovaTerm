@echo off
REM Build NovaTerm inside the MSVC environment (requires vcvars).
setlocal
set "VCVARS=C:\Programs\MicrosoftVisualStudio\18\Insiders\VC\Auxiliary\Build\vcvarsall.bat"
call "%VCVARS%" x64 >nul
if errorlevel 1 exit /b 1
cmake --build "E:\code\Qt\NovaTerm\build"
exit /b %errorlevel%
