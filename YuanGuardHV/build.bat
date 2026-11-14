@echo off
setlocal enabledelayedexpansion

set "PROJECT_DIR=%~dp0"
set "HV_DIR=%PROJECT_DIR%hv"
set "BIN_DIR=%PROJECT_DIR%bin"
set "WDK_ROOT=C:\Program Files (x86)\Windows Kits\10"

rem 9.358: source-level checks run BEFORE compiling (they read sources only).
rem The map-dependent checks (page layout) are deferred to AFTER the link, because
rem they read bin\yuanguard_hv.map - and running them here read the PREVIOUS
rem build's map. Measured: aligning four guard globals to their own pages changed
rem nothing that the check could see, so it kept reporting the old layout and
rem failing the build it had not yet produced.
echo [YuanGuardHV] Running source-level checks...
powershell -NoProfile -ExecutionPolicy Bypass -File "%PROJECT_DIR%tests\run_source_checks.ps1"
if errorlevel 1 (
    echo [ERROR] Static checks failed
    exit /b 1
)

if not exist "%BIN_DIR%" mkdir "%BIN_DIR%"

echo [YuanGuardHV] Building AMD-V Hypervisor Driver...

rem Find LLVM
set "LLVM_DIR=C:\Program Files\LLVM\bin"
if exist "%LLVM_DIR%\clang-cl.exe" (
    set "CLANG_CL=%LLVM_DIR%\clang-cl.exe"
) else (
    for %%i in (clang-cl.exe) do set "CLANG_CL=%%~$PATH:i"
)
if "%CLANG_CL%"=="" (
    echo [ERROR] clang-cl not found
    exit /b 1
)
echo   clang-cl: %CLANG_CL%

rem Find MSVC link.exe
set "MSVC_LINK=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\link.exe"
if exist "%MSVC_LINK%" goto :have_link
echo [ERROR] MSVC link.exe not found at "%MSVC_LINK%"
exit /b 1
:have_link
echo   link.exe: %MSVC_LINK%

rem Find WDK
if exist "%WDK_ROOT%" goto :have_wdk
echo [ERROR] WDK not found at %WDK_ROOT%
exit /b 1
:have_wdk

set "WDK_VER=10.0.19041.0"

set "INCLUDES=/I"%WDK_ROOT%\Include\%WDK_VER%\km""
set "INCLUDES=%INCLUDES% /I"%WDK_ROOT%\Include\%WDK_VER%\shared""
set "INCLUDES=%INCLUDES% /I"%WDK_ROOT%\Include\%WDK_VER%\um""
set "INCLUDES=%INCLUDES% /I"%HV_DIR%\common""
set "INCLUDES=%INCLUDES% /I"%HV_DIR%\pool""

