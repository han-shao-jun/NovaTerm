@echo off
REM ============================================================================
REM  build-ssh-thirdparty.bat -- one-time build of OpenSSL only.
REM
REM  Artifacts (static linking, no extra DLLs to ship):
REM    third_party/openssl-3.5.7/install   <- OpenSSL 3.5.7 (no-asm pure-C static)
REM
REM  libssh 0.12.2 is no longer built here: NovaTerm's top-level CMakeLists.txt
REM  builds it directly as a source subdirectory (add_subdirectory). This script
REM  exists solely to produce the OpenSSL install that both the main project
REM  (find_package(OpenSSL)) and the libssh subdirectory rely on.
REM
REM  Prerequisites:
REM    - MSVC + Ninja (same toolchain as the NovaTerm build)
REM    - Strawberry Perl (required by the OpenSSL build)
REM
REM  Usage: build-ssh-thirdparty.bat   (idempotent; safe to re-run)
REM ============================================================================
setlocal

set "REPO=E:\code\Qt\NovaTerm"
set "VCVARS=C:\Programs\MicrosoftVisualStudio\18\Insiders\VC\Auxiliary\Build\vcvarsall.bat"
set "PERL_DIR=E:\app\strawberry-perl-5.38.2.2-64bit-portable\perl\bin"

set "OSSL_SRC=%REPO%\third_party\openssl-3.5.7"
set "OSSL_BUILD=%REPO%\third_party\openssl-3.5.7\build"
set "OSSL_PREFIX=%REPO%\third_party\openssl-3.5.7\install"

if not exist "%VCVARS%" (
    echo [ERROR] vcvarsall.bat not found: "%VCVARS%"
    exit /b 1
)
if not exist "%PERL_DIR%\perl.exe" (
    echo [ERROR] Strawberry Perl not found: "%PERL_DIR%\perl.exe"
    exit /b 1
)

call "%VCVARS%" x64 >nul
if errorlevel 1 (
    echo [ERROR] vcvarsall failed
    exit /b 1
)

set "PATH=%PERL_DIR%;%PATH%"

echo.
echo ======================================================================
echo  [1/1] Building OpenSSL 3.5.7 (static, no-asm)
echo ======================================================================
if not exist "%OSSL_BUILD%" mkdir "%OSSL_BUILD%"
cd /d "%OSSL_BUILD%"

perl "%OSSL_SRC%\Configure" VC-WIN64A no-asm no-shared no-tests --prefix="%OSSL_PREFIX%" --openssldir="%OSSL_PREFIX%\ssl" --libdir=lib
if errorlevel 1 goto :failed

nmake
if errorlevel 1 goto :failed

nmake install_sw
if errorlevel 1 goto :failed

echo.
echo ======================================================================
echo  SUCCESS - OpenSSL is ready.
echo    OpenSSL : %OSSL_PREFIX%
echo  Next: libssh is built automatically by the NovaTerm CMake configure.
echo ======================================================================
exit /b 0

:failed
echo.
echo [ERROR] Build failed at the step above.
exit /b 1
