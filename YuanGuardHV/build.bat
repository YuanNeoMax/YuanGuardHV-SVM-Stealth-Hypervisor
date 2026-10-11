@echo off
setlocal enabledelayedexpansion

set "PROJECT_DIR=%~dp0"
set "HV_DIR=%PROJECT_DIR%hv"
set "BIN_DIR=%PROJECT_DIR%bin"
set "WDK_ROOT=C:\Program Files (x86)\Windows Kits\10"

echo [YuanGuardHV] Running static checks...
powershell -NoProfile -ExecutionPolicy Bypass -File "%PROJECT_DIR%tests\run_static_checks.ps1"
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
if "%YGHV_CATCHALL%"=="1" set "CFLAGS=%CFLAGS% /DYGHV_CATCHALL=1"
if "%YGHV_V101_4VEC%"=="1" set "CFLAGS=%CFLAGS% /DYGHV_V101_4VEC=1"

set "LINKS=/nologo /SUBSYSTEM:NATIVE /DRIVER:WDM /ENTRY:DriverEntry /MACHINE:X64"
set "LINKS=%LINKS% /OPT:NOREF,NOICF"
rem 9.304: emit a MAP file. Three BSODs this session had to be triaged by hand
rem (disassembling raw bytes and reading .pdata) because no symbol map existed;
rem with /MAP, "yghv+0x12e13" resolves to a named function in one lookup.
set "LINKS=%LINKS% /MAP:"%BIN_DIR%\yuanguard_hv.map""
set "LINKS=%LINKS% /LIBPATH:"%WDK_ROOT%\Lib\%WDK_VER%\km\x64""

echo   WDK Version: %WDK_VER%
echo   Compiling...

rem 2026-10-11 Phase 1: mem_pte added (page-table walk, Engine B foundation).
for %%f in (main svm_core npt_core vmexit vmmcall multi_core protect control_device loader_stealth svm_simplevm_port mem_pte mem_pa mem_engine mem_scan mem_selfmap) do call :compile %%f || goto :error

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
    "%BIN_DIR%\mem_selfmap.obj" ^
    "%BIN_DIR%\svm_simplevm_port.obj" ^
    "%BIN_DIR%\svm_simplevm_port_asm.obj" ^
    "%BIN_DIR%\svm_trampoline.obj" ^
    !SVP206_OBJS! ^
    ntoskrnl.lib
if errorlevel 1 goto :error

echo [YuanGuardHV] Build SUCCESS: %BIN_DIR%\yuanguard_hv.sys
echo   signing...
"C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe" sign /fd SHA256 /a /f "%PROJECT_DIR%yuanguard_test.cer" "%BIN_DIR%\yuanguard_hv.sys"
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