set "CFLAGS=/nologo /O2 /kernel /GR- /EHs-c- /Zl /GS-"
set "CFLAGS=%CFLAGS% -Wno-microsoft -Wno-unknown-pragmas -Wno-ignored-attributes -Wno-visibility -Wno-pragma-pack"
rem 9.349: make ONLY the silently-wrong-behaviour warnings fatal.
rem
rem hide_core() was declared uint32_t, counted the pages it hid, then fell off its
rem end with no return, so it returned whatever the register held. Measured on the
rem first clean-boot run: "hid 0 page(s)" while the counters showed hidden=3.
rem
rem The compiler DID warn (-Wreturn-type), but this build emits 208 warnings from
rem cl.exe's kernel-intrinsic set and the real one was invisible among them.
rem Making every warning fatal is not possible (those 208 are not defects).
rem Making the ones that mean "this code has no defined behaviour" fatal is:
rem such a value cannot be a warning to skim past, because the caller receives
rem something arbitrary that looks plausible.
set "CFLAGS=%CFLAGS% -Werror=return-type"
set "CFLAGS=%CFLAGS% -Werror=implicit-function-declaration"
set "CFLAGS=%CFLAGS% /D_KERNEL_MODE /D_AMD64_ /DNTDDI_VERSION=0x0A000005 /DYGHV_DEBUG_LOG"
if "%YGHV_BAREMETAL_NO_RESIDENT%"=="1" set "CFLAGS=%CFLAGS% /DYGHV_BAREMETAL_NO_RESIDENT=1"
if not "%YGHV_BAREMETAL_STEP%"=="" set "CFLAGS=%CFLAGS% /DYGHV_BAREMETAL_STEP=%YGHV_BAREMETAL_STEP%"
if "%YGHV_R1_EXCLUDE_PRIVATE%"=="1" set "CFLAGS=%CFLAGS% /DYGHV_R1_EXCLUDE_PRIVATE=1"
if "%YGHV_REAL_HOOK_TEST%"=="1" set "CFLAGS=%CFLAGS% /DYGHV_REAL_HOOK_TEST=1"
if "%YGHV_LOADER_STEALTH%"=="1" set "CFLAGS=%CFLAGS% /DYGHV_LOADER_STEALTH=1"
if "%YGHV_UNLOAD_GUARD%"=="1" set "CFLAGS=%CFLAGS% /DYGHV_UNLOAD_GUARD=1"
if "%YGHV_NO_APIC_SHADOW%"=="1" set "CFLAGS=%CFLAGS% /DYGHV_NO_APIC_SHADOW=1"
if "%YGHV_NO_MSV_DEEP%"=="1" set "CFLAGS=%CFLAGS% /DYGHV_NO_MSV_DEEP=1"
if "%YGHV_APIC_IDENTITY%"=="1" set "CFLAGS=%CFLAGS% /DYGHV_APIC_IDENTITY=1"
if "%YGHV_APIC_PASSTHROUGH%"=="1" set "CFLAGS=%CFLAGS% /DYGHV_APIC_PASSTHROUGH=1"
if "%YGHV_APIC_SHADOW%"=="1" set "CFLAGS=%CFLAGS% /DYGHV_APIC_SHADOW=1"
if "%YGHV_NO_CR3_INTERCEPT%"=="1" set "CFLAGS=%CFLAGS% /DYGHV_NO_CR3_INTERCEPT=1"
if "%YGHV_HOST_ISR%"=="1" set "CFLAGS=%CFLAGS% /DYGHV_HOST_ISR=1"
if "%YGHV_SINGLECORE%"=="1" set "CFLAGS=%CFLAGS% /DYGHV_SINGLECORE=1"
if "%YGHV_203Q%"=="1" set "CFLAGS=%CFLAGS% /DYGHV_203Q=1"
if "%YGHV_206B_COEXIST%"=="1" set "CFLAGS=%CFLAGS% /DYGHV_206B_COEXIST=1"
if "%YGHV_206B_GNPT%"=="1" set "CFLAGS=%CFLAGS% /DYGHV_206B_GNPT=1"
rem 9.375 (Tier 9.1): build with NO control device, so the CPUID channel can be
rem tested without the IOCTL path being available to explain the result.
if "%YGHV_NO_DEVICE%"=="1" set "CFLAGS=%CFLAGS% /DYGHV_NO_DEVICE=1"
if "%YGHV_CATCHALL%"=="1" set "CFLAGS=%CFLAGS% /DYGHV_CATCHALL=1"
if "%YGHV_V101_4VEC%"=="1" set "CFLAGS=%CFLAGS% /DYGHV_V101_4VEC=1"

set "LINKS=/nologo /SUBSYSTEM:NATIVE /DRIVER:WDM /ENTRY:DriverEntry /MACHINE:X64"
set "LINKS=%LINKS% /OPT:NOREF,NOICF"
rem 9.590: /Brepro makes the image byte-identical across rebuilds.
rem Measured cause of the churn: the PE header carries a link timestamp,
rem so the same sources produced a different SHA256 every time. That made
rem docs_check owner of a number nobody could reproduce - the documented
rem hash was right only for the one build that wrote it, and every rebuild
rem had to edit two documents before the static checks would pass again.
rem A reproducible hash turns that field into a real assertion: it now
rem says the sources produce this image, which is checkable by anyone.
set "LINKS=%LINKS% /Brepro"
rem 9.304: emit a MAP file. Three BSODs this session had to be triaged by hand
rem (disassembling raw bytes and reading .pdata) because no symbol map existed;
rem with /MAP, "yghv+0x12e13" resolves to a named function in one lookup.
set "LINKS=%LINKS% /MAP:"%BIN_DIR%\yuanguard_hv.map""
set "LINKS=%LINKS% /LIBPATH:"%WDK_ROOT%\Lib\%WDK_VER%\km\x64""

