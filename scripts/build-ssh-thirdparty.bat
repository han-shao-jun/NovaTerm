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
REM    - jom (optional but strongly recommended): Qt's parallel nmake clone.
REM      Auto-detected under Qt installs (e.g. <Qt>\Tools\QtCreator\bin\jom).
REM      Override with JOM_EXE, or add jom.exe to PATH.
REM
REM  Parallel build notes:
REM    - The old CL=/MP trick does NOT parallelize OpenSSL's nmake build:
REM      nmake runs targets one at a time, and OpenSSL's makefile invokes
REM      cl.exe once per source file, so /MP (which only helps when a single
REM      cl command compiles several files) has no effect.
REM    - The real fix is jom (make -j equivalent). OpenSSL's own CI uses jom
REM      for parallel Windows builds. Falls back to sequential nmake if jom
REM      is not found.
REM    - CL=/FS is required for parallel builds: every cl.exe writes debug
REM      info into the same ossl_static.pdb, and concurrent writers fail
REM      with C1041 unless /FS serializes PDB writes.
REM    - no-makedepend: recommended by NOTES-WINDOWS.md for one-shot builds
REM      (skips per-TU dependency scanning; can be up to 50% faster). Safe here
REM      because we never edit the third-party sources, so no incremental
REM      header-dependency tracking is needed.
REM
REM  Usage: build-ssh-thirdparty.bat   (idempotent; safe to re-run)
REM ============================================================================
setlocal EnableExtensions

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

REM ============================================================================
REM  Parallel compile setup (real parallelism via jom)
REM  - Use the built-in NUMBER_OF_PROCESSORS env var (always set on Windows NT)
REM  - Cap max parallelism to avoid memory pressure (OpenSSL TUs can be large)
REM  - Override the job count with BUILD_JOBS if desired
REM ============================================================================
set "CPU_CORES=%NUMBER_OF_PROCESSORS%"
if not defined CPU_CORES set "CPU_CORES=4"
if defined BUILD_JOBS set "CPU_CORES=%BUILD_JOBS%"
if %CPU_CORES% GTR 16 set "CPU_CORES=16"
echo  [INFO] CPU logical cores: %CPU_CORES%

REM --- PDB safety for parallel compiles ---
REM Multiple cl.exe processes (jom -jN) write to the same ossl_static.pdb.
REM Without /FS, concurrent PDB access fails with C1041. /FS serializes PDB
REM writes through mspdbsrv, making the parallel build reliable.
if defined CL (
    set "CL=%CL% /FS"
) else (
    set "CL=/FS"
)
echo  [INFO] CL=%CL%

REM --- Locate jom (parallel nmake); leave JOM empty to fall back to nmake ---
set "JOM="
if defined JOM_EXE if exist "%JOM_EXE%" set "JOM=%JOM_EXE%"
if not defined JOM (
    where jom >nul 2>nul && set "JOM=jom"
)
if not defined JOM (
    for %%Q in ("%QTDIR%" "C:\Qt" "D:\Qt" "E:\Qt" "C:\Programs\Qt" "E:\Programs\Qt" "C:\Program Files\Qt" "E:\Program Files\Qt") do (
        if not defined JOM if exist "%%~Q\Tools\QtCreator\bin\jom\jom.exe" set "JOM=%%~Q\Tools\QtCreator\bin\jom\jom.exe"
    )
)
if defined JOM (
    echo  [INFO] Parallel build tool: %JOM%  -j%CPU_CORES%
) else (
    echo  [WARN] jom not found - falling back to SEQUENTIAL nmake.
    echo  [WARN] Install Qt Creator or put jom.exe on PATH to enable parallel builds.
)

echo.
echo ======================================================================
echo  [1/1] Building OpenSSL 3.5.7 (static, no-asm, parallel compile via jom)
echo ======================================================================
if not exist "%OSSL_BUILD%" mkdir "%OSSL_BUILD%"
cd /d "%OSSL_BUILD%"

perl "%OSSL_SRC%\Configure" VC-WIN64A no-asm no-shared no-tests no-makedepend --prefix="%OSSL_PREFIX%" --openssldir="%OSSL_PREFIX%\ssl" --libdir=lib
if errorlevel 1 goto :failed

REM --- Build: parallel when jom is available ---
if defined JOM (
    "%JOM%" -j%CPU_CORES%
) else (
    nmake
)
if errorlevel 1 goto :failed

REM --- Install: run sequentially (mostly file copies; no parallelism needed) ---
if defined JOM (
    "%JOM%" install_sw
) else (
    nmake install_sw
)
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