echo   WDK Version: %WDK_VER%
echo   Compiling...

rem 2026-10-11 Phase 1: mem_pte added (page-table walk, Engine B foundation).
for %%f in (main svm_core npt_core vmexit vmmcall multi_core protect control_device loader_stealth svm_simplevm_port mem_pte mem_pa mem_engine mem_scan mem_bench mem_share yghv_sections mem_selfmap mem_coop mem_island mem_view mem_watch mem_time mem_cache mem_bp mem_frame yghv_fail yghv_guard yghv_crypto yghv_auth yghv_channel yghv_unload_trace yghv_sentry yghv_offsets yghv_sig yghv_proc yghv_inject yghv_pt yghv_obguard yghv_win yghv_safe) do call :compile %%f || goto :error

echo   svm_trampoline.S
"%CLANG_CL%" %CFLAGS% %INCLUDES% /c /Fo"%BIN_DIR%\svm_trampoline.obj" "%HV_DIR%\svm_trampoline.S"
if errorlevel 1 goto :error

echo   svm_simplevm_port.S
"%CLANG_CL%" %CFLAGS% %INCLUDES% /c /Fo"%BIN_DIR%\svm_simplevm_port_asm.obj" "%HV_DIR%\svm_simplevm_port.S"
if errorlevel 1 goto :error

set "SVP206_OBJS="
if not "%YGHV_BAREMETAL_STEP%"=="206" goto :no206
rem step206-A: vendored VERBATIM upstream SimpleSvm as the entry vehicle.
rem C++ .cpp via MSVC cl + .asm via ml64, using the proven build_simplesvm flags.
set "MSVC_DIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207"
set "SS_INC=%WDK_ROOT%\Include\10.0.26100.0\km\crt;%WDK_ROOT%\Include\10.0.26100.0\km;%WDK_ROOT%\Include\10.0.26100.0\shared;%WDK_ROOT%\Include\10.0.26100.0\shared\win32;%WDK_ROOT%\Include\10.0.26100.0\ucrt;%MSVC_DIR%\include"
set "SS_LIB=%WDK_ROOT%\Lib\10.0.26100.0\km\x64;%MSVC_DIR%\lib\x64"
echo   svm_simplevm206_x64.asm (ml64)...
"%MSVC_DIR%\bin\Hostx64\x64\ml64.exe" /nologo /c /Fo"%BIN_DIR%\svm_simplevm206_asm.obj" "%HV_DIR%\svm_simplevm206_x64.asm"
if errorlevel 1 goto :error
echo   svm_simplevm206.cpp (cl)...
set "INCLUDE=%SS_INC%"
set "SS206_DEFS=/D_AMD64_ /DNDEBUG /DNTDDI_VERSION=0x0A000005 /D_WIN32_WINNT=0x0A00 /DYGHV_BAREMETAL_STEP=%YGHV_BAREMETAL_STEP%"
if "%YGHV_206B_COEXIST%"=="1" set "SS206_DEFS=%SS206_DEFS% /DYGHV_206B_COEXIST=1"
if "%YGHV_206B_GNPT%"=="1" set "SS206_DEFS=%SS206_DEFS% /DYGHV_206B_GNPT=1"
"%MSVC_DIR%\bin\Hostx64\x64\cl.exe" /nologo /c /kernel /O2 /GS- /GR- /EHs-c- /Zl /std:c++17 /utf-8 %SS206_DEFS% /Fo"%BIN_DIR%\svm_simplevm206.obj" "%HV_DIR%\svm_simplevm206.cpp"
if errorlevel 1 goto :error
set "SVP206_OBJS="%BIN_DIR%\svm_simplevm206.obj" "%BIN_DIR%\svm_simplevm206_asm.obj""
:no206

echo   Linking (MSVC)...
set "LIB=%SS_LIB%"
"%MSVC_LINK%" %LINKS% /OUT:"%BIN_DIR%\yuanguard_hv.sys" ^
    "%BIN_DIR%\main.obj" ^
    "%BIN_DIR%\svm_core.obj" ^
    "%BIN_DIR%\npt_core.obj" ^
    "%BIN_DIR%\vmexit.obj" ^
    "%BIN_DIR%\vmmcall.obj" ^
    "%BIN_DIR%\multi_core.obj" ^
    "%BIN_DIR%\protect.obj" ^
    "%BIN_DIR%\control_device.obj" ^
    "%BIN_DIR%\loader_stealth.obj" ^
    "%BIN_DIR%\mem_pte.obj" ^
    "%BIN_DIR%\mem_pa.obj" ^
    "%BIN_DIR%\mem_engine.obj" ^
    "%BIN_DIR%\mem_scan.obj" ^
    "%BIN_DIR%\mem_bench.obj" ^
    "%BIN_DIR%\mem_share.obj" ^
    "%BIN_DIR%\yghv_sections.obj" ^
    "%BIN_DIR%\mem_selfmap.obj" ^
    "%BIN_DIR%\mem_coop.obj" ^
    "%BIN_DIR%\mem_island.obj" ^
    "%BIN_DIR%\mem_view.obj" ^
    "%BIN_DIR%\mem_watch.obj" ^
    "%BIN_DIR%\mem_time.obj" ^
    "%BIN_DIR%\mem_cache.obj" ^
    "%BIN_DIR%\mem_bp.obj" ^
    "%BIN_DIR%\mem_frame.obj" ^
    "%BIN_DIR%\yghv_fail.obj" ^
    "%BIN_DIR%\yghv_guard.obj" ^
    "%BIN_DIR%\yghv_crypto.obj" ^
    "%BIN_DIR%\yghv_auth.obj" ^
    "%BIN_DIR%\yghv_channel.obj" ^
    "%BIN_DIR%\yghv_unload_trace.obj" ^
    "%BIN_DIR%\yghv_sentry.obj" ^
    "%BIN_DIR%\yghv_offsets.obj" ^
    "%BIN_DIR%\yghv_sig.obj" ^
    "%BIN_DIR%\yghv_proc.obj" ^
    "%BIN_DIR%\yghv_inject.obj" ^
    "%BIN_DIR%\yghv_obguard.obj" ^
    "%BIN_DIR%\yghv_pt.obj" ^
    "%BIN_DIR%\yghv_win.obj" ^
    "%BIN_DIR%\yghv_safe.obj" ^
    "%BIN_DIR%\svm_simplevm_port.obj" ^
    "%BIN_DIR%\svm_simplevm_port_asm.obj" ^
    "%BIN_DIR%\svm_trampoline.obj" ^
    !SVP206_OBJS! ^
    "%WDK_ROOT%\Lib\%WDK_VER%\km\x64\ntoskrnl.lib"
if errorlevel 1 goto :error

rem 9.358: map-dependent checks run HERE, after the linker has written the map.
echo [YuanGuardHV] Running map-level checks...
powershell -NoProfile -ExecutionPolicy Bypass -File "%PROJECT_DIR%tests\run_map_checks.ps1"
if errorlevel 1 (
    echo [ERROR] Map-level checks failed
    exit /b 1
)

echo [YuanGuardHV] Build SUCCESS: %BIN_DIR%\yuanguard_hv.sys
echo   signing...
rem 9.608: THE SIGNING RESULT MUST GATE THE BUILD.
rem
rem This script used to run signtool, ignore its exit code, print the hash
rem and exit 0. Measured: one build produced an UNSIGNED driver and said
rem "Build SUCCESS"; sc start then failed with 577 "cannot verify the
rem digital signature", which reads as a certificate or test-signing
rem problem rather than "the signer did not run".
rem
rem An artifact that reports success and cannot be loaded is the exact
rem shape this tree keeps paying for: the check did not observe the event.
"C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe" sign /fd SHA256 /a /f "%PROJECT_DIR%yuanguard_test.cer" "%BIN_DIR%\yuanguard_hv.sys"
if errorlevel 1 (
    echo [ERROR] signtool FAILED - the driver would not load
    goto :error
)
certutil -hashfile "%BIN_DIR%\yuanguard_hv.sys" SHA256
exit /b 0

:compile
echo   %1.c
"%CLANG_CL%" %CFLAGS% %INCLUDES% /c /Fo"%BIN_DIR%\%1.obj" "%HV_DIR%\%1.c"
if errorlevel 1 exit /b 1
exit /b 0

:error
echo [ERROR] Build FAILED
exit /b 1
