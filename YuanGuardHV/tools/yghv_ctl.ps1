<#
.SYNOPSIS
YuanGuardHV control-device client (IOCTL validation tool).

Commands:
  state
  set-target <pid>
  add-page <hex_va>
  remove-page <hex_va>
  start
  stop
  target
  list-targets
  list-pages
  list-hooks
  install-hook <name|hex_va> [hook_id]
  remove-hook <hook_id>
  clear
  config [auto-disarm <0|1> | deny-status <hex>]
  set-auto-start
  unset-auto-start
  harden-service
  unharden-service
  service-name                      (read-only: which name, exists?, start type, SDDL)
  pa-section <hex_pa>               (read-only: which of our sections holds this PA)
  npt-raw <hex_pa> [a|d|ad]          (raw NPT entry; A/D are the DECOY-read observer)
  npt-scan <hex_start> <pages> [clear]  (A/D over a range - the one-command measurement)
  exit-codes                        (per-code exit counters: which codes actually fire)
  handler-cost [reset]              (TSC ticks per #VMEXIT inside the handler)
  byovd-scan                        (local only: blocklist + KDU provider names, no driver needed)
  selftest
  selftest-abort
  exit-test
  protect-page <pid> <hex_va>      (206-C4: cross-process arm by pid)
  unprotect-page <pid> <hex_va>
  mmf-open <path>                  (map a file-backed MMF, print VA)
  mmf-loop <path> <sec> [info]     (target: write loop on the MMF view)
  mmf-write <path>                 (attacker: one write; BLOCKED or LANDED)
  scan-pid <pid> [count]           (206-C5: enumerate target's private RW pages)
  wpm-write <pid> <hex_va> <hex_val> (cross-process write via kernel API)
#>
param(
    [Parameter(Position = 0)][string]$Command = 'state',
    [Parameter(Position = 1)]$Arg1 = $null,
    [Parameter(Position = 2)]$Arg2 = $null,
    [Parameter(Position = 3)]$Arg3 = $null,
    # 9.305: Arg4 added for mmf-hold's optional mapping size (multi-page tests).
    [Parameter(Position = 4)]$Arg4 = $null,
    # Phase 4: Arg5..Arg7 carry mem-scan's writable/exec flags and range.
    # Without these declarations PowerShell rejects the extra positional
    # arguments outright ("cannot find a positional parameter"), which is why
    # mem-scan failed before its handler ever ran.
    [Parameter(Position = 5)]$Arg5 = $null,
    [Parameter(Position = 6)]$Arg6 = $null,
    [Parameter(Position = 7)]$Arg7 = $null,
    # 9.500: the service the sc.exe commands act on. The tree's real service
    # is yghvpa; the four commands used to hard-code `yuanguard`, which never
    # existed (measured: sc config yuanguard -> 1060). Overridable so a
    # differently-named deployment does not need a source edit.
    [string]$ServiceName = 'yghvpa'
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class YghvCtlNative
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateFile(
        string lpFileName,
        uint dwDesiredAccess,
        uint dwShareMode,
        IntPtr lpSecurityAttributes,
        uint dwCreationDisposition,
        uint dwFlagsAndAttributes,
        IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr hObject);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool DeviceIoControl(
        IntPtr hDevice,
        uint dwIoControlCode,
        byte[] lpInBuffer,
        uint nInBufferSize,
        byte[] lpOutBuffer,
        uint nOutBufferSize,
        out uint lpBytesReturned,
        IntPtr lpOverlapped);

    [DllImport("kernel32.dll")]
    public static extern uint GetCurrentProcessId();

    public static uint IoCtl(uint fn)
    {
        return (0x5947u << 16) | (0u << 14) | (fn << 2) | 0u;
    }

    public const uint GENERIC_READ = 0x80000000;
    public const uint GENERIC_WRITE = 0x40000000;
    public const uint FILE_SHARE_READ = 0x1;
    public const uint FILE_SHARE_WRITE = 0x2;
    public const uint OPEN_EXISTING = 3;
}

public static class YghvPrivilege
{
    [StructLayout(LayoutKind.Sequential)]
    public struct Luid
    {
        public uint LowPart;
        public int HighPart;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct TokenPrivileges
    {
        public uint PrivilegeCount;
        public Luid Luid;
        public uint Attributes;
    }

    [DllImport("kernel32.dll")]
    public static extern IntPtr GetCurrentProcess();

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr handle);

    [DllImport("advapi32.dll", SetLastError = true)]
    public static extern bool OpenProcessToken(
        IntPtr processHandle,
        uint desiredAccess,
        out IntPtr tokenHandle);

    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern bool LookupPrivilegeValue(
        string systemName,
        string name,
        out Luid luid);

    [DllImport("advapi32.dll", SetLastError = true)]
    public static extern bool AdjustTokenPrivileges(
        IntPtr tokenHandle,
        bool disableAllPrivileges,
        ref TokenPrivileges newState,
        uint bufferLength,
        IntPtr previousState,
        IntPtr returnLength);

    public static bool EnableSeDebugPrivilege()
    {
        IntPtr token;
        if (!OpenProcessToken(GetCurrentProcess(), 0x0028, out token))
            return false;
        try
        {
            Luid luid;
            if (!LookupPrivilegeValue(null, "SeDebugPrivilege", out luid))
                return false;

            TokenPrivileges tp = new TokenPrivileges();
            tp.PrivilegeCount = 1;
            tp.Luid = luid;
            tp.Attributes = 0x2; /* SE_PRIVILEGE_ENABLED */

            if (!AdjustTokenPrivileges(token, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero))
                return false;
            return Marshal.GetLastWin32Error() == 0;
        }
        finally
        {
            CloseHandle(token);
        }
    }
}
'@

if (-not [YghvPrivilege]::EnableSeDebugPrivilege()) {
    throw 'Unable to enable SeDebugPrivilege. Run this script from an elevated PowerShell session.'
}

# 206-C4: file-backed MMF helper. Two processes mapping the same file section
# share the physical pages (GC cannot move a mapped view), so the controller
# can arm the target's mapped VA via protect-page and the attacker's write to
# the same content hits the DENY path. MapViewOfFile returns the native base
# VA directly (MemoryMappedFile views hide it), which protect-page needs.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public sealed class YghvMapView
{
    public IntPtr View;
    public IntPtr Map;
    public IntPtr File;
}

public static class YghvMmf
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateFileW(string name, uint access,
        uint share, IntPtr sa, uint disp, uint flags, IntPtr tmpl);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr CreateFileMappingW(IntPtr hFile, IntPtr sa,
        uint protect, uint sizeHigh, uint sizeLow, IntPtr name);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr MapViewOfFile(IntPtr hMap, uint access,
        uint offHigh, uint offLow, UIntPtr bytes);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool UnmapViewOfFile(IntPtr baseAddr);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr h);

    public static YghvMapView MapFile(string path, uint size)
    {
        IntPtr f = CreateFileW(path, 0x80000000u | 0x40000000u,
            0x1u | 0x2u, IntPtr.Zero, 4 /* OPEN_ALWAYS */, 0, IntPtr.Zero);
        if (f == (IntPtr)(-1))
            throw new Exception("CreateFile failed, Win32 error " +
                Marshal.GetLastWin32Error());
        IntPtr m = CreateFileMappingW(f, IntPtr.Zero, 0x4 /* PAGE_READWRITE */,
            0, size, IntPtr.Zero);
        if (m == IntPtr.Zero)
        {
            CloseHandle(f);
            throw new Exception("CreateFileMapping failed, Win32 error " +
                Marshal.GetLastWin32Error());
        }
        IntPtr v = MapViewOfFile(m, 0x2u | 0x4u /* FILE_MAP_WRITE|READ */,
            0, 0, (UIntPtr)size);
        if (v == IntPtr.Zero)
        {
            CloseHandle(m);
            CloseHandle(f);
            throw new Exception("MapViewOfFile failed, Win32 error " +
                Marshal.GetLastWin32Error());
        }
        YghvMapView r = new YghvMapView();
        r.View = v; r.Map = m; r.File = f;
        return r;
    }

    /*
     * 9.372 (C1): an EXECUTABLE mapping of the same section.
     *
     * MapFile above asks for PAGE_READWRITE, so a byte written there can never be
     * fetched - and a 0xCC that is never fetched cannot raise #BP. That is why
     * the INT3 test since Tier 4.4 has only ever proved "the byte was written and
     * restored": its own target page comment says "never executed".
     *
     * This variant asks for PAGE_EXECUTE_READWRITE and VirtualProtect's the view
     * executable, so the same section can be RUN FROM. The page is still a guest
     * physical page we can point mem-bp int3 at, which is what makes the hot path
     * testable.
     */
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool VirtualProtect(IntPtr addr, System.UIntPtr size,
        uint newProtect, out uint oldProtect);

    public static YghvMapView MapFileExec(string path, uint size)
    {
        /*
         * 9.372 (C1) CORRECTION: a file mapping object CANNOT be created with
         * PAGE_EXECUTE_READWRITE.
         *
         * Measured: CreateFileMapping(..., PAGE_EXECUTE_READWRITE, ...) returns
         * Win32 error 5 (ACCESS_DENIED). The section object has no "executable"
         * notion - executability is a property of the VIEW, applied by
         * MapViewOfFile's access mask and then by VirtualProtect. Asking the
         * mapping for XRW looks like the obvious spelling and is simply invalid.
         *
         * So the section is created PAGE_READWRITE (the only thing it accepts),
         * the view is mapped with FILE_MAP_ALL_ACCESS, and VirtualProtect makes
         * the VIEW executable. The physical page is the same either way, which is
         * what matters: mem-bp int3 patches the GPA, and this process fetches
         * from that GPA.
         */
        IntPtr f = CreateFileW(path, 0x80000000u | 0x40000000u,
            0x1u | 0x2u, IntPtr.Zero, 4 /* OPEN_ALWAYS */, 0, IntPtr.Zero);
        if (f == (IntPtr)(-1))
            throw new Exception("CreateFile failed, Win32 error " +
                Marshal.GetLastWin32Error());
        IntPtr m = CreateFileMappingW(f, IntPtr.Zero, 0x4 /* PAGE_READWRITE */,
            0, size, IntPtr.Zero);
        if (m == IntPtr.Zero)
        {
            CloseHandle(f);
            throw new Exception("CreateFileMapping failed, Win32 error " +
                Marshal.GetLastWin32Error());
        }
        IntPtr v = MapViewOfFile(m, 0xF001Fu /* FILE_MAP_ALL_ACCESS */, 0, 0, (UIntPtr)size);
        if (v == IntPtr.Zero)
        {
            CloseHandle(m);
            CloseHandle(f);
            throw new Exception("MapViewOfFile failed, Win32 error " +
                Marshal.GetLastWin32Error());
        }
        YghvMapView r = new YghvMapView();
        r.View = v; r.Map = m; r.File = f;
        return r;
    }

    /*
     * 9.372 (C1): make an ALREADY-MAPPED view executable.
     *
     * Separate from the mapping because VirtualProtect on a fresh view failed
     * with error 87 in this container: the protect must come AFTER the pages are
     * touched, not before. Ordering it this way also means the executable window
     * is as short as it can be - the stub is written, then the page becomes
     * executable, then it is called.
     */
    public static string MakeExecutable(IntPtr view, uint size)
    {
        uint old = 0;
        /* Return the error instead of a bool: "false" alone cannot distinguish
           an invalid argument from a refused protection, and both were seen. */
        if (VirtualProtect(view, new System.UIntPtr(size), 0x40, out old))
            return "OK";
        return "ERR " + Marshal.GetLastWin32Error();
    }

    /*
     * 9.372 (C1): allocate a page, write the `xor eax,eax; ret` stub, return its VA.
     *
     * PAGE_EXECUTE_READWRITE at allocation time is what makes the page both
     * patchable (mem-bp writes 0xCC through the GPA) and fetchable (we call it).
     * The same pattern already works for the CPUID/RDTSC stubs above.
     *
     * A FILE MAPPING CANNOT DO THIS - measured twice: CreateFileMapping with
     * PAGE_EXECUTE_READWRITE returns error 5 (a section has no executability),
     * and VirtualProtect on a mapped view returns error 87. An allocated page is
     * also exactly what mem-pte can resolve to a GPA, which is the property the
     * INT3 test actually needs.
     */
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern System.IntPtr VirtualAlloc(System.IntPtr addr,
        System.UIntPtr size, uint allocType, uint protect);

    [System.Runtime.InteropServices.UnmanagedFunctionPointer(
        System.Runtime.InteropServices.CallingConvention.Cdecl)]
    private delegate int ExecStubFn();

    public static ulong AllocExecStub()
    {
        System.IntPtr mem = VirtualAlloc(System.IntPtr.Zero, (System.UIntPtr)4096,
            0x3000 /* COMMIT|RESERVE */, 0x40 /* PAGE_EXECUTE_READWRITE */);
        if (mem == System.IntPtr.Zero)
            throw new Exception("VirtualAlloc(RWX) failed, Win32 error " +
                Marshal.GetLastWin32Error());
        byte[] stub = new byte[] { 0x31, 0xC0, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90 };
        Marshal.Copy(stub, 0, mem, stub.Length);
        return (ulong)mem.ToInt64();
    }

    /*
     * Call into a guest page with the fault CATCHABLE.
     *
     * A 0xCC we placed should be handled and re-executed, so the caller sees a
     * normal return. If the chain is broken the exception reaches this managed
     * frame, and an unhandled one in a powershell child is a WER dialog and a dead
     * process - indistinguishable from "the driver hung". So the outcome is
     * RETURNED as a string.
     *
     * This has to be a REAL call. A placeholder that reported success without
     * transferring control would make the INT3 test pass while never fetching the
     * 0xCC, which is exactly the defect C1 exists to remove.
     */
    [System.Runtime.ExceptionServices.HandleProcessCorruptedStateExceptions]
    [System.Security.SecurityCritical]
    public static string CallAddress(ulong va)
    {
        try
        {
            ExecStubFn fn = (ExecStubFn)System.Runtime.InteropServices.Marshal
                .GetDelegateForFunctionPointer(new System.IntPtr((long)va), typeof(ExecStubFn));
            int r = fn();
            return "returned " + r;
        }
        catch (System.AccessViolationException) { return "AVE"; }
        catch (System.Exception ex_) { return "EX " + ex_.GetType().Name; }
    }

    public static string CallView(System.IntPtr view)
    {
        return CallAddress((ulong)view.ToInt64());
    }


    public static void UnmapFile(YghvMapView v)
    {
        if (v == null) return;
        if (v.View != IntPtr.Zero) UnmapViewOfFile(v.View);
        if (v.Map != IntPtr.Zero) CloseHandle(v.Map);
        if (v.File != IntPtr.Zero) CloseHandle(v.File);
        v.View = v.Map = v.File = IntPtr.Zero;
    }

    /* 9.286: the AVE must be caught INSIDE this C# frame - PowerShell's
     * dynamic call site cannot catch cross-boundary AVEs even with the CSE
     * attributes (the mmf-write child died unhandled in run_c18). */
    [System.Runtime.ExceptionServices.HandleProcessCorruptedStateExceptions]
    [System.Security.SecurityCritical]
    public static string TryRead(IntPtr view, out long value)
    {
        try
        {
            value = Marshal.ReadInt64(view);
            return "OK";
        }
        catch (AccessViolationException)
        {
            value = 0;
            return "AVE";
        }
    }

    /* 9.284: CSE-protected - a read right after a DENY-injected #PF can
     * still catch an in-flight AV; without the attribute the mmf-write
     * child dies unhandled before printing BLOCKED (run_c18 gate2). */
    [System.Runtime.ExceptionServices.HandleProcessCorruptedStateExceptions]
    [System.Security.SecurityCritical]
    public static long ReadVal(IntPtr view)
    {
        return Marshal.ReadInt64(view);
    }

    /*
     * 9.529: COMMIT one page of a mapped view.
     *
     * MEASURED, not assumed: on a mapped file only a STORE commits a page. Two
     * cheaper-looking alternatives were tried and BOTH failed on this machine:
     *   - VirtualLock reported success for 8 pages while mem-pte showed pages
     *     6-7 still NOT MAPPED (it pins, it does not bring in);
     *   - PrefetchVirtualMemory reported success for all 8 while mem-pte showed
     *     ALL 8 NOT MAPPED (it is a hint, not a commit).
     * And the pre-fix code, re-measured: page 0 MAPPED, pages 1-7 NOT MAPPED.
     *
     * So a store is required, and the store is safe because of ORDERING:
     * mmf-hold runs BEFORE guard add arms the page, so it cannot trip write
     * protection. The value carries the page index in its low bits, so a later
     * roll-back can be attributed to a specific page - one repeated constant
     * would make "restored page N" and "restored the wrong page" identical.
     *
     * This does mean mmf-hold stores once per page, which contradicts its old
     * "never writes" comment; that comment was corrected where the loop lives
     * rather than left to disagree with the code. The property that actually
     * matters survives: after seeding it never writes again, so a read following
     * OUR write is unambiguous.
     */
    public static void SeedPage(IntPtr view, int offset, long value)
    {
        Marshal.WriteInt64(new IntPtr(view.ToInt64() + offset), value);
    }

    /*
     * 9.529: FLUSH the whole view after seeding.
     *
     * Measured without this: 8 pages seeded, only 6 of them MAPPED
     * (p0-p5 yes, p6-p7 NOT MAPPED) - and the same 6-of-8 happened with the
     * old code that seeded one page. So the writes DO commit pages, but the
     * tail of the range stays uncommitted because nothing forces the mapping
     * to materialise.
     *
     * FlushViewOfFile asks the OS to push the range out, which commits every
     * page it touches. It is the only call in this API that acts on the WHOLE
     * range rather than on pages something already happened to touch.
     */
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool FlushViewOfFile(IntPtr addr, UIntPtr numBytes);

    public static void FlushAll(IntPtr view, uint size)
    {
        if (!FlushViewOfFile(view, (UIntPtr)size))
        {
            int err = Marshal.GetLastWin32Error();
            throw new Exception("FlushAll failed, Win32 error " + err);
        }
    }

    public static void WriteVal(IntPtr view, long value)
    {
        Marshal.WriteInt64(view, value);
    }

    /* One machine store, then report. The DENY verdict arrives as an injected
     * #PF (present, write, user) on this store; .NET raises it as an
     * AccessViolationException, which is a corrupted-state exception - only
     * catchable from a method marked HandleProcessCorruptedStateExceptions.
     * Windows PowerShell 5.1 (.NET Framework) honours the attribute; pwsh 7
     * fail-fasts instead, so this must run under powershell.exe. */
    [System.Runtime.ExceptionServices.HandleProcessCorruptedStateExceptions]
    [System.Security.SecurityCritical]
    public static string TryWrite(IntPtr view, long value)
    {
        try
        {
            Marshal.WriteInt64(view, value);
            return "WROTE";
        }
        catch (AccessViolationException)
        {
            return "BLOCKED";
        }
    }
}
'@

# 9.368 (A4, Tier 5.4): GUEST-SIDE CPUID.
#
# The whole point of the CPUID view is what the GUEST sees, and the driver cannot
# answer that: __cpuidex executed inside the driver runs on the host side of the
# VMRUN boundary, so it returns the REAL hardware and never reaches
# SvHandleCpuid. A test built on a driver-side read would pass at every policy
# level while proving nothing.
#
# This runs CPUID from a normal user-mode process instead. CPUID is intercepted
# for the whole guest regardless of CPL, so this is the path that exercises the
# policy - and it is also exactly what a detector would do.
Add-Type -TypeDefinition @'
/* 9.368 (B1/B2, Tier 5.1): GUEST-side RDTSC / RDTSCP.
   Same reason as YghvCpuid above: __rdtsc called from the DRIVER would read the
   host clock and never reach SvHandleVmExit. A time test built on a driver-side
   read proves nothing about what the guest sees. */
public static class YghvTsc
{
    [System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true)]
    private static extern System.IntPtr VirtualAlloc(System.IntPtr addr, System.UIntPtr size,
        uint allocType, uint protect);

    /*  mfence; rdtsc; shl rdx,32; or rax,rdx; ret   (Win64: retval in rax)
     *  mfence is required: without it rdtsc may execute before earlier stores,
     *  which makes interval measurements noisy enough to hide a real effect. */
    private static readonly byte[] RdtscStub = new byte[]
    {
        0x0F, 0xAE, 0xF0,              /* mfence                  */
        0x0F, 0x31,                    /* rdtsc                   */
        0x48, 0xC1, 0xE2, 0x20,        /* shl rdx, 32             */
        0x48, 0x09, 0xD0,              /* or rax, rdx             */
        0xC3                           /* ret                     */
    };

    [System.Runtime.InteropServices.UnmanagedFunctionPointer(
        System.Runtime.InteropServices.CallingConvention.Cdecl)]
    private delegate ulong TscFn();

    private static TscFn _fn;

    public static ulong Read()
    {
        if (_fn == null)
        {
            System.IntPtr mem = VirtualAlloc(System.IntPtr.Zero,
                (System.UIntPtr)RdtscStub.Length, 0x3000, 0x40);
            if (mem == System.IntPtr.Zero)
                throw new System.InvalidOperationException("VirtualAlloc for RDTSC stub failed");
            System.Runtime.InteropServices.Marshal.Copy(RdtscStub, 0, mem, RdtscStub.Length);
            _fn = (TscFn)System.Runtime.InteropServices.Marshal.GetDelegateForFunctionPointer(
                mem, typeof(TscFn));
        }
        return _fn();
    }
}

public static class YghvCpuid
{
    [System.Runtime.InteropServices.UnmanagedFunctionPointer(
        System.Runtime.InteropServices.CallingConvention.Cdecl)]
    private delegate void CpuidFn(int leaf, int sub, out int a, out int b, out int c, out int d);

    /* Marshal the four output registers as int[4] so the caller does not need
       four out-parameters. CPUID itself cannot be called from C# directly, so
       the stub below is emitted as machine code into a private RWX region. */
    public static int[] Query(int leaf, int sub)
    {
        int a, b, c, d;
        Native(leaf, sub, out a, out b, out c, out d);
        return new int[] { a, b, c, d };
    }

    [System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true)]
    private static extern System.IntPtr VirtualAlloc(System.IntPtr addr, System.UIntPtr size,
        uint allocType, uint protect);

    /*  x86-64 stub (Win64 ABI): args are leaf->ecx? no: (leaf, sub, &a, &b, &c, &d)
     *  rcx=leaf rdx=sub r8=&a r9=&b  [rsp+0x28]=&c [rsp+0x30]=&d
     *
     *      push rbx
     *      mov  eax, ecx          ; leaf
     *      mov  ecx, edx          ; subleaf
     *      cpuid
     *      mov  [r8], eax
     *      mov  [r9], ebx
     *      mov  r10, [rsp+0x30]   ; &c  (0x28 after push +0x08 for ret addr)
     *      mov  [r10], ecx
     *      mov  r11, [rsp+0x38]   ; &d
     *      mov  [r11], edx
     *      pop  rbx
     *      ret
     */
    private static readonly byte[] Stub = new byte[]
    {
        0x53,                                            /* push rbx                */
        0x89, 0xC8,                                      /* mov eax, ecx            */
        0x89, 0xD1,                                      /* mov ecx, edx            */
        0x0F, 0xA2,                                      /* cpuid                   */
        0x41, 0x89, 0x00,                                /* mov [r8], eax           */
        0x41, 0x89, 0x19,                                /* mov [r9], ebx           */
        0x4C, 0x8B, 0x54, 0x24, 0x30,                    /* mov r10, [rsp+0x30]     */
        0x41, 0x89, 0x0A,                                /* mov [r10], ecx          */
        0x4C, 0x8B, 0x5C, 0x24, 0x38,                    /* mov r11, [rsp+0x38]     */
        0x41, 0x89, 0x13,                                /* mov [r11], edx          */
        0x5B,                                            /* pop rbx                 */
        0xC3                                             /* ret                     */
    };

    private static CpuidFn _fn;

    private static void Native(int leaf, int sub, out int a, out int b, out int c, out int d)
    {
        if (_fn == null)
        {
            System.IntPtr mem = VirtualAlloc(System.IntPtr.Zero, (System.UIntPtr)Stub.Length,
                0x3000 /*COMMIT|RESERVE*/, 0x40 /*PAGE_EXECUTE_READWRITE*/);
            if (mem == System.IntPtr.Zero)
                throw new System.InvalidOperationException("VirtualAlloc for CPUID stub failed");
            System.Runtime.InteropServices.Marshal.Copy(Stub, 0, mem, Stub.Length);
            _fn = (CpuidFn)System.Runtime.InteropServices.Marshal.GetDelegateForFunctionPointer(
                mem, typeof(CpuidFn));
        }
        _fn(leaf, sub, out a, out b, out c, out d);
    }
}
'@
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class YghvMem
{
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr OpenProcess(uint access, bool inherit,
        uint pid);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool VirtualQueryEx(IntPtr hProcess, IntPtr addr,
        out MEMORY_BASIC_INFORMATION mbi, uint len);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool ReadProcessMemory(IntPtr h, IntPtr addr,
        byte[] buf, uint size, out uint read);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool WriteProcessMemory(IntPtr h, IntPtr addr,
        byte[] buf, uint size, out uint written);

    [StructLayout(LayoutKind.Sequential)]
    public struct MEMORY_BASIC_INFORMATION
    {
        public IntPtr BaseAddress;
        public IntPtr AllocationBase;
        public uint AllocationProtect;
        public IntPtr RegionSize;
        public uint State;
        public uint Protect;
        public uint Type;
    }

    public static MEMORY_BASIC_INFORMATION VirtualQueryEx(IntPtr h,
        IntPtr addr)
    {
        MEMORY_BASIC_INFORMATION mbi;
        bool r = VirtualQueryEx(h, addr, out mbi,
            (uint)Marshal.SizeOf(typeof(MEMORY_BASIC_INFORMATION)));
        if (!r)
        {
            mbi = new MEMORY_BASIC_INFORMATION();
            mbi.BaseAddress = IntPtr.Zero;
            mbi.RegionSize = IntPtr.Zero;
        }
        return mbi;
    }
}
'@

# 9.500: one place decides the service name for every sc.exe command.
$YghvService = $ServiceName

$sysCommands = @('set-auto-start', 'unset-auto-start', 'harden-service', 'unharden-service', 'byovd-scan', 'service-name')
if ($sysCommands -contains $Command.ToLower()) {
    switch ($Command.ToLower()) {
        'service-name' {
            # 9.500: READ-ONLY. Answers all three questions the four sc commands
            # depend on, without changing anything: which name is in effect, does
            # the service exist, what is its start type, and what is its DACL.
            # It exists because the four commands below had a wrong name for a
            # long time and there was no way to notice short of running one of
            # THEM (which changes machine state to answer a question that did not
            # need changing it).
            $sc = Join-Path $env:SystemRoot 'System32\sc.exe'
            Write-Host ("service-name: resolved = {0}" -f $YghvService)
            if ($YghvService -ne 'yghvpa') {
                Write-Host '  note: not the tree default (yghvpa) - passed with -ServiceName.'
            }
            # Keep the output as an ARRAY. Joining it first made every
            # Select-String match return the whole report, twice.
            $qcLines = @(& $sc qc $YghvService 2>&1 | ForEach-Object { "$_" })
            $qc = ($qcLines -join ' ')
            if ($qc -match '1060') {
                Write-Host ("  EXISTS = NO  (sc qc returned 1060: no service named {0})" -f $YghvService)
                Write-Host '  Every command below would fail with 1060. Pass the right -ServiceName.'
                exit 1
            }
            Write-Host '  EXISTS = YES'
            $seen = @{}
            foreach ($line in @($qcLines)) {
                $text = [string]$line
                foreach ($key in @('START_TYPE', 'BINARY_PATH_NAME')) {
                    if (-not $seen.ContainsKey($key) -and $text.Contains($key)) {
                        # collapse runs of whitespace without a regex escape
                        $joined = ([string]::Join(' ', ($text -split '\s'))).Trim()
                        while ($joined.Contains('  ')) { $joined = $joined.Replace('  ', ' ') }
                        Write-Host ('    ' + $joined)
                        $seen[$key] = $true
                    }
                }
            }
            $sddl = (& $sc sdshow $YghvService 2>&1 | Select-Object -Last 1).Trim()
            if (-not $sddl) {
                Write-Host '  SDDL = (empty) - the DACL could not be read.'
                exit 1
            }
            Write-Host ('  SDDL = ' + $sddl)
            if ($sddl -match 'D;;SDWP;;;BA') {
                Write-Host '  hardened = YES (BA delete is denied - harden-service is in effect)'
            } else {
                Write-Host '  hardened = NO  (BA still holds delete - harden-service not applied)'
            }
            exit 0
        }
        'set-auto-start' {
            # 9.500: was `sc.exe config yuanguard ...` - 1060, never worked.
            $sc = Join-Path $env:SystemRoot 'System32\sc.exe'
            $out = (& $sc config $YghvService start= auto 2>&1) -join ' '
            if ($out -notmatch 'SUCCESS') {
                throw ("set-auto-start: sc config {0} failed: {1}" -f $YghvService, $out)
            }
            Write-Host ("set-auto-start: OK (service {0} start=auto)" -f $YghvService)
            exit 0
        }
        'unset-auto-start' {
            $sc = Join-Path $env:SystemRoot 'System32\sc.exe'
            $out = (& $sc config $YghvService start= demand 2>&1) -join ' '
            if ($out -notmatch 'SUCCESS') {
                throw ("unset-auto-start: sc config {0} failed: {1}" -f $YghvService, $out)
            }
            Write-Host ("unset-auto-start: OK (service {0} start=demand)" -f $YghvService)
            exit 0
        }
        'harden-service' {
            # 9.500: was `sc.exe sdshow yuanguard` / `sdset yuanguard ...`.
            $sc = Join-Path $env:SystemRoot 'System32\sc.exe'
            $backup = (& $sc sdshow $YghvService 2>&1 | Select-Object -Last 1).Trim()
            if (-not $backup) {
                throw ("harden-service: sdshow {0} returned nothing" -f $YghvService)
            }
            Set-Content -LiteralPath 'D:\aaaaaavm\yghv_service_sddl_backup.txt' `
                -Value $backup -Encoding UTF8
            $hardenSddl = 'D:(D;;SDWP;;;BA)(A;;CCLCSWRPWPDTLOCRRC;;;SY)' +
                '(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)' +
                '(A;;CCLCSWLOCRRC;;;IU)(A;;CCLCSWLOCRRC;;;SU)' +
                'S:(AU;FA;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;WD)'
            $out = (& $sc sdset $YghvService $hardenSddl 2>&1) -join ' '
            if ($out -notmatch 'SUCCESS') {
                throw ("harden-service: sdset {0} failed: {1}" -f $YghvService, $out)
            }
            Write-Host ("harden-service: OK (service {0} delete denied, backup saved)" -f $YghvService)
            exit 0
        }
        'unharden-service' {
            $sc = Join-Path $env:SystemRoot 'System32\sc.exe'
            $backupPath = 'D:\aaaaaavm\yghv_service_sddl_backup.txt'
            if (-not (Test-Path $backupPath)) {
                throw 'unharden-service: backup SDDL not found'
            }
            $backup = (Get-Content -LiteralPath $backupPath -Raw).Trim()
            $out = (& $sc sdset $YghvService $backup 2>&1) -join ' '
            if ($out -notmatch 'SUCCESS') {
                throw ("unharden-service: sdset {0} failed: {1}" -f $YghvService, $out)
            }
            Write-Host ("unharden-service: OK (service {0} backup SDDL restored)" -f $YghvService)
            exit 0
        }
        'byovd-scan' {
            # 9.499 (Tier 7.1, the other half): we can hide our OWN pages, but until
            # now we could not answer "does this machine hold SOMEONE ELSE'S kernel
            # read/write". That question is what KDU's provider list is, read from
            # the other side of the same table.
            #
            # LOCAL AND READ-ONLY. It never loads, opens or talks to a driver - it
            # returns before CreateFile, so it works whether or not yghvpa runs.
            #
            # TWO NAME SETS, and they are different kinds of evidence:
            #   BLOCK  Microsoft's vulnerable-driver blocklist, parsed out of
            #          System32\CodeIntegrity\driversipolicy.p7b. Authoritative:
            #          Microsoft has already ruled these bad.
            #   KDU    the provider drivers listed in hfiref0x/KDU Help/providers.md.
            #          The attacker's menu - each is a signed driver giving arbitrary
            #          kernel memory read/write. Only part of them is blocklisted,
            #          so "absent from BLOCK" is NOT "not usable". That gap is the
            #          reason both sets are scanned.
            #
            # WHAT IS INSPECTED, with counts printed so that "found nothing" can
            # never be mistaken for "inspected nothing":
            #   1. System32\drivers\*.sys on disk   -> basename match
            #   2. currently loaded kernel drivers -> PathName basename match
            #
            # GRADING. An EXACT basename match is STRONG. A basename that merely
            # CONTAINS a provider name of six or more characters is WEAK and is
            # printed separately, because "winio" also matches "WinIoTool"; the two
            # grades are never merged into one number.
            #
            # KNOWN LIMIT, stated here rather than left to be assumed: STRONG is a
            # NAME match. Version is NOT judged against KDU's ceilings (e.g. it wants
            # RTCore64 <= 4.6.2, ProcExp152 <= 1.5.2). A newer, fixed file with the
            # same name therefore shows up as STRONG - which is why FileVersion is
            # printed beside every STRONG line. Read the version before believing it.
            $kduNames = @(
                'NalDrv','RTCore64','Gdrv','ATSZIO','MsIo64','GLCKIo2','EneIo64',
                'WinRing0x64','EneTechIo64','PhyMemx64','RtkIo64','LHA','AsIO2',
                'DirectIo64','GmerDrv','DBUtil_2_3','mimidrv','KProcessHacker2',
                'ProcExp152','DBUtilDrv2','Dbk64','AsIO3','hw','SysDrv3S','amsdk',
                'inpoutx64','AsrDrv106','ALSysIO64','AMDRyzenMasterDriver','physmem',
                'LDD','pcdsrvc_x64','winio','EtdSupport','KExplore','KObjExp',
                'KRegExp','EchoDrv','nvoclock','IREC','PhyDMACC','rzpnk','PdFwKrnl',
                'AODDriver','wnBios64','EleetX1','AxtuDrv','AppShopDrv103',
                'AsrDrv107n','AsrDrv107','PmxDrv','HwRwDrv','NeacSafe64',
                'ThrottleStop','TPwSav','LnvMSRIO','CorMem','Ipctype','WinHwDrv',
                'affdriver','mtxC9CB','PGRHostControl','LECOMA','RootLaser','KtAPI',
                'hpwks'
            )

            # 9.502: the CSV beside this script is the authority when it exists -
            # FileSHA1 per provider, so a hit can be CONFIRMED instead of argued.
            # The literal list above stays as the fallback for a checkout that
            # lacks the CSV, and the row count is printed so a stale table is
            # visible rather than silent.
            $kduTable = @{}
            $kduCsvCount = 0
            $csvPath = Join-Path $PSScriptRoot 'kdu_providers.csv'
            if (Test-Path -LiteralPath $csvPath) {
                $rows = @(Import-Csv -LiteralPath $csvPath)
                foreach ($r in $rows) {
                    if (-not $r.FileSHA1) { continue }
                    $kduTable[$r.FileSHA1.ToUpperInvariant()] = $r
                    $kduCsvCount++
                }
            }

            # ---- 1. the blocklist --------------------------------------------
            $blNames = @()
            $blNote = ''
            $p7b = Join-Path $env:SystemRoot 'System32\CodeIntegrity\driversipolicy.p7b'
            if (Test-Path -LiteralPath $p7b) {
                # There is no documented API to enumerate a .p7b, so this extracts
                # UTF-16LE strings and keeps anything shaped like a driver file
                # name. It reads the bytes Microsoft's signed policy committed to;
                # it is not a policy query. An entry stored in any other shape
                # would be missed, which is why the name COUNT is printed.
                $blob = [IO.File]::ReadAllBytes($p7b)
                $blTxt = [Text.Encoding]::Unicode.GetString($blob)
                $blNames = @([regex]::Matches($blTxt, '[A-Za-z0-9_\-.]{4,}\.sys') |
                             ForEach-Object { $_.Value.ToLowerInvariant() } |
                             Sort-Object -Unique)
                $blNote = 'parsed'
            } else {
                $blNote = 'NOT FOUND - the BLOCK half did not run'
            }

            # ---- 2. what is on disk and what is loaded -----------------------
            $drvDir = Join-Path $env:SystemRoot 'System32\drivers'
            $onDisk = @(Get-ChildItem -LiteralPath $drvDir -Filter *.sys -ErrorAction SilentlyContinue)
            $loaded = @(Get-CimInstance Win32_SystemDriver -ErrorAction SilentlyContinue)
            $loadedNote = ''
            if ($loaded.Count -eq 0) {
                $loadedNote = 'Win32_SystemDriver returned nothing - the LOADED half did not run'
            }

            $strong = @()
            $weak = @()
            $blMs = $false
            $msDowngraded = 0
            $hits = @()

            foreach ($f in $onDisk) {
                $fileLeaf = $f.Name.ToLowerInvariant()
                $why = @()
                # 9.502: hash first. A FileSHA1 match is the only thing that may
                # be called a HIT; everything else is a candidate at best.
                $fsha1 = ''
                if ($kduCsvCount -gt 0) {
                    try {
                        $fsha1 = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA1).Hash
                    } catch { $fsha1 = '' }
                }
                if ($fsha1 -and $kduTable.ContainsKey($fsha1)) {
                    $rec = $kduTable[$fsha1]
                    $hits += ('  {0,-26} HIT       KDU #{1} {2}  v={3}  advisory={4}' -f
                              $f.Name, $rec.Id, $rec.DriverName,
                              $f.VersionInfo.FileVersion, $rec.AdvisoryId)
                    continue
                }
                if ($blNames -contains $fileLeaf) {
                    # A name hit is not a verdict. The blocklist also keys files
                    # by page hash and by certificate, so a MICROSOFT-signed file
                    # that merely shares a name with a blocked one (appid.sys,
                    # monitor.sys - both WinBuild files on this machine) is a
                    # FALSE POSITIVE at name level. Downgrade it honestly and say
                    # which channel proved it.
                    $sg = (Get-AuthenticodeSignature -LiteralPath $f.FullName).SignerCertificate
                    if ($sg -and $sg.Subject -match 'Microsoft') {
                        $blMs = $true
                    } else {
                        $why += 'BLOCK'
                    }
                }
                if ($blMs) { $msDowngraded++; $blMs = $false }
                if ($kduNames -contains $f.BaseName) { $why += 'KDU' }
                if ($why.Count -gt 0) {
                    $strong += ('  {0,-26} on disk   v={1,-14} {2}' -f $f.Name,
                                $f.VersionInfo.FileVersion, ($why -join '+'))
                } else {
                    foreach ($n in @($kduNames)) {
                        if ($n.Length -ge 6 -and $fileLeaf.Contains($n.ToLowerInvariant())) {
                            $weak += ('  {0,-26} on disk   contains "{1}"' -f $f.Name, $n)
                            break
                        }
                    }
                }
            }

            foreach ($d in @($loaded)) {
                # Win32_SystemDriver.PathName comes in as \??\C:\... - a native
                # path. Strip the device prefix before touching the file, or every
                # version read fails and the column shows 'unreadable'.
                $pn = "$($d.PathName)"
                $pn = $pn.TrimStart('\??\')
                if ($pn -notmatch '\.sys$') { continue }
                $pn = $pn.TrimStart('\??\')
                $leaf = [IO.Path]::GetFileName($pn)
                $leafLower = $leaf.ToLowerInvariant()
                $base = [IO.Path]::GetFileNameWithoutExtension($pn)
                $why = @()
                $msSigned = $false
                $lsha1 = ''
                if ($kduCsvCount -gt 0 -and (Test-Path -LiteralPath $pn)) {
                    try { $lsha1 = (Get-FileHash -LiteralPath $pn -Algorithm SHA1).Hash } catch { $lsha1 = '' }
                }
                if ($lsha1 -and $kduTable.ContainsKey($lsha1)) {
                    $rec2 = $kduTable[$lsha1]
                    $hits += ('  {0,-26} HIT+LOADED KDU #{1} {2}  state={3}  advisory={4}' -f
                              $leaf, $rec2.Id, $rec2.DriverName, $d.State, $rec2.AdvisoryId)
                    continue
                }
                if ($blNames -contains $leafLower) {
                    $sg2 = (Get-AuthenticodeSignature -LiteralPath $pn -ErrorAction SilentlyContinue).SignerCertificate
                    if ($sg2 -and $sg2.Subject -match 'Microsoft') { $msSigned = $true }
                    else { $why += 'BLOCK' }
                }
                if ($kduNames -contains $base) { $why += 'KDU' }
                if ($why.Count -eq 0) { continue }
                $vv = ''
                try { $vv = (Get-Item -LiteralPath $pn -ErrorAction Stop).VersionInfo.FileVersion }
                catch { $vv = 'unreadable' }
                $strong += ('  {0,-26} LOADED    v={1,-14} {2}  state={3}' -f $leaf,
                            $vv, ($why -join '+'), $d.State)
            }

            # ---- 3. report --------------------------------------------------
            Write-Host ("byovd-scan: block={0} name(s) [{1}]  kdu={2} name(s)  hash-table={3}  on-disk={4}  loaded={5}" -f
                $blNames.Count, $blNote, $kduNames.Count, $kduCsvCount, $onDisk.Count, $loaded.Count)
            if ($loadedNote) { Write-Host ("  {0}" -f $loadedNote) }

            # 9.502: three grades, never merged into one number.
            if ($hits.Count -gt 0) {
                Write-Host ("  HIT ({0}) - FileSHA1 matches a KDU provider record. This IS the file:" -f $hits.Count)
                foreach ($s in @($hits)) { Write-Host $s }
            }
            if ($strong.Count -gt 0) {
                Write-Host ("  CANDIDATE ({0}) - right NAME, wrong FILE; usable only if its version is in range:" -f $strong.Count)
                foreach ($s in @($strong)) { Write-Host $s }
            }
            if ($weak.Count -gt 0) {
                Write-Host ("  WEAK ({0}) - name merely contained, NOT a hit until a hash says so:" -f $weak.Count)
                foreach ($s in @($weak)) { Write-Host $s }
            }

            $nLoad = 0
            foreach ($s in @($strong)) { if ($s -match ' LOADED ') { $nLoad++ } }
            Write-Host ("  verdict: hit={0} candidate={1} weak={2} candidate-and-loaded={3} block-name-but-Microsoft-signed={4} kdu-table-rows={5}" -f
                $hits.Count, $strong.Count, $weak.Count, $nLoad, $msDowngraded, $kduCsvCount)

            if ($blNames.Count -eq 0) {
                Write-Host '  INCOMPLETE: the blocklist yielded no names, so a zero hit count here means nothing.'
            } elseif ($strong.Count -eq 0 -and $onDisk.Count -gt 0 -and $loaded.Count -gt 0) {
                Write-Host '  clean: no blocklisted driver and no KDU provider by name, on disk or loaded.'
            }
            # Local command: return before the device handle is opened. Falling
            # through reached CreateFile and threw ERROR_FILE_NOT_FOUND whenever
            # yghvpa was not running - a read-only scan that failed because the
            # driver was stopped, which is exactly when you want to run it.
            return
        }
    }
}

$devicePath = '\\.\YuanGuardHV'
$handle = [YghvCtlNative]::CreateFile(
    $devicePath,
    [YghvCtlNative]::GENERIC_READ -bor [YghvCtlNative]::GENERIC_WRITE,
    [YghvCtlNative]::FILE_SHARE_READ -bor [YghvCtlNative]::FILE_SHARE_WRITE,
    [IntPtr]::Zero,
    [YghvCtlNative]::OPEN_EXISTING,
    0,
    [IntPtr]::Zero)
if ($handle -eq [IntPtr]::Zero) {
    throw ("CreateFile {0} failed, Win32 error 0x{1:X8}" -f $devicePath,
        [Runtime.InteropServices.Marshal]::GetLastWin32Error())
}
if ($handle -eq [IntPtr](-1)) {
    throw ("CreateFile {0} failed, Win32 error 0x{1:X8}" -f $devicePath,
        [Runtime.InteropServices.Marshal]::GetLastWin32Error())
}

# ===== Phase 3-5 helpers (2026-10-11) =====

# sizeof(yghv_ioctl_scan_req_t) = 4*4 + 8*3 + 64 + 64 = 168
$sizeofYghvScanReq = 168

function Get-YghvMemTypeId {
    param([string]$Name)
    switch ($Name.ToLower()) {
        'u8'    { 0 }  'byte'  { 0 }
        'u16'   { 1 }  'i16'   { 5 }
        'u32'   { 2 }  'int32' { 6 }  'i32' { 6 }
        'u64'   { 3 }  'int64' { 7 }  'i64' { 7 }
        'i8'    { 4 }
        'f32'   { 8 }  'float' { 8 }
        'f64'   { 9 }  'double'{ 9 }
        'str'   { 10 } 'ascii' { 10 }
        'wstr'  { 11 } 'utf16' { 11 }
        'utf8'  { 12 }
        'bytes' { 13 } 'hex'   { 13 }
        'vec3'  { 14 }
        'mat4'  { 15 }
        default { -1 }
    }
}

function Get-YghvMemTypeSize {
    param([int]$Type)
    switch ($Type) {
        0 { 1 } 1 { 2 } 2 { 4 } 3 { 8 }
        4 { 1 } 5 { 2 } 6 { 4 } 7 { 8 }
        8 { 4 } 9 { 8 }
        14 { 12 } 15 { 64 }
        default { 0 }   # strings / bytes are variable
    }
}

function Format-YghvMemValue {
    param([int]$Type, [byte[]]$Bytes, [int]$Count)
    if ($Bytes.Length -eq 0) { return '(no data)' }
    switch ($Type) {
        0  { return ("0x{0:X2} ({0})" -f $Bytes[0]) }
        1  { return ("0x{0:X4} ({0})" -f [BitConverter]::ToUInt16($Bytes,0)) }
        2  { return ("0x{0:X8} ({0})" -f [BitConverter]::ToUInt32($Bytes,0)) }
        3  { return ("0x{0:X16} ({0})" -f [BitConverter]::ToUInt64($Bytes,0)) }
        4  { return ("{0}" -f [sbyte]$Bytes[0]) }
        5  { return ("{0}" -f [BitConverter]::ToInt16($Bytes,0)) }
        6  { return ("{0}" -f [BitConverter]::ToInt32($Bytes,0)) }
        7  { return ("{0}" -f [BitConverter]::ToInt64($Bytes,0)) }
        8  { return ("{0:G9}" -f [BitConverter]::ToSingle($Bytes,0)) }
        9  { return ("{0:G17}" -f [BitConverter]::ToDouble($Bytes,0)) }
        10 { return ('"' + [Text.Encoding]::ASCII.GetString($Bytes).TrimEnd([char]0) + '"') }
        11 { return ('"' + [Text.Encoding]::Unicode.GetString($Bytes).TrimEnd([char]0) + '"') }
        12 { return ('"' + [Text.Encoding]::UTF8.GetString($Bytes).TrimEnd([char]0) + '"') }
        13 { return (($Bytes | ForEach-Object { '{0:X2}' -f $_ }) -join ' ') }
        14 {
            $f = @(); for($i=0;$i -lt 3;$i++){ $f += ('{0:G6}' -f [BitConverter]::ToSingle($Bytes,$i*4)) }
            return ('vec3(' + ($f -join ', ') + ')')
        }
        15 {
            $f = @(); for($i=0;$i -lt 16;$i++){ $f += ('{0:G6}' -f [BitConverter]::ToSingle($Bytes,$i*4)) }
            return ('mat4[' + ($f -join ' ') + ']')
        }
        default { return (($Bytes | ForEach-Object { '{0:X2}' -f $_ }) -join ' ') }
    }
}

function ConvertTo-YghvMemBytes {
    param([int]$Type, [string]$Value)
    $v = $Value.Trim()
    # Integer parses must not see a 0x prefix: [uint64]::Parse('0x..') throws
    # and NumberStyles::HexNumber does not accept the prefix either.
    $vi = $v -replace '^0[xX]',''
    switch ($Type) {
        0  { return ,([byte[]]@([byte]([uint32]::Parse($vi, [Globalization.NumberStyles]::HexNumber)))) }
        1  { return ,([BitConverter]::GetBytes([uint16]::Parse($vi, [Globalization.NumberStyles]::HexNumber))) }
        2  { return ,([BitConverter]::GetBytes([uint32]::Parse($vi, [Globalization.NumberStyles]::HexNumber))) }
        3  { return ,([BitConverter]::GetBytes([uint64]::Parse($vi, [Globalization.NumberStyles]::HexNumber))) }
        4  { return ,([byte[]]@([byte]([int32]::Parse($vi, [Globalization.NumberStyles]::HexNumber) -band 0xFF))) }
        5  { return ,([BitConverter]::GetBytes([int16]::Parse($vi, [Globalization.NumberStyles]::HexNumber))) }
        6  { return ,([BitConverter]::GetBytes([int32]::Parse($vi, [Globalization.NumberStyles]::HexNumber))) }
        7  { return ,([BitConverter]::GetBytes([int64]::Parse($vi, [Globalization.NumberStyles]::HexNumber))) }
        8  { return ,([BitConverter]::GetBytes([single]::Parse($v, [Globalization.CultureInfo]::InvariantCulture))) }
        9  { return ,([BitConverter]::GetBytes([double]::Parse($v, [Globalization.CultureInfo]::InvariantCulture))) }
        10 { return ,([Text.Encoding]::ASCII.GetBytes($v)) }
        11 { return ,([Text.Encoding]::Unicode.GetBytes($v)) }
        12 { return ,([Text.Encoding]::UTF8.GetBytes($v)) }
        13 {
            $hx = ($v -replace '[\s,]','')
            if ($hx.Length % 2) { throw 'hex payload needs an even digit count' }
            $b = New-Object byte[] ($hx.Length/2)
            for ($i=0; $i -lt $b.Length; $i++) { $b[$i] = [Convert]::ToByte($hx.Substring($i*2,2),16) }
            return ,$b
        }
        14 {
            $p = ($v -split '[\s,]+') | Where-Object { $_ -ne '' }
            if ($p.Count -ne 3) { throw 'vec3 needs 3 floats' }
            $b = New-Object byte[] 12
            for($i=0;$i -lt 3;$i++){ [BitConverter]::GetBytes([single]::Parse($p[$i],[Globalization.CultureInfo]::InvariantCulture)).CopyTo($b,$i*4) }
            return ,$b
        }
        15 {
            $p = ($v -split '[\s,]+') | Where-Object { $_ -ne '' }
            if ($p.Count -ne 16) { throw 'mat4 needs 16 floats' }
            $b = New-Object byte[] 64
            for($i=0;$i -lt 16;$i++){ [BitConverter]::GetBytes([single]::Parse($p[$i],[Globalization.CultureInfo]::InvariantCulture)).CopyTo($b,$i*4) }
            return ,$b
        }
        default { throw "unsupported type $Type" }
    }
}

function Invoke-YghvIoctl {
    param([uint32]$Code, [byte[]]$InBytes = $null, [uint32]$OutputLength = 0)
    if ($null -eq $InBytes) { $InBytes = New-Object byte[] 0 }
    $out = New-Object byte[] $OutputLength
    $returned = [uint32]0
    $ok = [YghvCtlNative]::DeviceIoControl(
        $handle, $Code, $InBytes, [uint32]$InBytes.Length,
        $out, $OutputLength, [ref]$returned, [IntPtr]::Zero)
    if (-not $ok) {
        throw ("DeviceIoControl 0x{0:X8} failed, Win32 error 0x{1:X8}" -f
            $Code, [Runtime.InteropServices.Marshal]::GetLastWin32Error())
    }
    return $out
}

function Read-YghvState {
    $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x805)) -OutputLength 12
    return @{
        active    = [BitConverter]::ToUInt32($out, 0)
        pid       = [BitConverter]::ToUInt32($out, 4)
        pageCount = [BitConverter]::ToUInt32($out, 8)
    }
}

function Read-YghvTarget {
    $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x806)) -OutputLength 24
    return @{
        active    = [BitConverter]::ToUInt32($out, 0)
        pid       = [BitConverter]::ToUInt32($out, 4)
        pageCount = [BitConverter]::ToUInt32($out, 8)
        hookCount = [BitConverter]::ToUInt32($out, 12)
        cr3       = [BitConverter]::ToUInt64($out, 16)
    }
}

function Read-YghvPages {
    $buf = New-Object byte[] 1544
    [BitConverter]::GetBytes([uint32]64).CopyTo($buf, 0)
    $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x807)) `
        -InBytes $buf -OutputLength 1544
    $returned = [BitConverter]::ToUInt32($out, 4)
    $pages = @()
    for ($i = 0; $i -lt [Math]::Min($returned, 64); $i++) {
        $base = 8 + $i * 24
        $pages += [pscustomobject]@{
            gpa       = ('0x{0:X}' -f [BitConverter]::ToUInt64($out, $base))
            targetVa  = ('0x{0:X}' -f [BitConverter]::ToUInt64($out, $base + 8))
            flags     = $out[$base + 16]
            armed     = $out[$base + 17]
        }
    }
    return @{ returned = $returned; pages = $pages }
}

function Read-YghvHooks {
    $buf = New-Object byte[] 104
    [BitConverter]::GetBytes([uint32]4).CopyTo($buf, 0)
    $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x808)) `
        -InBytes $buf -OutputLength 104
    $returned = [BitConverter]::ToUInt32($out, 4)
    $hooks = @()
    for ($i = 0; $i -lt $returned; $i++) {
        $base = 8 + $i * 24
        $hooks += [pscustomobject]@{
            funcVa   = ('0x{0:X}' -f [BitConverter]::ToUInt64($out, $base))
            hookId   = [BitConverter]::ToUInt32($out, $base + 8)
            installed = [BitConverter]::ToUInt32($out, $base + 12)
            patchLen = [BitConverter]::ToUInt32($out, $base + 16)
        }
    }
    return @{ returned = $returned; hooks = $hooks }
}

function Read-YghvTargets {
    $buf = New-Object byte[] 104
    [BitConverter]::GetBytes([uint32]4).CopyTo($buf, 0)
    $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x80E)) `
        -InBytes $buf -OutputLength 104
    $returned = [BitConverter]::ToUInt32($out, 4)
    $targets = @()
    for ($i = 0; $i -lt $returned; $i++) {
        $base = 8 + $i * 24
        $targets += [pscustomobject]@{
            active    = [BitConverter]::ToUInt32($out, $base)
            pid       = [BitConverter]::ToUInt32($out, $base + 4)
            pageCount = [BitConverter]::ToUInt32($out, $base + 8)
            hookCount = [BitConverter]::ToUInt32($out, $base + 12)
            cr3       = [BitConverter]::ToUInt64($out, $base + 16)
        }
    }
    return @{ returned = $returned; targets = $targets }
}

function Read-YghvConfig {
    # 9.303: the struct grew to 16 bytes (auto_disarm, deny_status,
    # sync_enable, sync_interval_ms).
    $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x80B)) -OutputLength 16
    return @{
        autoDisarm = [BitConverter]::ToUInt32($out, 0)
        denyStatus = [BitConverter]::ToUInt32($out, 4)
        syncEnable = [BitConverter]::ToUInt32($out, 8)
        syncMs     = [BitConverter]::ToUInt32($out, 12)
    }
}

function New-YghvHookInstallBuffer {
    param([string]$NameOrVa, [uint32]$HookId)
    $buf = New-Object byte[] 144
    [BitConverter]::GetBytes($HookId).CopyTo($buf, 0)
    if ($NameOrVa -match '^0x') {
        $va = [Convert]::ToUInt64($NameOrVa.Substring(2), 16)
        [BitConverter]::GetBytes([uint64]$va).CopyTo($buf, 8)
    } else {
        if ($NameOrVa.Length -gt 63) {
            throw 'install-hook: name too long (max 63 chars)'
        }
        $bytes = [System.Text.Encoding]::Unicode.GetBytes($NameOrVa)
        $bytes.CopyTo($buf, 16)
    }
    return $buf
}

try {
    switch ($Command.ToLower()) {
        'state' {
            $st = Read-YghvState
            Write-Host ("state: active={0} pid={1} page_count={2}" -f
                $st.active, $st.pid, $st.pageCount)
        }
        'lasthit' {
            # 9.258 (206-C3): last protection verdict hit (exact faulting gpa)
            $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x80F)) -OutputLength 8
            $hit = [BitConverter]::ToUInt64($out, 0)
            Write-Host ("lasthit: 0x{0:X}" -f $hit)
        }
        'set-target' {
            $pidVal = [uint32]::Parse($Arg1)
            Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x800)) `
                -InBytes ([BitConverter]::GetBytes($pidVal)) | Out-Null
            Write-Host ("set-target: pid={0} OK" -f $pidVal)
        }
        'add-page' {
            $va = [Convert]::ToUInt64($Arg1, 16)
            Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x801)) `
                -InBytes ([BitConverter]::GetBytes($va)) | Out-Null
            Write-Host ("add-page: va=0x{0:X} OK" -f $va)
        }
        'remove-page' {
            $va = [Convert]::ToUInt64($Arg1, 16)
            Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x802)) `
                -InBytes ([BitConverter]::GetBytes($va)) | Out-Null
            Write-Host ("remove-page: va=0x{0:X} OK" -f $va)
        }
        'start' {
            Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x803)) | Out-Null
            Write-Host 'start: OK'
        }
        'stop' {
            Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x804)) | Out-Null
            Write-Host 'stop: OK'
        }
        'target' {
            $t = Read-YghvTarget
            Write-Host ("target: active={0} pid={1} cr3=0x{2:X} page_count={3} hook_count={4}" -f
                $t.active, $t.pid, $t.cr3, $t.pageCount, $t.hookCount)
        }
        'list-targets' {
            $r = Read-YghvTargets
            Write-Host ("list-targets: returned={0}" -f $r.returned)
            foreach ($t in $r.targets) {
                Write-Host ("  active={0} pid={1} cr3=0x{2:X} pages={3} hooks={4}" -f
                    $t.active, $t.pid, $t.cr3, $t.pageCount, $t.hookCount)
            }
        }
        'list-pages' {
            $r = Read-YghvPages
            Write-Host ("list-pages: returned={0}" -f $r.returned)
            foreach ($p in $r.pages) {
                Write-Host ("  gpa={0} va={1} flags={2} armed={3}" -f
                    $p.gpa, $p.targetVa, $p.flags, $p.armed)
            }
        }
        'list-hooks' {
            $r = Read-YghvHooks
            Write-Host ("list-hooks: returned={0}" -f $r.returned)
            foreach ($h in $r.hooks) {
                Write-Host ("  id={0} va={1} installed={2} patch_len={3}" -f
                    $h.hookId, $h.funcVa, $h.installed, $h.patchLen)
            }
        }
        'install-hook' {
            $hid = if ($null -eq $Arg2) { [uint32]2 } else { [uint32]::Parse($Arg2) }
            $buf = New-YghvHookInstallBuffer -NameOrVa $Arg1 -HookId $hid
            Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x80C)) `
                -InBytes $buf | Out-Null
            Write-Host ("install-hook: id={0} target={1} OK" -f $hid, $Arg1)
        }
        'remove-hook' {
            $hid = [uint32]::Parse($Arg1)
            $buf = New-Object byte[] 8
            [BitConverter]::GetBytes($hid).CopyTo($buf, 0)
            Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x80D)) `
                -InBytes $buf | Out-Null
            Write-Host ("remove-hook: id={0} OK" -f $hid)
        }
        'clear' {
            Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x809)) | Out-Null
            $st = Read-YghvState
            Write-Host ("clear: OK (active={0} pid={1} page_count={2})" -f
                $st.active, $st.pid, $st.pageCount)
        }
        'config' {
            if ($null -eq $Arg1) {
                $c = Read-YghvConfig
                Write-Host ("config: auto_disarm={0} deny_status=0x{1:X} sync={2} sync_ms={3}" -f
                    $c.autoDisarm, $c.denyStatus, $c.syncEnable, $c.syncMs)
            } elseif ($Arg1.ToLower() -eq 'auto-disarm') {
                $val = [uint32]::Parse($Arg2)
                if ($val -gt 1) { throw 'config: auto-disarm must be 0 or 1' }
                $cur = Read-YghvConfig
                $cfg = New-Object byte[] 16
                [BitConverter]::GetBytes($val).CopyTo($cfg, 0)
                [BitConverter]::GetBytes([uint32]$cur.denyStatus).CopyTo($cfg, 4)
                [BitConverter]::GetBytes([uint32]$cur.syncEnable).CopyTo($cfg, 8)
                [BitConverter]::GetBytes([uint32]$cur.syncMs).CopyTo($cfg, 12)
                Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x80A)) `
                    -InBytes $cfg | Out-Null
                Write-Host ("config: auto_disarm={0} OK" -f $val)
            } elseif ($Arg1.ToLower() -eq 'deny-status') {
                $val = [uint32]([Convert]::ToUInt64($Arg2, 16))
                if ($val -eq 0) { throw 'config: deny-status must be non-zero' }
                $cur = Read-YghvConfig
                $cfg = New-Object byte[] 16
                [BitConverter]::GetBytes([uint32]$cur.autoDisarm).CopyTo($cfg, 0)
                [BitConverter]::GetBytes($val).CopyTo($cfg, 4)
                [BitConverter]::GetBytes([uint32]$cur.syncEnable).CopyTo($cfg, 8)
                [BitConverter]::GetBytes([uint32]$cur.syncMs).CopyTo($cfg, 12)
                Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x80A)) `
                    -InBytes $cfg | Out-Null
                Write-Host ("config: deny_status=0x{0:X} OK" -f $val)
            } elseif ($Arg1.ToLower() -eq 'sync') {
                # 9.303: set the guard in the same call as the other options,
                # so a session can configure-then-arm without a separate
                # config-sync round trip.
                #   config sync <0|1> [interval_ms]
                $en = [uint32]::Parse($Arg2)
                if ($en -gt 1) { throw 'config: sync must be 0 or 1' }
                $cur = Read-YghvConfig
                $ms = if ($null -ne $Arg3) { [uint32]::Parse($Arg3) } else { [uint32]$cur.syncMs }
                if ($ms -lt 1) { throw 'config: sync interval must be >= 1 ms' }
                $cfg = New-Object byte[] 16
                [BitConverter]::GetBytes([uint32]$cur.autoDisarm).CopyTo($cfg, 0)
                [BitConverter]::GetBytes([uint32]$cur.denyStatus).CopyTo($cfg, 4)
                [BitConverter]::GetBytes($en).CopyTo($cfg, 8)
                [BitConverter]::GetBytes($ms).CopyTo($cfg, 12)
                Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x80A)) `
                    -InBytes $cfg | Out-Null
                $now = Read-YghvConfig
                Write-Host ("config: sync={0} sync_ms={1} OK (readback sync={2} sync_ms={3})" -f
                    $en, $ms, $now.syncEnable, $now.syncMs)
            } else {
                throw ("config: unknown option {0}" -f $Arg1)
            }
        }
        'selftest' {
            $bytes = New-Object byte[] 4096
            $gc = [Runtime.InteropServices.GCHandle]::Alloc(
                $bytes, [Runtime.InteropServices.GCHandleType]::Pinned)
            try {
                $addr = $gc.AddrOfPinnedObject().ToInt64()
                $pidVal = [YghvCtlNative]::GetCurrentProcessId()
                Write-Host ("selftest: pid={0} buf_va=0x{1:X}" -f $pidVal, $addr)
                $base = Read-YghvState
                $baseCount = $base.pageCount
                Write-Host ("selftest: baseline page_count={0}" -f $baseCount)

                Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x800)) `
                    -InBytes ([BitConverter]::GetBytes([uint32]$pidVal)) | Out-Null
                Write-Host 'selftest: set-target OK'
                $targets = Read-YghvTargets
                $mySlot = $targets.targets | Where-Object { $_.pid -eq $pidVal }
                if ($null -eq $mySlot -or $mySlot.pageCount -ne 0) {
                    throw 'selftest: own target slot not registered/empty'
                }

                Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x801)) `
                    -InBytes ([BitConverter]::GetBytes([uint64]$addr)) | Out-Null
                Write-Host 'selftest: add-page OK'
                $targets = Read-YghvTargets
                $mySlot = $targets.targets | Where-Object { $_.pid -eq $pidVal }
                if ($null -eq $mySlot -or $mySlot.pageCount -ne 1) {
                    throw 'selftest: add-page did not register in own slot'
                }
                $pages = Read-YghvPages
                $addrText = '0x{0:X}' -f $addr
                if (-not ($pages.pages.targetVa -contains $addrText)) {
                    throw ("selftest: list-pages mismatch (returned={0})" -f
                        $pages.returned)
                }
                Write-Host 'selftest: list-pages OK'

                Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x803)) | Out-Null
                Write-Host 'selftest: start OK'

                $st = Read-YghvState
                if ($st.active -ne 1) {
                    throw ("selftest: not active after start (active={0})" -f
                        $st.active)
                }

                [Runtime.InteropServices.Marshal]::WriteInt64(
                    [IntPtr]$addr, 0x1122334455667788)
                $readback = [Runtime.InteropServices.Marshal]::ReadInt64([IntPtr]$addr)
                if ($readback -ne 0x1122334455667788) {
                    throw 'selftest: write/read mismatch on protected page'
                }
                # 9.258 (206-C3): the verdict must have recorded a hit
                $hitOut = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x80F)) -OutputLength 8
                $lastHit = [BitConverter]::ToUInt64($hitOut, 0)
                if ($lastHit -eq 0) {
                    throw 'selftest: no protection hit recorded (last-hit==0)'
                }
                Write-Host 'selftest: user write/read OK (page armed in NPT)'

                Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x804)) | Out-Null
                Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x802)) `
                    -InBytes ([BitConverter]::GetBytes([uint64]$addr)) | Out-Null

                $st = Read-YghvState
                if ($st.active -ne 0) {
                    throw ("selftest: still active after stop (active={0})" -f
                        $st.active)
                }
                $pages = Read-YghvPages
                if ($pages.pages.targetVa -contains $addrText) {
                    throw 'selftest: own page still listed after remove'
                }
                Write-Host 'selftest: PASS'
            } finally {
                $gc.Free()
            }
        }
        'selftest-abort' {
            # 9.258 (206-C3): watchdog test - arm a page then die WITHOUT any
            # cleanup (the deliberate c12 leak scenario). The driver's process
            # -exit watchdog must disarm + free the slot; a state probe right
            # after this process exits must show page_count=0 for this pid.
            $bytes = New-Object byte[] 4096
            $gc = [Runtime.InteropServices.GCHandle]::Alloc(
                $bytes, [Runtime.InteropServices.GCHandleType]::Pinned)
            try {
                $addr = $gc.AddrOfPinnedObject().ToInt64()
                $pidVal = [YghvCtlNative]::GetCurrentProcessId()
                Write-Host ("selftest-abort: pid={0} buf_va=0x{1:X}" -f
                    $pidVal, $addr)
                Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x800)) `
                    -InBytes ([BitConverter]::GetBytes([uint32]$pidVal)) | Out-Null
                Write-Host 'selftest-abort: set-target OK'
                Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x801)) `
                    -InBytes ([BitConverter]::GetBytes([uint64]$addr)) | Out-Null
                Write-Host 'selftest-abort: add-page OK'
                Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x803)) | Out-Null
                Write-Host 'selftest-abort: start OK - dying WITHOUT cleanup'
            } finally {
                $gc.Free()
            }
            # process exits here with the page still armed; the driver's
            # watchdog does the disarm+free during teardown
        }
        'exit-test' {
            $child = Start-Process -FilePath 'cmd.exe' `
                -ArgumentList '/c ping -n 3 127.0.0.1 >nul' `
                -PassThru -WindowStyle Hidden
            try {
                $childPid = $child.Id
                Write-Host ("exit-test: child pid={0}" -f $childPid)
                Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x800)) `
                    -InBytes ([BitConverter]::GetBytes([uint32]$childPid)) | Out-Null
                Write-Host 'exit-test: set-target OK'

                $targets = Read-YghvTargets
                $childSlot = $targets.targets | Where-Object { $_.pid -eq $childPid }
                if ($null -eq $childSlot) {
                    throw 'exit-test: child slot not registered'
                }

                $deadline = (Get-Date).AddSeconds(20)
                while ((Get-Date) -lt $deadline) {
                    $child.Refresh()
                    if ($child.HasExited) { break }
                    Start-Sleep -Milliseconds 200
                }
                if (-not $child.HasExited) {
                    throw 'exit-test: child did not exit in time'
                }

                $deadline = (Get-Date).AddSeconds(10)
                do {
                    Start-Sleep -Milliseconds 250
                    $targets = Read-YghvTargets
                    $childSlot = $targets.targets |
                        Where-Object { $_.pid -eq $childPid -and $_.cr3 -ne 0 }
                } while ($null -ne $childSlot -and
                    (Get-Date) -lt $deadline)
                $targets = Read-YghvTargets
                $childSlot = $targets.targets |
                    Where-Object { $_.pid -eq $childPid -and $_.cr3 -ne 0 }
                if ($null -ne $childSlot) {
                    throw 'exit-test: child slot auto disarm not observed'
                }
                Write-Host 'exit-test: PASS (child slot auto cleared)'
            } finally {
                if (-not $child.HasExited) {
                    Stop-Process -Id $child.Id -Force -ErrorAction SilentlyContinue
                }
                $child.Dispose()
            }
        }
        'protect-page' {
            # 206-C4: cross-process arm. The target slot must already exist
            # (set-target <pid>); the page VA is resolved in the TARGET's
            # address space by the driver (KeStackAttachProcess), so the
            # controller never needs the target's CR3 itself.
            if ($null -eq $Arg1 -or $null -eq $Arg2) {
                throw 'protect-page: usage: protect-page <pid> <hex_va>'
            }
            $pidVal = [uint32]::Parse($Arg1)
            # .NET Framework Convert.ToUInt64(s,16) rejects the 0x prefix
            $va = [Convert]::ToUInt64(($Arg2 -replace '^0[xX]', ''), 16)
            $buf = New-Object byte[] 16
            [BitConverter]::GetBytes($pidVal).CopyTo($buf, 0)
            [BitConverter]::GetBytes([uint64]$va).CopyTo($buf, 8)
            Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x810)) `
                -InBytes $buf | Out-Null
            Write-Host ("protect-page: pid={0} va=0x{1:X} OK" -f $pidVal, $va)
        }
        'unprotect-page' {
            if ($null -eq $Arg1 -or $null -eq $Arg2) {
                throw 'unprotect-page: usage: unprotect-page <pid> <hex_va>'
            }
            $pidVal = [uint32]::Parse($Arg1)
            $va = [Convert]::ToUInt64(($Arg2 -replace '^0[xX]', ''), 16)
            $buf = New-Object byte[] 16
            [BitConverter]::GetBytes($pidVal).CopyTo($buf, 0)
            [BitConverter]::GetBytes([uint64]$va).CopyTo($buf, 8)
            Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x811)) `
                -InBytes $buf | Out-Null
            Write-Host ("unprotect-page: pid={0} va=0x{1:X} OK" -f $pidVal, $va)
        }
        'config-fake' {
            # 206-C6: shadow fake-write mode -- foreign KERNEL writes (cpl=0,
            # e.g. WriteProcessMemory) to an armed page are redirected to a
            # scratch page (writer sees success, real page untouched).
            # No arg = read diagnostics {mode, attempts, ok, last_reject}.
            if ($null -eq $Arg1) {
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x812)) -OutputLength 40
                Write-Host ("config-fake: mode={0} attempts={1} ok={2} last_reject={3} restores={4}" -f
                    [BitConverter]::ToUInt64($out, 0), [BitConverter]::ToUInt64($out, 8),
                    [BitConverter]::ToUInt64($out, 16), [BitConverter]::ToUInt64($out, 24),
                    [BitConverter]::ToUInt64($out, 32))
            } else {
                $val = [uint32]::Parse($Arg1)
                if ($val -gt 1) { throw 'config-fake: must be 0 or 1' }
                Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x812)) `
                    -InBytes ([BitConverter]::GetBytes($val)) | Out-Null
                Write-Host ("config-fake: {0} OK" -f $val)
            }
        }
        'config-fake-tlb' {
            # 9.297 (A'): runtime TLB-flush strategy for the fake-write path.
            # No arg = read back {mode, times_consulted}.
            # Modes: 0=legacy(TlbControl=1+INVLPGA) 1=TlbControl=2+INVLPGA
            #        2=TlbControl=2 no-INVLPGA      3=TlbControl=1+INVLPGA
            if ($null -eq $Arg1) {
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x813)) -OutputLength 16
                $names = @('legacy(TC=1+INVLPGA)', 'flushall(TC=2+INVLPGA)',
                           'flushall-noINVLPGA', 'legacy-noVmcbClean',
                           'reopen-main+TC=2')
                $m = [BitConverter]::ToUInt64($out, 0)
                $nm = if ($m -lt $names.Count) { $names[$m] } else { '?' }
                Write-Host ("config-fake-tlb: mode={0} [{1}] consulted={2}" -f
                    $m, $nm, [BitConverter]::ToUInt64($out, 8))
            } else {
                $val = [uint32]::Parse($Arg1)
                if ($val -gt 4) { throw 'config-fake-tlb: must be 0..4' }
                Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x813)) `
                    -InBytes ([BitConverter]::GetBytes($val)) | Out-Null
                Write-Host ("config-fake-tlb: set {0} OK" -f $val)
            }
        }
        'config-sync' {
            # 9.299 (C line): SYNC GUARD - snapshot + poll, no NPT permission
            # flip. The page stays writable so the hardware never needs to
            # invalidate a translation; a guard thread detects drift by
            # comparison and rolls the page back.
            # No arg = read {mode, polls, hits, interval_ms, running, pages}.
            # 1 arg = enable/disable; 2 args = also set the interval (ms).
            if ($null -eq $Arg1) {
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x814)) -OutputLength 64
                Write-Host ("config-sync: mode={0} polls={1} hits={2} interval_ms={3} running={4} refused={5} pages={6} tsc_cycles={7}" -f
                    [BitConverter]::ToUInt64($out, 0), [BitConverter]::ToUInt64($out, 8),
                    [BitConverter]::ToUInt64($out, 16), [BitConverter]::ToUInt64($out, 24),
                    [BitConverter]::ToUInt64($out, 32), [BitConverter]::ToUInt64($out, 40), [BitConverter]::ToUInt64($out, 48), [BitConverter]::ToUInt64($out, 56))
            } else {
                $en = [uint32]::Parse($Arg1)
                if ($en -gt 1) { throw 'config-sync: enable must be 0 or 1' }
                $ms = 50
                if ($null -ne $Arg2) { $ms = [uint32]::Parse($Arg2) }
                $cfg = New-Object byte[] 8
                [BitConverter]::GetBytes($en).CopyTo($cfg, 0)
                [BitConverter]::GetBytes($ms).CopyTo($cfg, 4)
                Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x814)) `
                    -InBytes $cfg | Out-Null
                Write-Host ("config-sync: set enable={0} interval_ms={1} OK" -f $en, $ms)
            }
        }
        'mmf-open' {
            if ($null -eq $Arg1) { throw 'mmf-open: usage: mmf-open <path>' }
            $size = [uint32]4096
            $map = [YghvMmf]::MapFile($Arg1, $size)
            try {
                $probe = [long]0x59474856   # 'YGHV'
                [YghvMmf]::WriteVal($map.View, $probe)
                $back = [YghvMmf]::ReadVal($map.View)
                Write-Host ("mmf-open: pid={0} va=0x{1:X} probe=0x{2:X} file={3}" -f
                    [YghvCtlNative]::GetCurrentProcessId(),
                    $map.View.ToInt64(), $back, $Arg1)
            } finally {
                [YghvMmf]::UnmapFile($map)
            }
        }
        'mmf-loop' {
            # Target side: map the section, report pid + mapped VA (the
            # controller feeds both to set-target/protect-page), then write
            # the slot on a cadence so the armed page exercises the full
            # ALLOW -> reopen -> TF -> #DB -> re-arm cycle per store.
            if ($null -eq $Arg1 -or $null -eq $Arg2) {
                throw 'mmf-loop: usage: mmf-loop <path> <seconds> [info_path]'
            }
            $path = $Arg1
            $secs = [int]$Arg2
            $map = [YghvMmf]::MapFile($path, [uint32]4096)
            $pidVal = [YghvCtlNative]::GetCurrentProcessId()
            $line = ("mmf-loop: pid={0} va=0x{1:X}" -f $pidVal, $map.View.ToInt64())
            Write-Host $line
            if ($null -ne $Arg3) {
                Set-Content -LiteralPath $Arg3 -Value $line -Encoding ASCII
            }
            try {
                $deadline = (Get-Date).AddSeconds($secs)
                $i = 0
                while ((Get-Date) -lt $deadline) {
                    $i++
                    $val = [long]0x00C4C40000000000 + $i
                    try {
                        [YghvMmf]::WriteVal($map.View, $val)
                        Write-Host ("mmf-loop: wrote i={0} val=0x{1:X}" -f $i, $val)
                    } catch [System.AccessViolationException] {
                        Write-Host ("mmf-loop: i={0} WRITE AVE" -f $i)
                    }
                    Start-Sleep -Milliseconds 400
                }
            } finally {
                [YghvMmf]::UnmapFile($map)
            }
            Write-Host 'mmf-loop: done'
        }
        'mmf-hold' {
            # 9.302 (A phase): a PASSIVE target - maps the section, publishes
            # pid + VA, then just sleeps. It never writes the page itself.
            # Needed because mmf-loop writes every 400ms, which makes it
            # impossible to tell "the guard rolled my write back" from "the
            # target's own next write overwrote it". With a holder that never
            # writes, a read after our write is unambiguous.
            if ($null -eq $Arg1 -or $null -eq $Arg2) {
                throw 'mmf-hold: usage: mmf-hold <path> <seconds> [info_path] [size_bytes]'
            }
            $path = $Arg1
            $secs = [int]$Arg2
            # 9.305: optional mapping size, so a multi-page test can arm several
            # CONTROLLED pages instead of scan-pid's arbitrary process pages.
            # Arming those made the guard roll back live code/data - that is how
            # the 0x7E crash was reached, and it was a test-design error.
            $mapSize = if ($null -ne $Arg4) { [uint32]::Parse($Arg4) } else { [uint32]4096 }
            $map = [YghvMmf]::MapFile($path, $mapSize)
            $pidVal = [YghvCtlNative]::GetCurrentProcessId()
            # Seed a recognisable, constant pattern ONCE so the page has a
            # known baseline (the guard snapshots it at arm time).
            #
            # 9.529: COMMIT EVERY PAGE, AND COMMIT BY READING.
            #
            # MapViewOfFile reserves the range; the pages are only committed when
            # touched. The old code touched page 1 and nothing else, so a
            # multi-page hold left pages 2..N with no mapping at all - `mem-pte`
            # reported NOT MAPPED for them, which is exactly what tripped 9.511
            # (`t12_3.bin`, pages 2..N).
            #
            # A READ is used, not a write, for two reasons that both matter:
            #   1. this command's whole contract is "a target that NEVER writes" -
            #      that is what makes a read after our own write unambiguous;
            #   2. once the guard arms a page, our write is DENIED by design and
            #      .NET raises an AccessViolation. The old `catch {}` hid that,
            #      which read as "seeded" when nothing had been touched.
            #
            # Failures are REPORTED, not swallowed: a page that did not commit is
            # indistinguishable from a genuinely unmapped one at the next step,
            # and silently pretending otherwise is how 9.511 was misled.
            # 9.529: [int] ROUNDS in PowerShell - (32768+4095)/4096 = 8.9997 -> 9, one
            # page past the mapping. Ceiling of the plain quotient is exact.
            $pageCount = [int][math]::Ceiling($mapSize / 4096)
            $committed = 0
            $failed    = 0
            #
            # 9.529: SEED EVERY PAGE, because a store is the only thing that commits
            # a mapped-file page (measured - see SeedPage).
            #
            # Each page gets a value carrying its own index in the low 16 bits, so a
            # later roll-back can be attributed to a specific page. One repeated
            # constant would make "restored page N" and "restored the wrong page"
            # look the same.
            #
            # This write happens BEFORE the guard arms the page (mmf-hold runs first,
            # guard add comes after) - that ordering is what makes a store safe here.
            $seedBase = [long]0x00C4C40000000000
            for ($i = 0; $i -lt $pageCount; $i++) {
                [YghvMmf]::SeedPage($map.View, $i * 4096, ($seedBase -bor [long]$i))
                $committed++
            }
            #
            # 9.529: flush the whole range, or the tail stays uncommitted
            # (measured: 6 of 8 pages mapped without this).
            [YghvMmf]::FlushAll($map.View, $mapSize)
            $line = ("mmf-hold: pid={0} va=0x{1:X} seed=0x{2:X} size=0x{3:X} pages={4} seeded={5}" -f $pidVal, $map.View.ToInt64(), $seedBase, $mapSize, $pageCount, $committed)
            Write-Host $line
            if ($null -ne $Arg3) { Set-Content -LiteralPath $Arg3 -Value $line -Encoding ASCII }
            try {
                Start-Sleep -Seconds $secs
            } finally {
                [YghvMmf]::UnmapFile($map)
            }
            Write-Host 'mmf-hold: done'
        }
        'mem-pte' {
            # Phase 1 (2026-10-11): page-table walk probe (Engine B foundation).
            #
            # Resolves a VA through the target's CR3 by hand -
            # CR3 -> PML4 -> PDPT -> PD -> PT - reading only the table pages
            # (system pages). It does NOT read the target's memory; it proves
            # the translation works, which is what everything else builds on.
            #
            # usage:
            #   mem-pte reset                    clear walk statistics
            #   mem-pte <pid> <hex_va>           resolve one VA
            #
            # Reported: PA, page size (4K/2M/1G), depth, attribute bits, and
            # cumulative walk stats (so large-page awareness is observable).
            if ($null -eq $Arg1) {
                throw 'mem-pte: usage: mem-pte reset | mem-pte <pid> <hex_va>'
            }
            if ($Arg1 -eq 'reset') {
                Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x815)) -InBytes @() | Out-Null
                Write-Host 'mem-pte: stats reset'
                return
            }
            if ($null -eq $Arg2) {
                throw 'mem-pte: usage: mem-pte reset | mem-pte <pid> <hex_va>'
            }

            $tpid = [uint32]::Parse($Arg1)
            $tva  = [Convert]::ToUInt64(($Arg2 -replace '^0[xX]', ''), 16)

            $req = New-Object byte[] 16
            [BitConverter]::GetBytes($tpid).CopyTo($req, 0)
            [BitConverter]::GetBytes([uint64]$tva).CopyTo($req, 8)

            $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x815)) `
                                    -InBytes $req -OutputLength 128
            if ($out.Length -lt 128) { throw "mem-pte: short reply ($($out.Length) bytes)" }

            $ok        = [BitConverter]::ToUInt64($out, 0)
            $cr3       = [BitConverter]::ToUInt64($out, 8)
            $vaEcho    = [BitConverter]::ToUInt64($out, 16)
            $pa        = [BitConverter]::ToUInt64($out, 24)
            $pageBase  = [BitConverter]::ToUInt64($out, 32)
            $pageSize  = [BitConverter]::ToUInt64($out, 40)
            $kind      = [BitConverter]::ToUInt64($out, 48)
            $levels    = [BitConverter]::ToUInt64($out, 56)
            $attrs     = [BitConverter]::ToUInt64($out, 64)
            $walks     = [BitConverter]::ToUInt64($out, 72)
            $h4k       = [BitConverter]::ToUInt64($out, 80)
            $h2m       = [BitConverter]::ToUInt64($out, 88)
            $h1g       = [BitConverter]::ToUInt64($out, 96)
            $np        = [BitConverter]::ToUInt64($out, 104)
            $badcr3    = [BitConverter]::ToUInt64($out, 112)
            $treads    = [BitConverter]::ToUInt64($out, 120)

            $kindName = switch ($kind) { 1 { '4K' } 2 { '2M' } 3 { '1G' } default { 'none' } }
            $attrStr = @()
            if ($attrs -band 0x01) { $attrStr += 'P' }
            if ($attrs -band 0x02) { $attrStr += 'W' }
            if ($attrs -band 0x04) { $attrStr += 'U' }
            if ($attrs -band 0x08) { $attrStr += 'A' }
            if ($attrs -band 0x10) { $attrStr += 'D' }
            if ($attrs -band 0x20) { $attrStr += 'NX' }
            if ($attrs -band 0x40) { $attrStr += 'G' }
            if ($attrStr.Count -eq 0) { $attrStr = @('-') }

            Write-Host ("mem-pte: pid={0} va=0x{1:X} -> {2}" -f $tpid, $vaEcho,
                        $(if ($ok) { "PA=0x$('{0:X}' -f $pa)" } else { 'NOT MAPPED' }))
            Write-Host ("         cr3=0x{0:X}  base=0x{1:X}  size={2}  kind={3}  depth={4}  attrs={5}" -f
                        $cr3, $pageBase, $pageSize, $kindName, $levels, ($attrStr -join ''))
            Write-Host ("         stats: walks={0} 4K={1} 2M={2} 1G={3} np={4} badcr3={5} reads={6}" -f
                        $walks, $h4k, $h2m, $h1g, $np, $badcr3, $treads)
        }
        'mmf-write' {
            # Attacker side: same section, foreign CR3. One store; while the
            # page is armed this must come back BLOCKED (DENY -> injected #PF
            # -> AccessViolationException caught in TryWrite). After the
            # target dies and the watchdog disarms, the same command must
            # report LANDED. 9.284: every access is CSE-guarded and AVEs are
            # reported (never fatal) so the fake-state read behavior is fully
            # visible in the output.
            if ($null -eq $Arg1) { throw 'mmf-write: usage: mmf-write <path>' }
            $map = [YghvMmf]::MapFile($Arg1, [uint32]4096)
            try {
                $before = 0L
                $st1 = [YghvMmf]::TryRead($map.View, [ref]$before)
                Write-Host ("mmf-write: pid={0} va=0x{1:X} current={2} 0x{3:X}" -f
                    [YghvCtlNative]::GetCurrentProcessId(),
                    $map.View.ToInt64(), $st1, $before)
                try {
                    $r = [YghvMmf]::TryWrite($map.View, [long]0xDEADBEEF00000001)
                    if ($r -eq 'BLOCKED') {
                        $after = 0L
                        $st2 = [YghvMmf]::TryRead($map.View, [ref]$after)
                        Write-Host ("mmf-write: BLOCKED (post-read {0} 0x{1:X})" -f
                            $st2, $after)
                    } else {
                        Write-Host 'mmf-write: LANDED'
                    }
                } catch [System.AccessViolationException] {
                    Write-Host 'mmf-write: WRITE AVE (uncaught by TryWrite)'
                }
            } finally {
                [YghvMmf]::UnmapFile($map)
            }
        }
        'exec-page' {
            # 9.372 (C1): a guest-EXECUTABLE page, ALLOCATED rather than mapped.
            #
            # Measured twice that a file mapping is the wrong tool:
            #   CreateFileMapping(PAGE_EXECUTE_READWRITE) -> Win32 error 5
            #     (a section object has no notion of executability)
            #   VirtualProtect on a mapped view          -> Win32 error 87
            # VirtualAlloc with MEM_COMMIT|MEM_RESERVE and PAGE_EXECUTE_READWRITE
            # is the shape that already works in this file for the CPUID/RDTSC
            # stubs, and it is also exactly what mem-pte can resolve to a GPA -
            # which is the property the INT3 test needs.
            #
            # usage:
            #   exec-page hold <sec> <info_path>   allocate, write stub, publish VA, sleep
            #   exec-page run                      allocate, write stub, CALL it
            #   exec-page call <hex_va>            call an address in an existing page
            if ($null -eq $Arg1) { throw 'exec-page: hold, run or call is required' }
            $mode = "$Arg1".ToLower()
            if ($mode -eq 'call') {
                if ($null -eq $Arg2) { throw 'exec-page call: a hex va is required' }
                $va = [Convert]::ToUInt64(("$Arg2" -replace '^0[xX]',''),16)
                $r = [YghvMmf]::CallAddress($va)
                Write-Host ("exec-page: called 0x{0:X} -> {1}" -f $va, $r)
                return
            }
            $secs = 0
            $infoPath = $null
            # hold and spinhit share the same argument shape: <sec> [info_path].
            if ($mode -eq 'hold' -or $mode -eq 'spinhit') {
                if ($null -eq $Arg2) { throw ('exec-page ' + $mode + ': a duration in seconds is required') }
                $secs = [int]$Arg2
                if ($null -ne $Arg3) { $infoPath = "$Arg3" }
            } elseif ($mode -ne 'run') {
                throw 'exec-page: mode must be hold, spinhit, run or call'
            }
            $va = [YghvMmf]::AllocExecStub()
            $pidVal = [YghvCtlNative]::GetCurrentProcessId()
            $line = ("exec-page: pid={0} va=0x{1:X}" -f $pidVal, $va)
            Write-Host $line
            if ($null -ne $infoPath) { Set-Content -LiteralPath $infoPath -Value $line -Encoding ASCII }
            if ($mode -eq 'run') {
                $r = [YghvMmf]::CallAddress($va)
                Write-Host ("exec-page: returned {0}" -f $r)
            } elseif ($mode -eq 'spinhit') {
#
# 9.372 (C1): HOLD AND REPEATEDLY CALL, in the SAME process.
# 
# A VA belongs to the process that allocated it, so a controller
# calling into a holder's address from another process is calling
# a wild pointer - measured: G42 step 3 produced NO output at all
# (the child died silently), while steps 5/6 showed the patch and
# the restore had both worked. The chain was fine; the caller was
# wrong.
# 
# So the process that owns the page is also the one that fetches
# it. The controller patches the GPA; this loop keeps executing
# the same page, and any 0xCC placed underneath becomes a #BP on
# the next iteration.
# /
                $end = (Get-Date).AddSeconds($secs)
                $n = 0
                while ((Get-Date) -lt $end) {
                    $r = [YghvMmf]::CallAddress($va)
                    $n++
                    if ($r -ne 'returned 0') {
                        Write-Host ("exec-page: iteration {0} -> {1}" -f $n, $r)
                    }
                    Start-Sleep -Milliseconds 50
                }
                Write-Host ("exec-page: spinhit done, {0} calls, last={1}" -f $n, $r)
            } else {
                Start-Sleep -Seconds $secs
                Write-Host 'exec-page: done'
            }
        }
        'mmf-read' {
            # 9.366: THE GUEST'S OWN READ OF THE PAGE - the half `mmf-write`
            # never covered.
            #
            # Why a separate command: `mem-read` reads through the DRIVER
            # (Engine A/B, our image's RIP), so it cannot distinguish a page the
            # guest can see from one it cannot - both return whatever our side
            # resolves. The decoy question is about the GUEST path: a CPL=3
            # store/load from a foreign process whose page we hid. That needs a
            # read issued by a process we are not, on the same section, so the
            # fault reaches the NPF handler as a foreign access.
            #
            # Reported values, each of which means something different:
            #   OK + the seeded value   page is visible (baseline, or after del)
            #   OK + 0                  a hidden page served by the DECOY: present
            #                           and zero, not a hole
            #   AVE                     UNMAP mode: the read faulted, i.e. the
            #                           address space has a hole in it
            #
            # usage: mmf-read <path>
            if ($null -eq $Arg1) { throw 'mmf-read: usage: mmf-read <path>' }
            $map = [YghvMmf]::MapFile($Arg1, [uint32]4096)
            try {
                $v = 0L
                $st = [YghvMmf]::TryRead($map.View, [ref]$v)
                Write-Host ("mmf-read: pid={0} va=0x{1:X} state={2} value=0x{3:X}" -f
                    [YghvCtlNative]::GetCurrentProcessId(),
                    $map.View.ToInt64(), $st, $v)
            } finally {
                [YghvMmf]::UnmapFile($map)
            }
        }
        'mem-read' {
            # Phase 3 (2026-10-11): typed cross-process read, dual engine.
            # usage: mem-read <pid> <hex_va> <type> [count]
            if ($null -eq $Arg1 -or $null -eq $Arg2 -or $null -eq $Arg3) {
                throw 'mem-read: usage: mem-read <pid> <hex_va> <type> [count]'
            }
            $tpid = [uint32]::Parse($Arg1)
            $tva  = [Convert]::ToUInt64(($Arg2 -replace '^0[xX]',''), 16)
            $tname = "$Arg3"
            $type = Get-YghvMemTypeId $tname
            if ($type -lt 0) { throw "mem-read: unknown type '$tname'" }

            $count = 1
            if ($null -ne $Arg4) { $count = [uint32]::Parse($Arg4) }
            $fixed = Get-YghvMemTypeSize $type
            $want = if ($fixed -gt 0) { $fixed * $count } else { $count }
            if ($want -le 0) { $want = 1 }

            $req = New-Object byte[] (64 + $want)
            [BitConverter]::GetBytes($tpid).CopyTo($req, 0)
            [BitConverter]::GetBytes([uint32]$type).CopyTo($req, 4)
            [BitConverter]::GetBytes([uint64]$tva).CopyTo($req, 8)
            [BitConverter]::GetBytes([uint32]$count).CopyTo($req, 16)
            [BitConverter]::GetBytes([uint32]0).CopyTo($req, 20)   # engine auto
            [BitConverter]::GetBytes([uint32]0).CopyTo($req, 24)   # flags

            $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x816)) `
                                    -InBytes $req -OutputLength (64 + $want)
            $ok    = [BitConverter]::ToUInt64($out, 0)
            $eng   = [BitConverter]::ToUInt64($out, 8)
            $done  = [BitConverter]::ToUInt64($out, 16)
            $engName = switch ($eng) { 1 { 'api' } 2 { 'pte' } 3 { 'pte-selfmap' } default { '?' } }

            if (-not $ok) {
                $st = [BitConverter]::ToUInt64($out, 48)
                Write-Host ("mem-read: FAILED status=0x{0:X8}" -f $st)
                return
            }
            $payload = New-Object byte[] $done
            [Array]::Copy($out, 64, $payload, 0, $done)
            Write-Host ("mem-read: pid={0} va=0x{1:X} type={2} engine={3} bytes={4}" -f
                        $tpid, $tva, $tname, $engName, $done)
            Write-Host ("  value: " + (Format-YghvMemValue -Type $type -Bytes $payload -Count $count))
        }
        'mem-write' {
            # Phase 3: typed cross-process write, dual engine.
            # usage: mem-write <pid> <hex_va> <type> <value>
            if ($null -eq $Arg1 -or $null -eq $Arg2 -or $null -eq $Arg3 -or $null -eq $Arg4) {
                throw 'mem-write: usage: mem-write <pid> <hex_va> <type> <value>'
            }
            $tpid = [uint32]::Parse($Arg1)
            $tva  = [Convert]::ToUInt64(($Arg2 -replace '^0[xX]',''), 16)
            $tname = "$Arg3"
            $type = Get-YghvMemTypeId $tname
            if ($type -lt 0) { throw "mem-write: unknown type '$tname'" }

            $payload = ConvertTo-YghvMemBytes -Type $type -Value "$Arg4"
            if ($payload.Length -eq 0) { throw 'mem-write: empty payload' }

            $count = if ((Get-YghvMemTypeSize $type) -gt 0) {
                        [int]($payload.Length / (Get-YghvMemTypeSize $type))
                     } else { $payload.Length }

            $req = New-Object byte[] (64 + $payload.Length)
            [BitConverter]::GetBytes($tpid).CopyTo($req, 0)
            [BitConverter]::GetBytes([uint32]$type).CopyTo($req, 4)
            [BitConverter]::GetBytes([uint64]$tva).CopyTo($req, 8)
            [BitConverter]::GetBytes([uint32]$count).CopyTo($req, 16)
            [BitConverter]::GetBytes([uint32]0).CopyTo($req, 20)
            [BitConverter]::GetBytes([uint32]0).CopyTo($req, 24)
            $payload.CopyTo($req, 64)

            $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x817)) `
                                    -InBytes $req -OutputLength 64
            $ok   = [BitConverter]::ToUInt64($out, 0)
            $eng  = [BitConverter]::ToUInt64($out, 8)
            $done = [BitConverter]::ToUInt64($out, 16)
            $engName = switch ($eng) { 1 { 'api' } 2 { 'pte' } 3 { 'pte-selfmap' } default { '?' } }
            if ($ok) {
                Write-Host ("mem-write: pid={0} va=0x{1:X} type={2} engine={3} bytes={4} OK" -f
                            $tpid, $tva, $tname, $engName, $done)
            } else {
                $st = [BitConverter]::ToUInt64($out, 48)
                Write-Host ("mem-write: FAILED status=0x{0:X8}" -f $st)
            }
        }
        'mem-engine' {
            # Phase 3: engine selection + diagnostics.
            # usage: mem-engine            (read)
            #        mem-engine auto|api|pte (set)
            #        mem-engine reset        (clear stats)
            if ($null -eq $Arg1) {
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x818)) -OutputLength 152
                $sel = [BitConverter]::ToUInt64($out, 0)
                $selName = switch ($sel) { 1 { 'api' } 2 { 'pte' } 3 { 'selfmap' } default { 'auto' } }
                Write-Host ("mem-engine: selection={0}" -f $selName)
                Write-Host ("  api : reads={0} writes={1} fail={2}" -f `
                    [BitConverter]::ToUInt64($out,8), [BitConverter]::ToUInt64($out,16), [BitConverter]::ToUInt64($out,24))
                Write-Host ("  pte : reads={0} writes={1} fail={2}" -f `
                    [BitConverter]::ToUInt64($out,32), [BitConverter]::ToUInt64($out,40), [BitConverter]::ToUInt64($out,48))
                Write-Host ("  fallbacks={0}  auto: api={1} pte={2}" -f `
                    [BitConverter]::ToUInt64($out,56), [BitConverter]::ToUInt64($out,64), [BitConverter]::ToUInt64($out,72))
                Write-Host ("  pa  : reads={0} rfail={1} writes={2} wfail={3} bytes_r={4} mapfail={5}" -f `
                    [BitConverter]::ToUInt64($out,80), [BitConverter]::ToUInt64($out,88),
                    [BitConverter]::ToUInt64($out,96), [BitConverter]::ToUInt64($out,104),
                    [BitConverter]::ToUInt64($out,112), [BitConverter]::ToUInt64($out,120))
                Write-Host ("  B-wfail: irql={0} walk={1} pa={2}" -f `
                    [BitConverter]::ToUInt64($out,128), [BitConverter]::ToUInt64($out,136),
                    [BitConverter]::ToUInt64($out,144))
                return
            }
            $v = "$Arg1".ToLower()
            if ($v -eq 'reset') {
                Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x818)) -InBytes @() | Out-Null
                Write-Host 'mem-engine: stats reset'
                return
            }
            $sel = switch ($v) { 'auto' { 0 } 'api' { 1 } 'pte' { 2 } 'selfmap' { 3 } default { -1 } }
            if ($sel -lt 0) { throw 'mem-engine: use auto|api|pte|selfmap|reset' }
            Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x818)) `
                             -InBytes ([BitConverter]::GetBytes([uint32]$sel)) | Out-Null
            Write-Host ("mem-engine: set to $v")
        }
        'mem-bench' {
            # 9.449 (Tier 1.5): Engine A vs Engine B, same range, same pattern.
            #
            # This replaces "B should be 5-20x faster" with a stopwatch. Both
            # engines read the same bytes and match the same pattern; A pays one
            # MmCopyVirtualMemory per ADDRESS, B pays one walk per PAGE plus one
            # physical read.
            #
            # usage: mem-bench <pid> <hex_pattern> [start_hex] [end_hex] [budget]
            #
            # HOW TO READ IT
            #   reads  A's unit is an address, B's is a page. On a range with holes
            #          B's count is smaller by the number of absent pages - that IS
            #          the page-table-driven claim, in numbers.
            #   hits   printed SEPARATELY, never averaged. If A and B disagree, the
            #          disagreement is the finding; folding it into a speed number
            #          would hide the more important result.
            #   faster B ran FIRST, so A had the warmer cache. Above 1.00x means B
            #          won despite that.
            if ($null -eq $Arg1 -or $null -eq $Arg2) {
                throw 'mem-bench: usage: mem-bench <pid> <hex_pattern> [start_hex] [end_hex] [budget]'
            }
            $bpid = [uint32]::Parse($Arg1)
            $toks = ("$Arg2" -split '[\s,]+') | Where-Object { $_ -ne '' }
            $patLen = $toks.Count
            if ($patLen -lt 1 -or $patLen -gt 64) { throw 'mem-bench: pattern must be 1..64 bytes' }

            $pat = New-Object byte[] 64
            $msk = New-Object byte[] 64
            for ($i = 0; $i -lt $patLen; $i++) {
                if ($toks[$i] -eq '??') { $pat[$i] = 0; $msk[$i] = 0 }
                else {
                    $pat[$i] = [Convert]::ToByte(($toks[$i] -replace '^0x',''), 16)
                    $msk[$i] = 0xFF
                }
            }

            $bstart = [uint64]0x10000
            $bend   = [uint64]0x7FFFFFFF0000
            if ($null -ne $Arg3) { $bstart = [Convert]::ToUInt64(($Arg3 -replace '^0x',''), 16) }
            if ($null -ne $Arg4) { $bend   = [Convert]::ToUInt64(($Arg4 -replace '^0x',''), 16) }
            $budget = 0
            if ($null -ne $Arg5) { $budget = [uint32]::Parse($Arg5) }

            # yghv_ioctl_bench_req_t is 160 bytes, not 152:
            #   pid(4) pat_len(4) page_budget(4) flags(4) = 16
            #   range_start(8) range_end(8)             = 32
            #   pattern[64]                             = 96
            #   mask[64]                                = 160
            # Copying mask at offset 96 into a 152-byte buffer is what threw
            # "destination array is not long enough" on the first attempt.
            $req = New-Object byte[] 160
            [BitConverter]::GetBytes($bpid).CopyTo($req, 0)
            [BitConverter]::GetBytes([uint32]$patLen).CopyTo($req, 4)
            [BitConverter]::GetBytes([uint32]$budget).CopyTo($req, 8)
            [BitConverter]::GetBytes([uint32]0).CopyTo($req, 12)
            [BitConverter]::GetBytes([uint64]$bstart).CopyTo($req, 16)
            [BitConverter]::GetBytes([uint64]$bend).CopyTo($req, 24)
            $pat.CopyTo($req, 32)
            $msk.CopyTo($req, 96)

            # 11 qwords = 88 bytes. Asking for less produces STATUS_BUFFER_TOO_SMALL
            # (0x7A), not a truncated reply - the driver refuses rather than lies.
            $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x82B)) `
                                     -InBytes $req -OutputLength 88

            $ac  = [BitConverter]::ToUInt64($out, 0)
            $ar  = [BitConverter]::ToUInt64($out, 8)
            $ab  = [BitConverter]::ToUInt64($out, 16)
            $ah  = [BitConverter]::ToUInt64($out, 24)
            $bc  = [BitConverter]::ToUInt64($out, 32)
            $brd = [BitConverter]::ToUInt64($out, 40)
            $bb  = [BitConverter]::ToUInt64($out, 48)
            $bh  = [BitConverter]::ToUInt64($out, 56)
            $pv  = [BitConverter]::ToUInt64($out, 64)
            $sp  = [BitConverter]::ToUInt64($out, 72)
            $wcr3 = [BitConverter]::ToUInt64($out, 80)

            Write-Host ("mem-bench: pid={0} range=0x{1:X}-0x{2:X} pattern={3} byte(s)" -f $bpid, $bstart, $bend, $patLen)
            Write-Host ("  A (api) : {0} cycles, {1} reads, {2} bytes, {3} hits" -f $ac, $ar, $ab, $ah)
            Write-Host ("  B (pte) : {0} cycles, {1} reads, {2} bytes, {3} hits  (pages visited {4})" -f $bc, $brd, $bb, $bh, $pv)
            Write-Host ("  walk    : cr3=0x{0:X}" -f $wcr3)

            # 9.491: TWO BUGS IN ONE THREE-LINE BLOCK, AND BOTH CHANGED THE VERDICT.
            #
            # (a) `$brid` WAS NEVER DEFINED ANYWHERE IN THIS FILE - the variable is
            #     `$brd` (B's read count, read from out[40] above). In PowerShell an
            #     undefined variable is $null, and `$null -eq 0` is FALSE, so the
            #     second half of the INVALID test never fired. A run where B visited
            #     pages but read ZERO bytes (a walk that resolves but every page
            #     misses) printed no warning at all.
            #
            # (b) The ratio was printed BEFORE the check, so a meaningless
            #     "0.00x" appeared above a warning that called it an artefact. The
            #     reader sees the number first and the caveat second; measured
            #     consequence in this tree's own history is that the number is what
            #     gets quoted (9.463 quoted a bogus "CPU 0%" the same way).
            #
            # Fixed by computing the verdict first, printing the ratio only when it
            # means something, and naming it INVALID otherwise.
            $invalid = ($pv -eq 0 -or $brd -eq 0 -or $bc -eq 0)

            if ($invalid) {
                if ($bc -eq 0) {
                    Write-Host '  INVALID : B measured 0 cycles, so no ratio exists.'
                } elseif ($pv -eq 0) {
                    Write-Host '  INVALID : B visited 0 pages, so the ratio is an artefact,'
                    Write-Host '            NOT a speedup. Check the walk CR3 (0 = the target'
                    Write-Host '            DTB could not be read) and the range.'
                } else {
                    Write-Host ("  INVALID : B visited {0} page(s) but read 0 bytes, so the" -f $pv)
                    Write-Host '            ratio is an artefact, NOT a speedup. The walk' 
                    Write-Host '            resolved but every page missed.'
                }
            } else {
                Write-Host ("  faster  : {0:N2}x  (B ran first, so A had the warmer cache)" -f ($sp / 100.0))
            }

            if ($ar -gt 0 -and $brd -gt 0) {
                Write-Host ("  reads   : A/B = {0:N1}x more reads for A" -f ($ar / [double]$brd))
            }
            if ($ah -ne $bh) {
                Write-Host ("  WARNING : hits DISAGREE (A={0} B={1}) - a functional finding," -f $ah, $bh)
                Write-Host  '            worth more than the timing above.'
            }
            if ($ac -gt 0 -and $bc -gt 0 -and $sp -lt 100) {
                Write-Host '  NOTE    : B was not faster here. That is a result, not a failure -'
                Write-Host '            the range has too few absent pages for the walk to pay off.'
            }
        }
        'mem-scan' {
            # Phase 4 (2026-10-11): page-table driven signature scan.
            # usage: mem-scan <pid> <hex_pattern> [max_results] [writable] [exec]
            #   pattern: hex bytes, '??' = wildcard, e.g. "48 8B 05 ?? ?? ?? ??"
            if ($null -eq $Arg1 -or $null -eq $Arg2) {
                throw 'mem-scan: usage: mem-scan <pid> <hex_pattern> [max] [writable] [exec]'
            }
            $tpid = [uint32]::Parse($Arg1)
            $toks = ("$Arg2" -split '[\s,]+') | Where-Object { $_ -ne '' }
            $patLen = $toks.Count
            if ($patLen -lt 1 -or $patLen -gt 64) { throw 'mem-scan: pattern must be 1..64 bytes' }

            $pat = New-Object byte[] 64
            $msk = New-Object byte[] 64
            for ($i = 0; $i -lt $patLen; $i++) {
                $t = $toks[$i]
                if ($t -eq '??' -or $t -eq '?') { $pat[$i] = 0; $msk[$i] = 0 }
                else { $pat[$i] = [Convert]::ToByte(($t -replace '^0[xX]',''), 16); $msk[$i] = 0xFF }
            }

            $maxR = 32
            if ($null -ne $Arg3) { $maxR = [uint32]::Parse($Arg3) }
            # Flags: bit0 writable, bit1 executable, bit2 dirty-only.
            # Arg4/Arg5 take a letter set: 'w', 'x', 'd', or a combination
            # ('wx', 'd', ...). 'x' alone keeps meaning "executable only", so
            # the letters are matched explicitly instead of "anything not x".
            $flags = 0
            $f4 = if ($null -ne $Arg4) { "$Arg4".ToLower() } else { '' }
            $f5 = if ($null -ne $Arg5) { "$Arg5".ToLower() } else { '' }
            if ($f4 -match 'w') { $flags = $flags -bor 1 }
            if ($f4 -match 'x' -or $f5 -match 'x') { $flags = $flags -bor 2 }
            if ($f4 -match 'd' -or $f5 -match 'd') { $flags = $flags -bor 4 }

            # Optional explicit range: mem-scan <pid> <pat> <max> <fl> <ex> <start> <end>
            $rStart = [uint64]0
            $rEnd   = [uint64]0
            if ($null -ne $Arg6 -and "$Arg6" -ne '' -and "$Arg6" -ne 'x') { $rStart = [Convert]::ToUInt64(("$Arg6" -replace '^0[xX]',''),16) }
            if ($null -ne $Arg7 -and "$Arg7" -ne '' -and "$Arg7" -ne 'x') { $rEnd   = [Convert]::ToUInt64(("$Arg7" -replace '^0[xX]',''),16) }

            $req = New-Object byte[] $sizeofYghvScanReq
            [BitConverter]::GetBytes($tpid).CopyTo($req, 0)
            [BitConverter]::GetBytes([uint32]$patLen).CopyTo($req, 4)
            [BitConverter]::GetBytes([uint32]$maxR).CopyTo($req, 8)
            [BitConverter]::GetBytes([uint32]$flags).CopyTo($req, 12)
            [BitConverter]::GetBytes($rStart).CopyTo($req, 16)   # range_start
            [BitConverter]::GetBytes($rEnd).CopyTo($req, 24)     # range_end
            [BitConverter]::GetBytes([uint64]0).CopyTo($req, 32) # align
            [Array]::Copy($pat, 0, $req, 40, 64)
            [Array]::Copy($msk, 0, $req, 104, 64)

            $outLen = 40 + $maxR * 8
            $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x819)) `
                                    -InBytes $req -OutputLength $outLen
            $ok    = [BitConverter]::ToUInt64($out, 0)
            $hits  = [BitConverter]::ToUInt64($out, 8)
            $pr    = [BitConverter]::ToUInt64($out, 16)
            $psk   = [BitConverter]::ToUInt64($out, 24)
            $bs    = [BitConverter]::ToUInt64($out, 32)
            if (-not $ok) { Write-Host 'mem-scan: FAILED'; return }
            Write-Host ("mem-scan: hits={0} pages_read={1} pages_skipped={2} bytes={3}" -f $hits,$pr,$psk,$bs)
            for ($i = 0; $i -lt $hits; $i++) {
                $va = [BitConverter]::ToUInt64($out, 40 + $i*8)
                Write-Host ("  hit[{0}] va=0x{1:X}" -f $i, $va)
            }
        }
        'mem-selfmap' {
            # Phase 2b (2026-10-11): self-built physical mapping window.
            #   mem-selfmap            read diagnostics
            #   mem-selfmap init|stop|reset
            #
            # The window is one reserved VA whose leaf PTE is rewritten to
            # alias a target frame, so a later access is a plain load/store -
            # no MmCopyMemory, no per-page MDL. Requires PASSIVE_LEVEL, so it
            # is set up from here rather than from the island.
            if ($null -eq $Arg1) {
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x81A)) -OutputLength 88
                $up = [BitConverter]::ToUInt64($out, 0)
                $va = [BitConverter]::ToUInt64($out, 8)
                Write-Host ("mem-selfmap: ready={0} window_va=0x{1:X}" -f $up, $va)
                Write-Host ("  inits={0} retargets={1} reads={2} writes={3} failures={4} tlb_flushes={5}" -f `
                    [BitConverter]::ToUInt64($out,16), [BitConverter]::ToUInt64($out,24),
                    [BitConverter]::ToUInt64($out,32), [BitConverter]::ToUInt64($out,40),
                    [BitConverter]::ToUInt64($out,48), [BitConverter]::ToUInt64($out,56))
                $eng = [BitConverter]::ToUInt64($out, 64)
                $engName = switch ($eng) { 1 { 'api' } 2 { 'pte' } 3 { 'pte-selfmap' } default { 'auto' } }
                Write-Host ("  engine={0}" -f $engName)
                return
            }
            $op = switch ("$Arg1".ToLower()) {
                'init'  { 1 }  'stop' { 0 }  'reset' { 2 }
                default { -1 }
            }
            if ($op -lt 0) { throw 'mem-selfmap: use init|stop|reset' }
            $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x81A)) `
                                    -InBytes ([BitConverter]::GetBytes([uint32]$op)) `
                                    -OutputLength 8
            Write-Host ("mem-selfmap: {0} OK" -f "$Arg1")
        }
                                        'mem-cache' {
            # Tier 4.7 (2026-10-17): cache behaviour control (NPT PWT/PCD).
            #
            # PWT/PCD live in the NPT entry, so they are set per GPA and the
            # guest sees them on every access through that entry. Modes:
            #   0 writeback (default)  1 write-through  2 uncached  3 uncacheable-strong
            #
            # HONEST: this changes the GUEST's behaviour too - uncached memory
            # is dramatically slower for the guest, and a target that measures
            # its own throughput can detect it. This is not a stealth feature.
            # "Per-observer" needs the split-view mechanism: set the mode in
            # the ALT table and register an observer (mem-view).
            # usage: mem-cache [set <gpa> <mode> | setalt <gpa> <mode> | reset <gpa>]
            if ($null -eq $Arg1) {
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x822)) -OutputLength 88
                Write-Host ("mem-cache: tracked={0} sets={1} resets={2} failures={3}" -f `
                    [BitConverter]::ToUInt64($out,16), [BitConverter]::ToUInt64($out,24),
                    [BitConverter]::ToUInt64($out,32), [BitConverter]::ToUInt64($out,40))
                return
            }
            $v = "$Arg1".ToLower()
            $op = -1; $a=[uint64]0; $b=[uint64]0
            switch ($v) {
                'set'     { $op = 0 }
                'setalt'  { $op = 1 }
                'reset'   { $op = 2 }
                'verify'  { $op = 3 }
                default   { throw 'mem-cache: use set|setalt|reset|verify' }
            }
            if ($null -eq $Arg2) { throw 'mem-cache: needs a hex_gpa' }
            $a = [Convert]::ToUInt64(("$Arg2" -replace '^0[xX]',''),16)
            if ($op -eq 0 -or $op -eq 1) {
                if ($null -eq $Arg3) { throw 'mem-cache set: needs a mode (0-3)' }
                $b = [uint64]::Parse($Arg3)
            }
            $req = New-Object byte[] 32
            [BitConverter]::GetBytes([uint32]$op).CopyTo($req, 0)
            [BitConverter]::GetBytes([uint64]$a).CopyTo($req, 8)
            [BitConverter]::GetBytes([uint64]$b).CopyTo($req, 16)
            if ($op -eq 3) {
                # 9.329: read the NPT entry back. "The call returned success"
                # is not evidence that PWT/PCD landed in the table.
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x822)) -InBytes $req -OutputLength 32
                if ([BitConverter]::ToUInt64($out,0)) {
                    Write-Host ("mem-cache verify: gpa=0x{0:X}  PWT={1}  PCD={2}  entry=0x{3:X}" -f `
                        $a, [BitConverter]::ToUInt64($out,8), [BitConverter]::ToUInt64($out,16),
                        [BitConverter]::ToUInt64($out,24))
                } else {
                    Write-Host 'mem-cache verify: entry not found'
                }
                return
            }
            $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x822)) -InBytes $req -OutputLength 8
            Write-Host ("mem-cache: {0} {1}" -f $v, $(if([BitConverter]::ToUInt64($out,0)){'OK'}else{'FAILED'}))
        }
        'pa-io' {
            # 9.338 (2026-10-18): direct physical-frame access.
            #
            # WHY THIS EXISTS: mem-write takes a VIRTUAL address and walks the
            # page tables. A test that wanted to touch a physical frame had no
            # entry point and handed a PHYSICAL address to mem-write instead -
            # which crashed the machine twice:
            #     0x3B  Engine A faults, the "VA" is not mapped
            #     0x1A  the memory manager then tries to make it resident
            # Rather than keep abusing an interface, this one says what it does.
            #
            # usage:
            #   pa-io read <hex_pa> [len] [off]
            #   pa-io write <hex_pa> <u64 value>
            if ($null -eq $Arg1 -or $null -eq $Arg2) { throw 'pa-io: usage: pa-io read <hex_pa> [len] [off] | pa-io write <hex_pa> <value>' }
            $v = "$Arg1".ToLower()
            $pa = [Convert]::ToUInt64(("$Arg2" -replace '^0[xX]',''),16)
            $req = New-Object byte[] 32
            [BitConverter]::GetBytes([uint64]$pa).CopyTo($req, 8)
            if ($v -eq 'read') {
                $len = 64; $off = 0
                if ($null -ne $Arg3) { $len = [int]::Parse($Arg3) }
                if ($null -ne $Arg4) { $off = [int]::Parse($Arg4) }
                [BitConverter]::GetBytes([uint64]$len).CopyTo($req, 16)
                [BitConverter]::GetBytes([uint64]$off).CopyTo($req, 24)
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x827)) -InBytes $req -OutputLength 264
                $got = [BitConverter]::ToUInt64($out, 0)
                Write-Host ("pa-io read: 0x{0:X} -> {1} byte(s)" -f $pa, $got)
                if ($got -gt 0) {
                    $n = [Math]::Min(32, [int]$got)
                    Write-Host ("  " + (($out[8..(8+$n-1)] | ForEach-Object { $_.ToString('X2') }) -join ' '))
                }
                return
            }
            if ($v -eq 'write') {
                if ($null -eq $Arg3) { throw 'pa-io write: usage: pa-io write <hex_pa> <value>' }
                $val = [Convert]::ToUInt64(("$Arg3" -replace '^0[xX]',''),16)
                [BitConverter]::GetBytes([uint64]1).CopyTo($req, 0)
                [BitConverter]::GetBytes([uint64]$val).CopyTo($req, 16)
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x827)) -InBytes $req -OutputLength 8
                Write-Host ("pa-io: write 0x{0:X} <- 0x{1:X} : {2}" -f $pa, $val, $(if([BitConverter]::ToUInt64($out,0)){'OK'}else{'FAILED'}))
                return
            }
            throw 'pa-io: use read|write'
        }
        'sys-crypto' {
            # Tier 7.3 (9.342): SHA-256 self-test.
            #
            # The integrity monitor's whole claim is "this object did not
            # change". If the hash itself is silently wrong, that claim is
            # false in the worst way: it says "unchanged" forever. These are
            # the published FIPS 180-4 vectors (empty, "abc", and a 56-byte
            # multi-block case that exercises the padding-into-a-new-block
            # path), checked at runtime rather than trusted.
            $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x826)) -OutputLength 64
            $ok = [BitConverter]::ToUInt64($out, 56)
            Write-Host ("sys-crypto: SHA-256 selftest " + $(if($ok -eq 1){'PASS (fips-180-4 vectors match)'}else{'FAIL - hashes are NOT trustworthy'}))
        }
        'sys-sentry' {
            # Tier 7.4/7.5/7.6 (2026-10-18): three sides of one subject - how
            # the hypervisor behaves when someone is looking at it or attacking.
            #
            # 7.4 WHAT A GUEST CAN MEASURE ABOUT US
            #   Time-based detection is the probe a guest can always run on
            #   itself: rdtsc around a hypervisor call, cpuidex leaf 1 /
            #   0x40000000. The cost of a VMEXIT is what leaks. This keeps a
            #   running worst case so a regression that makes us slow - and
            #   therefore loud - is visible here first.
            #   HONEST LIMIT: this measures and reports. Hiding the cost needs
            #   TSC scaling, which Tier 5 (5.1) owns and is a much larger change.
            #
            # 7.5 WHAT IS BEING TRIED ON US
            #   Every value reaching an emulation path is untrusted: MSR index,
            #   port number, CR value, VMCB field. Rejections are recorded with
            #   context instead of being silently clamped.
            #
            # 7.6 WHAT WE EXPOSE
            #   The cheapest defence is not having the feature. This lists what
            #   is currently intercepted and which IOCTLs are accepted, so "what
            #   can be attacked" is a list that can be read rather than a claim.
            #
            # usage:
            #   sys-sentry                    counters + timing
            #   sys-sentry surface            current intercept/IOCTL inventory
            #   sys-sentry reset
            if ($null -eq $Arg1) {
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x828)) -OutputLength 112
                $names = @('msr-index','io-port','cr-value','vmcb-field','ioctl-arg','guarded-page')
                $tot = [BitConverter]::ToUInt64($out,48)
                Write-Host ("sys-sentry: escapes={0}" -f $tot)
                for ($i=0; $i -lt 6; $i++) {
                    $v = [BitConverter]::ToUInt64($out, $i*8)
                    if ($v -gt 0) { Write-Host ("  {0,-14} {1}" -f $names[$i], $v) }
                }
                if ($tot -gt 0) {
                    $lk = [BitConverter]::ToUInt64($out,56)
                    Write-Host ("  last: {0} detail=0x{1:X} a=0x{2:X} b=0x{3:X}" -f `
                        $(if($lk -lt 6){$names[[int]$lk]}else{"kind=$lk"}), `
                        [BitConverter]::ToUInt64($out,64), [BitConverter]::ToUInt64($out,72),
                        [BitConverter]::ToUInt64($out,80))
                }
                $exits = [BitConverter]::ToUInt64($out,88)
                Write-Host ("  exit timing: measured={0} worst={1} cycles  avg={2}" -f `
                    $exits, [BitConverter]::ToUInt64($out,96), `
                    $(if($exits -gt 0){[Math]::Round([BitConverter]::ToUInt64($out,104) / $exits, 0)}else{0}))
                if ($exits -eq 0) {
                    Write-Host "  (no exit measurements yet - the VMEXIT path records them)"
                }
                return
            }
            $v = "$Arg1".ToLower()
            if ($v -eq 'reset') {
                $req = New-Object byte[] 32
                [BitConverter]::GetBytes([uint64]2).CopyTo($req, 0)
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x828)) -InBytes $req -OutputLength 8
                Write-Host 'sys-sentry: counters reset'
                return
            }
            if ($v -eq 'surface') {
                $req = New-Object byte[] 32
                [BitConverter]::GetBytes([uint64]1).CopyTo($req, 0)
                # 9.366: the inventory is populated from the intercept mask (9
                # entries) plus one per IOCTL code actually exercised (41). The
                # old 256-byte request could only carry 10 entries, so a full
                # inventory would have been TRUNCATED SILENTLY - the count would
                # have said 10 and the list would have looked complete.
                # 64 entries * 24 bytes + 8 header = 1544.
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x828)) -InBytes $req -OutputLength 1544
                $n = [BitConverter]::ToUInt64($out, 0)
                Write-Host ("sys-sentry surface: {0} entry(s)" -f $n)
                for ($i = 0; $i -lt $n; $i++) {
                    $o = 8 + $i*24
                    $nm = -join (0..11 | ForEach-Object { $ch = $out[$o + $_]; if ($ch -ne 0) { [char]$ch } })
                    $meta = [BitConverter]::ToUInt64($out, $o + 16)
                    $kind = if ($meta -band 1) { 'ioctl' } else { 'intercept' }
                    Write-Host ("  {0,-10} {1,-16} hits={2}" -f $kind, $nm.Trim(), ($meta -shr 8))
                }
                if ($n -eq 0) {
                    Write-Host '  (empty: no VMRUN has run on this boot, so no core has armed an intercept yet)'
                }
                return
            }
            if ($v -eq 'cpuid-view') {
                # 9.368 (A4, Tier 5.4): what CPUID tells the guest about us.
                #
                # Levels:
                #   0 honest          hypervisor bit SET, vendor string answers
                #                     (this is what the code always did)
                #   1 no-vendor-bit   bit 31 CLEARED, leaves 0x40000000+ still
                #                     answer a vendor string
                #   2 off             bit 31 cleared AND leaves answer zero,
                #                     i.e. "no hypervisor here"
                #
                # The field that cannot be faked: `altered`. A policy that is
                # set but never fires leaves it at 0 while the level still reads
                # back correctly, so the readout distinguishes "configured" from
                # "in effect".
                if ($null -eq $Arg2) {
                    $r0 = New-Object byte[] 32
                    [BitConverter]::GetBytes([uint64]3).CopyTo($r0, 0)
                    $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x828)) -InBytes $r0 -OutputLength 24
                    $lvl = [BitConverter]::ToUInt64($out, 0)
                    $sets = [BitConverter]::ToUInt64($out, 8)
                    $alt = [BitConverter]::ToUInt64($out, 16)
                    # A nested switch inside an expression is legal but the
                    # default arm's string interpolation is where this tripped
                    # the 5.1 parser. Build the name with if/elseif instead: no
                    # nested pipeline assignment, no interpolation.
                    $nm = 'unknown(' + $lvl + ')'
                    if ($lvl -eq 0) { $nm = 'honest (bit31 set, vendor answers)' }
                    elseif ($lvl -eq 1) { $nm = 'no-vendor-bit (bit31 cleared, vendor still answers)' }
                    elseif ($lvl -eq 2) { $nm = 'off (bit31 cleared, leaves answer zero)' }
                    Write-Host ("sys-sentry cpuid-view: {0}" -f $nm)
                    Write-Host ("  set {0} time(s); CPUID answers actually altered: {1}" -f $sets, $alt)
                    if ($sets -gt 0 -and $alt -eq 0 -and $lvl -ne 0) {
                        Write-Host '  NOTE: the level is not honest but nothing was altered'
                        Write-Host '        either no CPUID left 1/0x40000000+ has run since, or'
                        Write-Host '        the policy is set on a core that never exits.'
                    }
                    return
                }
                $want = [int]$Arg2
                if ($want -lt 0 -or $want -gt 2) { throw 'sys-sentry cpuid-view: level must be 0, 1 or 2' }
                $r2 = New-Object byte[] 32
                [BitConverter]::GetBytes([uint64]4).CopyTo($r2, 0)
                [BitConverter]::GetBytes([uint64]$want).CopyTo($r2, 8)
                $o2 = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x828)) -InBytes $r2 -OutputLength 8
                $ok = [BitConverter]::ToUInt64($o2, 0)
                Write-Host ("sys-sentry cpuid-view: set to {0} -> {1}" -f $want, $(if($ok){'OK'}else{'REJECTED (out of range)'}))
                return
            }
            if ($v -eq 'tsc') {
                # 9.368 (B1/B2, Tier 5.1): the guest's clock.
                #
                #   sys-sentry tsc                 read policy + counters
                #   sys-sentry tsc offset <hex>    set a constant shift
                #   sys-sentry tsc freeze 1|0      freeze / thaw
                #
                # WHAT EACH CAN DO, because they are not interchangeable:
                #   offset  hardware adds it to every RDTSC. NO exit, no cost.
                #           It hides WHEN, not HOW LONG: a measured interval is
                #           unchanged because a constant cancels in a subtraction.
                #   freeze  needs the RDTSC intercept (one exit per read) and makes
                #           every reading identical, so intervals become 0.
                if ($null -eq $Arg2) {
                    $r3 = New-Object byte[] 32
                    [BitConverter]::GetBytes([uint64]5).CopyTo($r3, 0)
                    $o3 = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x828)) -InBytes $r3 -OutputLength 136
                    $offs = [BitConverter]::ToUInt64($o3, 0)
                    $mode = [BitConverter]::ToUInt64($o3, 8)
                    $froze = [BitConverter]::ToUInt64($o3, 16)
                    $icpt = [BitConverter]::ToUInt64($o3, 24)
                    $supp = [BitConverter]::ToUInt64($o3, 32)
                    $armed = [BitConverter]::ToUInt64($o3, 40)
                    $real = [BitConverter]::ToUInt64($o3, 48)
                    $mn = 'normal'
                    if ($mode -eq 1) { $mn = 'FROZEN' }
                    Write-Host ("sys-sentry tsc: offset=0x{0:X} mode={1}" -f $offs, $mn)
                    Write-Host ("  rdtsc exits handled={0}  answers changed={1}" -f $icpt, $supp)
                    Write-Host ("  intercept bit armed={0}  real rdtsc now={1}" -f $armed, $real)
                    # 9.369: scoping fields. Without them "FROZEN" was ambiguous:
                    # it could mean the OS clock was held, which is what hung the
                    # machine, or that one address space was.
                    $scope = 0; $exp = 0; $expired = 0; $sco = 0
                    if ($o3.Length -ge 136) {
                        $scope   = [BitConverter]::ToUInt64($o3, 56)
                        $exp     = [BitConverter]::ToUInt64($o3, 64)
                        $expired = [BitConverter]::ToUInt64($o3, 72)
                        $sco     = [BitConverter]::ToUInt64($o3, 80)
                    }
                    Write-Host ("  scope cr3=0x{0:X} expires-at={1}" -f $scope, $exp)
                    $budget = 0; $budgetHits = 0
                    if ($o3.Length -ge 136) {
                        $budget     = [BitConverter]::ToUInt64($o3, 88)
                        $budgetHits = [BitConverter]::ToUInt64($o3, 96)
                    }
                    Write-Host ("  self-thawed={0}  reads let through (not in scope)={1}" -f $expired, $sco)
                    $selfdis = 0
                    if ($o3.Length -ge 136) { $selfdis = [BitConverter]::ToUInt64($o3, 104) }
                    Write-Host ("  exit budget left={0}  freezes cut short by budget={1}" -f $budget, $budgetHits)
                    Write-Host ("  cores that disarmed themselves={0}" -f $selfdis)
                    $walkFail = 0
                    if ($o3.Length -ge 136) { $walkFail = [BitConverter]::ToUInt64($o3, 112) }
                    Write-Host ("  per-core walk failures={0}" -f $walkFail)
                    $hits = 0; $misses = 0
                    if ($o3.Length -ge 136) {
                        $hits   = [BitConverter]::ToUInt64($o3, 120)
                        $misses = [BitConverter]::ToUInt64($o3, 128)
                    }
                    Write-Host ("  walk: cores reached={0} cores missed={1}" -f $hits, $misses)
                    if ($walkFail -gt 0) {
                        Write-Host '  WARNING: a per-core walk did not complete. Some cores may'
                        Write-Host '           still be armed - that is the 0x101 condition.'
                    }
                    if ($mode -eq 1 -and $scope -eq 0) {
                        Write-Host '  WARNING: frozen with NO scope - this shape is refused by'
                        Write-Host '           the setter now, so seeing it means an older driver.'
                    }
                    if ($mode -eq 1 -and $armed -eq 0) {
                        Write-Host '  WARNING: frozen but the intercept is NOT armed'
                        Write-Host '           the guest reads hardware and never asks us, so'
                        Write-Host '           the freeze will not take effect until a core re-enters'
                        Write-Host '           prepare (CPUID handshake).'
                    }
                    if ($mode -eq 0 -and $offs -eq 0 -and $armed -eq 0) {
                        Write-Host '  (nothing set: the guest reads the real clock, no exits)'
                    }
                    return
                }
                $what = "$Arg2".ToLower()
                if ($what -eq 'offset') {
                    if ($null -eq $Arg3) { throw 'sys-sentry tsc offset: a hex value is required' }
                    $ov = [Convert]::ToUInt64(("$Arg3" -replace '^0[xX]',''),16)
                    $r4 = New-Object byte[] 32
                    [BitConverter]::GetBytes([uint64]6).CopyTo($r4, 0)
                    [BitConverter]::GetBytes([uint64]$ov).CopyTo($r4, 8)
                    $o4 = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x828)) -InBytes $r4 -OutputLength 8
                    Write-Host ("sys-sentry tsc offset: set to 0x{0:X} -> {1}" -f $ov, $(if([BitConverter]::ToUInt64($o4,0)){'OK'}else{'REJECTED'}))
                    return
                }
                if ($what -eq 'freeze') {
                    # 9.369: THE SHAPE OF THIS COMMAND CHANGED BECAUSE THE OLD ONE
                    # HUNG THE MACHINE.
                    #
                    # The first freeze had no scope and no deadline: it stopped
                    # every RDTSC, including the ones Windows uses for its
                    # scheduler and timers, and the box stopped making progress
                    # with no bugcheck and no dump. Manual reboot.
                    #
                    # So freeze now REQUIRES a cr3 and a deadline, and refuses to
                    # run without them. The deadline is real milliseconds, capped
                    # at 10000, after which the driver thaws itself.
                    #
                    #   sys-sentry tsc freeze <cr3> [max_ms]
                    #   sys-sentry tsc freeze 0            thaw
                    if ($null -eq $Arg3) { throw 'sys-sentry tsc freeze: a cr3 is required, or the word thaw' }
                    $ms = 2000
                    if ($null -ne $Arg4) { $ms = [int]$Arg4 }
                    # 9.369: `thaw` is an EXPLICIT intent, not "cr3 == 0".
                    #
                    # The first version treated a cr3 of 0 as thaw. But 0 is also
                    # the value a test passes to check that a GLOBAL freeze is
                    # refused, so that check was silently answered with a thaw and
                    # the safety gate could never fire. Two intents, two tokens:
                    #   freeze thaw            -> thaw (on=0)
                    #   freeze 0x0 [ms]        -> forwarded as a freeze request, so
                    #                             the DRIVER's refusal is the answer
                    #
                    # 9.369c: and the token is checked BEFORE the hex parse. The
                    # first version parsed "$Arg3" as a number first, so the literal
                    # word `thaw` threw FormatException instead of thawing - the
                    # script's own thaw was broken by the check meant to make it
                    # explicit.
                    if ("$Arg3" -eq 'thaw') {
                        $r5 = New-Object byte[] 32
                        [BitConverter]::GetBytes([uint64]7).CopyTo($r5, 0)
                        [BitConverter]::GetBytes([uint64]0).CopyTo($r5, 8)
                        $o5 = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x828)) -InBytes $r5 -OutputLength 8
                        Write-Host ("sys-sentry tsc freeze: THAW -> {0}" -f $(if([BitConverter]::ToUInt64($o5,0)){'OK'}else{'REJECTED'}))
                        return
                    }
                    $cr3 = [Convert]::ToUInt64(("$Arg3" -replace '^0[xX]',''),16)
                    if ($ms -lt 1)  { $ms = 1 }
                    if ($ms -gt 10000) { $ms = 10000 }
                    $r5 = New-Object byte[] 32
                    [BitConverter]::GetBytes([uint64]7).CopyTo($r5, 0)
                    [BitConverter]::GetBytes([uint64]1).CopyTo($r5, 8)
                    [BitConverter]::GetBytes([uint64]$cr3).CopyTo($r5, 16)
                    [BitConverter]::GetBytes([uint64]$ms).CopyTo($r5, 24)
                    $o5 = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x828)) -InBytes $r5 -OutputLength 8
                    $rc = [BitConverter]::ToUInt64($o5, 0)
                    if ($rc) {
                        Write-Host ("sys-sentry tsc freeze: cr3=0x{0:X} for {1}ms -> OK (self-thaws at the deadline)" -f $cr3, $ms)
                    } else {
                        Write-Host ("sys-sentry tsc freeze: cr3=0x{0:X} -> REFUSED" -f $cr3)
                        Write-Host '  A cr3 of 0 means "every address space", which stops the OS'
                        Write-Host '  clock and hung this machine once (9.369). Pass the address'
                        Write-Host '  space to hold instead.'
                    }
                    return
                }
                if ($what -eq 'probe') {
                    # 9.370c: walk every core and VALIDATE the VPD pointer, arming
                    # NOTHING. 0x3B at SvTscApplyOnCore+0x36 came from dereferencing
                    # a physical address as a pointer, so the fix gets checked by a
                    # path that touches nothing even if the pointer is still wrong.
                    $rp = New-Object byte[] 32
                    [BitConverter]::GetBytes([uint64]8).CopyTo($rp, 0)
                    $op2 = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x828)) -InBytes $rp -OutputLength 24
                    $hits   = [BitConverter]::ToUInt64($op2, 0)
                    $misses = [BitConverter]::ToUInt64($op2, 8)
                    $st     = [BitConverter]::ToUInt64($op2, 16)
                    Write-Host ("sys-sentry tsc probe: cores reached={0} missed={1} walk={2}" -f $hits, $misses, $(if($st){'completed'}else{'FAILED'}))
                    if ($hits -eq 0) {
                        Write-Host '  No core answered the VPD handshake, so no walk can'
                        Write-Host '  reach a VMCB. Arming would be a no-op (or a crash).'
                    }
                    return
                }
                throw 'sys-sentry tsc: offset takes a hex value, freeze takes 1 or 0, or probe'
            }
            throw 'sys-sentry: use surface, reset, cpuid-view, or tsc'
        }
        'sys-bp-int3' {
            # 9.372 (C1): the #BP exception intercept, opt-in.
            #
            # WHY A SWITCH AND NOT ALWAYS-ON: measured in 9.367.3, bit 3 of
            # InterceptException was never set, so `case VMEXIT_EXCEPTION_BP` has
            # never executed. Turning an exception intercept on has already cost
            # this project a bugcheck once (#GP, PatchGuard, 0xE2 - 9.244). The
            # default boot path stays exactly as the 13/13 suite verified it; a
            # test asks for the intercept, measures the chain, and turns it off.
            #
            # usage:
            #   sys-bp-int3          state
            #   sys-bp-int3 on|off
            if ($null -eq $Arg1) {
                $r0 = New-Object byte[] 32
                [BitConverter]::GetBytes([uint64]9).CopyTo($r0, 0)
                $o = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x828)) -InBytes $r0 -OutputLength 8
                $v = [BitConverter]::ToUInt64($o, 0)
                Write-Host ("sys-bp-int3: intercept={0} ({1})" -f $v, $(if($v){'#BP exits are ours'}else{'default: the guest handles its own #BP'}))
                if ($v -and $v -ne 1) { Write-Host '  WARNING: reported value is not 0/1' }
                return
            }
            $w = "$Arg1".ToLower()
            if ($w -ne 'on' -and $w -ne 'off') { throw 'sys-bp-int3: use on or off' }
            $req = New-Object byte[] 32
            [BitConverter]::GetBytes([uint64]10).CopyTo($req, 0)
            [BitConverter]::GetBytes([uint64]$(if($w -eq 'on'){1}else{0})).CopyTo($req, 8)
            $o2 = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x828)) -InBytes $req -OutputLength 16
            $ok = [BitConverter]::ToUInt64($o2, 0)
            $walk = 0
            if ($o2.Length -ge 16) { $walk = [BitConverter]::ToUInt64($o2, 8) }
            # 9.374: TWO SEPARATE OUTCOMES, reported separately on purpose.
            #   flag set   the policy now says "intercept #BP"
            #   walk done  every core's VMCB was updated at runtime
            # The walk is DISABLED (it cost two restarts - see the note at
            # yghv_s206_bp_int3_apply_all), so `walk=0` is the expected answer and
            # NOT a failure. The flag takes effect at the next load, where prepare
            # arms it per core.
            if ($ok) {
                Write-Host ("sys-bp-int3: policy flag -> {0}" -f $(if($w -eq 'on'){'ON'}else{'OFF'}))
            } else {
                Write-Host ("sys-bp-int3: {0} -> REFUSED" -f $w)
            }
            Write-Host ("  runtime per-core walk: {0}" -f $(if($walk){'applied to every core'}else{'DISABLED - takes effect at next load'}))
            if (-not $walk -and $w -eq 'on') {
                Write-Host '  The flag is set but the VMCB bits are NOT: a core keeps its old'
                Write-Host '  mask until it re-enters prepare, which only happens at load. So for'
                Write-Host '  #BP interception to be live, set this flag BEFORE loading the driver.'
            }
        }
        'unload-trace' {
            # 9.390: the unload progress record.
            #
            # WHY THIS EXISTS: five bugchecks died on the unload path with nothing
            # observable afterwards. Two scripts wrote a file as their FIRST action
            # and the file did not appear - a user-mode write cannot survive. So the
            # driver stamps its teardown steps into a page that is never freed and
            # never hidden, and a crash dump carries the last stamp.
            #
            # Reading it here proves the mechanism works on a LIVE machine; if this
            # returns step=0, no dump would show anything either.
            $req = New-Object byte[] 32
            [BitConverter]::GetBytes([uint64]7).CopyTo($req, 0)
            $o = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x827)) -InBytes $req -OutputLength 24
            $pa   = [BitConverter]::ToUInt64($o, 0)
            $step = [BitConverter]::ToUInt32($o, 8)
            $val  = [BitConverter]::ToUInt64($o, 16)
            Write-Host ("unload-trace: page PA=0x{0:X}  last step={1}  value={2}" -f $pa, $step, $val)
            if ($step -eq 0) {
                Write-Host '  step=0 means nothing has been stamped yet: a driver that'
                Write-Host '  has only loaded never runs DriverUnload. This is expected'
                Write-Host '  before any unload attempt.'
            }
            if ($pa -eq 0) {
                Write-Host '  PA=0 means the page could not be resolved, so a dump would'
                Write-Host '  not find it either. That is a real problem for the record.'
            }
        }
        'npt-range' {
            # 9.384: the physical addresses this driver allocated for NPT tables.
            #
            # WHY THIS EXISTS: three recent bugchecks faulted on two ADJACENT pages
            # that looked like NPT tables, and the .map could not name the crashing
            # instruction (an import thunk table sits right after 0x24120, making
            # every offset in that range ambiguous). This turns "does it look like
            # NPT" into arithmetic against what was actually allocated.
            #
            # usage:
            #   npt-range            count, lowest and highest NPT page
            #   npt-range <hexpa>    is this address one of ours?
            if ($null -eq $Arg1) {
                $req = New-Object byte[] 32
                [BitConverter]::GetBytes([uint64]5).CopyTo($req, 0)
                $o = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x827)) -InBytes $req -OutputLength 24
                $cnt = [BitConverter]::ToUInt64($o, 0)
                $lo  = [BitConverter]::ToUInt64($o, 8)
                $hi  = [BitConverter]::ToUInt64($o, 16)
                Write-Host ("npt-range: {0} page(s) allocated" -f $cnt)
                if ($cnt -gt 0) {
                    Write-Host ("  lowest  0x{0:X}" -f $lo)
                    Write-Host ("  highest 0x{0:X}" -f $hi)
                }
                return
            }
            $pa = [Convert]::ToUInt64(("$Arg1" -replace '^0[xX]',''),16)
            $req2 = New-Object byte[] 32
            [BitConverter]::GetBytes([uint64]6).CopyTo($req2, 0)
            [BitConverter]::GetBytes([uint64]$pa).CopyTo($req2, 8)
            $o2 = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x827)) -InBytes $req2 -OutputLength 8
            $ours = [BitConverter]::ToUInt64($o2, 0)
            Write-Host ("npt-range: 0x{0:X} -> {1}" -f $pa, $(if($ours){'IS one of our NPT pages'}else{'not an NPT page'}))
        }
        'handler-cost' {
            # 9.521: how many TSC ticks ONE #VMEXIT costs inside SvHandleVmExit.
            #
            # usage: handler-cost            read the window since the last reset
            #        handler-cost reset      read, then start a new window
            #
            # WHY IT EXISTS: 9.520 measured 22,700 exits/s across twelve cores and
            # throughput 6.3x slower than unloaded. What was never measured is the
            # cost of ONE exit inside the handler - and every optimisation idea
            # depends on it. CPU utilisation cannot see inside a function.
            #
            # READ THIS BEFORE BELIEVING THE NUMBER:
            #   * the total INCLUDES the instrumentation (2 TSC reads + 1 interlocked
            #     add per exit). It is not subtracted - that correction was never
            #     measured either, and an invented correction reads as precision.
            #   * sample count is NOT exit count. The sanity gate can skip a sample,
            #     so dividing by your own exit tally mixes two denominators.
            #   * TSC ticks at a FIXED rate regardless of core P-state. This machine
            #     runs a pinned 3600 MHz, so ticks == cycles here - and the command
            #     computes the frequency from the reply rather than assuming it.
            $op = [uint64]0
            if ($null -ne $Arg1 -and "$Arg1".ToLower() -eq 'reset') { $op = [uint64]1 }

            $req = New-Object byte[] 8
            [BitConverter]::GetBytes([uint64]$op).CopyTo($req, 0)
            $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x835)) `
                                    -InBytes $req -OutputLength 24

            $total = [BitConverter]::ToUInt64($out, 0)
            $count = [BitConverter]::ToUInt64($out, 8)
            $tsc   = [BitConverter]::ToUInt64($out, 16)

            $mode = if ($op -eq 1) { 'reset' } else { 'read' }
            Write-Host ("handler-cost: mode={0}  total={1} ticks  samples={2}" -f $mode, $total, $count)
            Write-Host ("  tsc now = {0}" -f $tsc)

            if ($count -eq 0) {
                Write-Host '  no samples yet - no #VMEXIT has completed since the last reset.'
                Write-Host '  That is a reading, not a default.'
                return
            }

            $per = $total / $count
            # ticks -> time needs the TSC frequency, which is NOT the core clock.
            # Report per-exit ticks and let the caller divide by a measured rate.
            Write-Host ("  per exit = {0:N0} ticks (this includes the instrumentation)" -f $per)
            if ($per -lt 1000) {
                Write-Host ("             = {0:N1} us at a 3.6 GHz TSC" -f ($per / 3600.0))
            } elseif ($per -lt 1000000) {
                Write-Host ("             = {0:N1} us at a 3.6 GHz TSC" -f ($per / 3600.0))
            } else {
                Write-Host ("             = {0:N2} ms at a 3.6 GHz TSC" -f ($per / 3600000.0))
            }
            $lifetime = 0.0
            if ($count -gt 0) { $lifetime = ($total / 3.6e9) }
            Write-Host ("  total CPU time inside the handler = {0:N3} s over {1:N0} exits" -f $lifetime, $count)
        }
        'exit-codes' {
            # 9.515: which exit codes are ACTUALLY firing, per code.
            #
            # `exit-mask` prints `distinct observed = 2` and never says which two
            # (control_device.c:1747 states the reason: CPUID and VMMCALL are both
            # armed by default, so the count of DISTINCT codes is 2 either way).
            # That made "the rest is CPUID" an inference. This command replaces it
            # with counters.
            #
            # Read-only. Prints only non-zero codes; the number of rows shown is
            # stated so an empty result cannot be mistaken for a command that did
            # not run.
            $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x834)) -OutputLength 800

            $base = [BitConverter]::ToUInt64($out, 0)
            $n    = [BitConverter]::ToUInt64($out, 8)

            if ($n -eq 0) {
                Write-Host 'exit-codes: the driver returned no rows - the reply did not fit'
                return
            }

            # Names for the codes this tree handles. Anything not listed is still
            # printed (with its number) - an unlabelled code that is firing is
            # exactly the kind of thing this command exists to reveal.
            $names = @{
                0x40='CR0_READ'; 0x41='CR1_READ'; 0x42='CR2_READ'; 0x43='CR3_READ'
                0x44='CR4_READ'; 0x45='CR5_READ'; 0x46='CR6_READ'; 0x47='CR7_READ'
                0x48='CR8_READ'; 0x49='CR9_READ'; 0x4A='CR10_READ'; 0x4B='CR11_READ'
                0x4C='CR12_READ'; 0x4D='CR13_READ'; 0x4E='CR14_READ'; 0x4F='CR15_READ'
                0x50='CR0_WRITE'; 0x51='CR1_WRITE'; 0x52='CR2_WRITE'; 0x53='CR3_WRITE'
                0x54='CR4_WRITE'; 0x55='CR5_WRITE'; 0x56='CR6_WRITE'; 0x57='CR7_WRITE'
                0x58='CR8_WRITE'; 0x59='CR9_WRITE'; 0x5A='CR10_WRITE'; 0x5B='CR11_WRITE'
                0x5C='CR12_WRITE'; 0x5D='CR13_WRITE'; 0x5E='CR14_WRITE'; 0x5F='CR15_WRITE'
                0x60='DR0_READ'; 0x61='DR1_READ'; 0x62='DR2_READ'; 0x63='DR3_READ'
                0x64='DR6_READ'; 0x65='DR7_READ'; 0x66='DR8_READ'; 0x67='DR9_READ'
                0x68='DR10_READ'; 0x69='DR11_READ'; 0x6A='DR12_READ'; 0x6B='DR13_READ'
                0x6C='DR14_READ'; 0x6D='DR15_READ'; 0x6E='DR0_WRITE'; 0x6F='DR1_WRITE'
                0x70='DR2_WRITE'; 0x71='DR3_WRITE'; 0x72='DR6_WRITE'; 0x73='DR7_WRITE'
                0x74='DR8_WRITE'; 0x75='DR9_WRITE'; 0x76='DR10_WRITE'; 0x77='DR11_WRITE'
                0x78='DR12_WRITE'; 0x79='DR13_WRITE'; 0x7A='DR14_WRITE'; 0x7B='DR15_WRITE'
                0x7C='EXCP_BASE'; 0x7D='EXCP_DB'; 0x7E='EXCP_BP'; 0x7F='EXCP_UD'
                0x80='INTR'; 0x81='VMMCALL'; 0x82='VMRUN'; 0x83='VMLOAD'
                0x84='VMSAVE'; 0x85='STGI'; 0x86='CLGI'; 0x87='SKINIT'
                0x88='ICEBP'; 0x89='INVD'; 0x8A='WBINVD'; 0x8B='PAUSE'
                0x8C='HLT'; 0x8D='SHUTDOWN'
            }

            Write-Host ("exit-codes: base=0x{0:X}  rows={1}" -f $base, $n)
            $shown = 0
            $total = [uint64]0
            for ($i = 0; $i -lt [int]$n; $i++) {
                $v = [BitConverter]::ToInt64($out, 16 + $i * 8)
                $code = [uint64]$base + [uint64]$i
                if ($v -gt 0) { $total += [uint64]$v }
                if ($v -le 0) { continue }
                $nm = if ($names.ContainsKey([int]$code)) { $names[[int]$code] } else { '(unlabelled)' }
                Write-Host ("  0x{0:X2}  {1,-12} {2}" -f $code, $nm, $v)
                $shown++
            }
            Write-Host ("  summary: {0} non-zero code(s) of {1}   sum={2}" -f $shown, $n, $total)
            if ($shown -eq 0) {
                Write-Host '  NOTE: nothing has fired yet in this load. That is a reading,'
                Write-Host '        not a default - it means no exit occurred since prepare.'
            }
        }
        'exit-mask' {
            # 9.448: exit-coverage control and measurement.
            #
            # The 206 handler covers 35 exit codes. Which of them are ARMED
            # decides which can ever fire, and a case whose bit is not armed can
            # never fire - so "the case exists" and "the case works" differ by
            # exactly the counters this command reads.
            #
            # usage: exit-mask                     (read back)
            #        exit-mask quiet                 (arm HLT + PAUSE)
            #        exit-mask full                  (arm every defined bit)
            #        exit-mask set <m1hex> <m2hex>   (arm an explicit mask)
            #        exit-mask clear                 (back to the default set)
            #
            # THE MASK IS NOT LIVE: the bits are OR'd into each VMCB at prepare
            # time, so a new mask takes effect on the next load. Said out loud
            # rather than silently half-applied.
            $op = 0; $m1 = [uint64]0; $m2 = [uint64]0; $ap = [int64]-1
            # -not [string]::IsNullOrWhiteSpace, not just $null -eq: an explicit
            # empty argument (``exit-mask ''``) must read back, not fall through
            # to the "unknown option" throw.
            if (-not [string]::IsNullOrWhiteSpace("$Arg1")) {
                # Deliberately NOT a switch: the parity checker derives the
                # command list from lines shaped like `'name' {`, so a switch
                # whose cases look like commands would report those words as
                # commands of this client. if/elseif cannot be misread that way.
                $v = "$Arg1".ToLower()
                if ($v -eq 'quiet') { $op = 3 }
                elseif ($v -eq 'full') { $op = 4 }
                elseif ($v -eq 'events') { $op = 5 }
                elseif ($v -eq 'host-insn') { $op = 6 }
                elseif ($v -eq 'clear') { $op = 2 }
                elseif ($v -eq 'explicit') {
                    $op = 1
                    if ($null -eq $Arg2 -or $null -eq $Arg3) {
                        throw 'exit-mask: explicit needs <m1hex> <m2hex>'
                    }
                    $m1 = [Convert]::ToUInt64(($Arg2 -replace '^0[xX]',''), 16)
                    $m2 = [Convert]::ToUInt64(($Arg3 -replace '^0[xX]',''), 16)
                }
                else {
                    throw 'exit-mask: use quiet|full|clear|explicit <m1> <m2>'
                }
            }

            if ($op -ne 0) {
                $req = New-Object byte[] 16
                [BitConverter]::GetBytes([uint32]$op).CopyTo($req, 0)
                [BitConverter]::GetBytes([uint32]$m1).CopyTo($req, 8)
                [BitConverter]::GetBytes([uint32]$m2).CopyTo($req, 12)
                # 9.482: 88 BYTES, NOT 40 - the driver now replies with 11 qwords.
                #
                # YGHV_EXITMASK_OUT_QWORDS grew 5 -> 6 -> 11 as the vmmcall
                # counter and the guest-state block were added. The read-back call
                # below was updated each time; THIS call was not. Setting a mask
                # therefore asked for 40 bytes, the driver's `out_len >= 11 qwords`
                # test failed, and it returned STATUS_BUFFER_TOO_SMALL - so
                # `applied` (out[4]) was never delivered and `exit-mask quiet`
                # reported a failure it had actually performed.
                #
                # Both calls use the same size now because they hit the same
                # handler, whose reply shape does not depend on the op.
                $rr = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x82A)) `
                                       -InBytes $req -OutputLength 88
                $ap = [BitConverter]::ToInt64($rr, 32)
            }

            $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x82A)) -OutputLength 88
            $rm1 = [BitConverter]::ToUInt64($out, 0)
            $rm2 = [BitConverter]::ToUInt64($out, 8)
            $obs = [BitConverter]::ToUInt64($out, 16)
            $tot = [BitConverter]::ToUInt64($out, 24)
            if ($op -eq 0) { $ap = [BitConverter]::ToInt64($out, 32) }
            # 9.464: the stub's own instruction count - the only direct proof that
            # the guest stub is executing at all.
            $vmm = [BitConverter]::ToUInt64($out, 40)
            $grf = [BitConverter]::ToUInt64($out, 48)
            $gvi = [BitConverter]::ToUInt64($out, 56)
            $gsd = [BitConverter]::ToUInt64($out, 64)
            $gev = [BitConverter]::ToUInt64($out, 72)
            $gix = [BitConverter]::ToUInt64($out, 80)
            Write-Host ("exit-mask: extra Misc1=0x{0:X8}  Misc2=0x{1:X8}" -f $rm1, $rm2)
            if ($op -ne 0) {
                if ($ap -lt 0) {
                    Write-Host '  applied: (read-back only, nothing changed)'
                } elseif ($ap -eq 0) {
                    Write-Host '  applied: 0 live VMCBs - no core is virtualized, so this'
                    Write-Host '           mask is only staged for the next prepare.'
                } else {
                    Write-Host ("  applied: {0} live VMCB(s) NOW - no reload needed" -f $ap)
                }
            }
            Write-Host ("  distinct exit codes observed={0}  total exits={1}" -f $obs, $tot)
            Write-Host ("  vmmcall (stub ticks)={0}" -f $vmm)
            Write-Host ("  guest core#{0}: Rflags=0x{1:X}  IF={2}  VIntr=0x{3:X}  shadow={4}  EventInj=0x{5:X}" -f $gix, $grf, $(if($grf -band 0x200){1}else{0}), $gvi, $gsd, $gev)
            if ($rm1 -eq 0 -and $rm2 -eq 0) {
                Write-Host '  no extra bits armed - 25 of the 35 handled codes cannot'
                Write-Host '  fire this run. Use: exit-mask quiet (HLT+PAUSE) or full'
            }
        }
        'sys-auth' {
            # 9.371 (Tier 9.2): authenticated control commands.
            #
            # usage:
            #   sys-auth                        stats
            #   sys-auth key <hexbytes>         install a shared key
            #   sys-auth mode off|monitor|enforce
            #   sys-auth reset
            #   sys-auth sign <seq> <fn> [hexargs]
            #
            # WHAT EACH PART IS FOR
            #   key      authority that is NOT a handle. The device already checks
            #            owner process/CR3/SeDebugPrivilege; a key survives a handle
            #            being duplicated or inherited, an ACL does not.
            #   seq      monotonic; the driver refuses anything not STRICTLY greater
            #            than the last accepted. Without it a captured command
            #            replays forever and the MAC cannot tell the two apart.
            #   mode     off (default) / monitor (verify and count, but allow) /
            #            enforce (refuse). monitor exists so the key can be tested
            #            on a live machine before a wrong key locks it out.
            #   sign     the DRIVER produces the tag, so this client never
            #            re-derives the signed layout. A client that signs a
            #            different layout than the driver verifies looks exactly
            #            like a wrong key - the worst possible error message.
            if ($null -eq $Arg1) {
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x829)) -OutputLength 80
                $mode = [BitConverter]::ToUInt64($out, 0)
                $klen = [BitConverter]::ToUInt64($out, 8)
                $acc  = [BitConverter]::ToUInt64($out, 16)
                $rej  = [BitConverter]::ToUInt64($out, 24)
                $wld  = [BitConverter]::ToUInt64($out, 32)
                $bad  = [BitConverter]::ToUInt64($out, 40)
                $nt   = [BitConverter]::ToUInt64($out, 48)
                $seq  = [BitConverter]::ToUInt64($out, 56)
                $st   = [BitConverter]::ToUInt64($out, 64)
                $mn = 'off'
                if ($mode -eq 1) { $mn = 'monitor' }
                elseif ($mode -eq 2) { $mn = 'enforce' }
                Write-Host ("sys-auth: mode={0}  key={1} byte(s)" -f $mn, $klen)
                Write-Host ("  accepted={0} rejected={1} would-reject={2}" -f $acc, $rej, $wld)
                Write-Host ("  bad-seq={0} no-tag={1} last-seq={2}" -f $bad, $nt, $seq)
                Write-Host ("  HMAC self-test: " + $(if($st -eq 1){'PASS (RFC 4231 case 2)'}else{'FAIL - tags are NOT trustworthy'}))
                if ($klen -eq 0 -and $mode -ne 0) {
                    Write-Host '  WARNING: a mode is set but no key is installed. Enforce'
                    Write-Host '           with no key would refuse every command including'
                    Write-Host '           the one that turns it off, so the driver refuses'
                    Write-Host '           that combination; seeing it means something reset.'
                }
                return
            }
            $v = "$Arg1".ToLower()
            if ($v -eq 'key') {
                # 9.371b: PASSING AN EMPTY STRING DOES NOT ARRIVE AS AN EMPTY STRING.
                # 'sys-auth key ""' reaches here as $Arg2 == $null, so the clearing
                # case threw "a hex byte string is required" and a test that
                # cleared the key then tried enforce saw key=16B still installed.
                # Measured on G41 step 7: exactly that, and it read as "enforce
                # accepted with no key" - a security finding invented by a caller
                # convention.
                #
                # So clearing is an explicit token. A command that removes a key is
                # worth being explicit about anyway: it is the one that drops the
                # channel back to ACL-only.
                if ("$Arg2".ToLower() -eq 'clear') {
                    $req = New-Object byte[] 32
                    [BitConverter]::GetBytes([uint64]1).CopyTo($req, 0)
                    [BitConverter]::GetBytes([uint64]0).CopyTo($req, 8)
                    $o = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x829)) -InBytes $req -OutputLength 8
                    Write-Host ("sys-auth key: cleared -> {0}" -f $(if([BitConverter]::ToUInt64($o,0)){'OK'}else{'REJECTED'}))
                    return
                }
                if ($null -eq $Arg2) { throw 'sys-auth key: a hex byte string, or the word clear' }
                $hex = ("$Arg2" -replace '^0[xX]','') -replace '[^0-9A-Fa-f]',''
                if ($hex.Length % 2 -ne 0) { throw 'sys-auth key: odd number of hex digits' }
                $n = $hex.Length / 2
                if ($n -gt 64) { throw 'sys-auth key: at most 64 bytes' }
                $bytes = New-Object byte[] $n
                for ($i = 0; $i -lt $n; $i++) { $bytes[$i] = [Convert]::ToByte($hex.Substring($i*2,2),16) }
                $req = New-Object byte[] (32 + $n)
                [BitConverter]::GetBytes([uint64]1).CopyTo($req, 0)
                [BitConverter]::GetBytes([uint64]$n).CopyTo($req, 8)
                $bytes.CopyTo($req, 32)
                $o = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x829)) -InBytes $req -OutputLength 8
                Write-Host ("sys-auth key: {0} byte(s) -> {1}" -f $n, $(if([BitConverter]::ToUInt64($o,0)){'installed'}else{'REJECTED'}))
                return
            }
            if ($v -eq 'mode') {
                if ($null -eq $Arg2) { throw 'sys-auth mode: off, monitor or enforce is required' }
                $m = 0
                if ("$Arg2".ToLower() -eq 'monitor') { $m = 1 }
                elseif ("$Arg2".ToLower() -eq 'enforce') { $m = 2 }
                elseif ("$Arg2".ToLower() -ne 'off') { throw 'sys-auth mode: off, monitor or enforce only' }
                $req = New-Object byte[] 32
                [BitConverter]::GetBytes([uint64]2).CopyTo($req, 0)
                [BitConverter]::GetBytes([uint64]$m).CopyTo($req, 8)
                $o = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x829)) -InBytes $req -OutputLength 8
                Write-Host ("sys-auth mode: {0} -> {1}" -f "$Arg2", $(if([BitConverter]::ToUInt64($o,0)){'OK'}else{'REFUSED'}))
                if ([BitConverter]::ToUInt64($o,0) -eq 0 -and $m -eq 2) {
                    Write-Host '  enforce with no key installed would lock out every command,'
                    Write-Host '  including the one that turns it back off. Install a key first.'
                }
                return
            }
            if ($v -eq 'reset') {
                $req = New-Object byte[] 32
                [BitConverter]::GetBytes([uint64]3).CopyTo($req, 0)
                Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x829)) -InBytes $req -OutputLength 8 | Out-Null
                Write-Host 'sys-auth: stats reset'
                return
            }
            if ($v -eq 'sign') {
                if ($null -eq $Arg2 -or $null -eq $Arg3) { throw 'sys-auth sign: a seq and a function number are required' }
                $seq  = [uint64]::Parse($Arg2)
                $fn   = [uint64]::Parse($Arg3)
                $argsLen = 0
                $argsBytes = New-Object byte[] 0
                if ($null -ne $Arg4) {
                    $h = ("$Arg4" -replace '^0[xX]','') -replace '[^0-9A-Fa-f]',''
                    if ($h.Length % 2 -ne 0) { throw 'sys-auth sign: odd number of hex digits in args' }
                    $argsLen = $h.Length / 2
                    $argsBytes = New-Object byte[] $argsLen
                    for ($i = 0; $i -lt $argsLen; $i++) { $argsBytes[$i] = [Convert]::ToByte($h.Substring($i*2,2),16) }
                }
                $req = New-Object byte[] (32 + $argsLen)
                [BitConverter]::GetBytes([uint64]4).CopyTo($req, 0)
                [BitConverter]::GetBytes([uint64]$seq).CopyTo($req, 8)
                [BitConverter]::GetBytes([uint64]$argsLen).CopyTo($req, 16)
                [BitConverter]::GetBytes([uint64]$fn).CopyTo($req, 24)
                if ($argsLen -gt 0) { $argsBytes.CopyTo($req, 32) }
                $o = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x829)) -InBytes $req -OutputLength 40
                if ([BitConverter]::ToUInt64($o,0) -eq 0) { Write-Host 'sys-auth sign: FAILED'; return }
                $tag = -join (8..39 | ForEach-Object { '{0:x2}' -f $o[$_] })
                Write-Host ("sys-auth sign: seq={0} fn=0x{1:X} args={2}B -> {3}" -f $seq, $fn, $argsLen, $tag)
                return
            }
            throw 'sys-auth: use key, mode, reset or sign'
        }
        'guest-cpuid' {
            # 9.368 (A4, Tier 5.4): read CPUID the way a DETECTOR reads it.
            #
            # Driver-side __cpuidex answers from the host side of the boundary, so
            # it can never show what the policy did. This stub executes CPUID in a
            # plain user-mode process, which is intercepted for the whole guest.
            #
            # usage:
            #   guest-cpuid [leaf]           default leaf 1
            #   guest-cpuid 0x40000000       vendor + max leaf
            #
            # Reported: EAX EBX ECX EDX, plus the two bits that matter most:
            #   leaf 1 ECX bit31 = "hypervisor present"
            #   leaf 0x40000000  = vendor string, all zero when told there is none
            $leaf = 1
            if ($null -ne $Arg1) { $leaf = [int]([Convert]::ToUInt32(("$Arg1" -replace '^0[xX]',''), 16)) }
            $r = [YghvCpuid]::Query($leaf, 0)
            # The stub returns CPUID's registers as SIGNED int32, so ECX with
            # bit 31 set comes back negative. `[uint32]$negativeInt` throws
            # InvalidCast in PowerShell 5.1 -- the conversion has to go through
            # [uint32]::Parse or a mask, not a cast. Reinterpret the bits:
            #   ($r[n] -band 0xFFFFFFFFL) is wrong for a negative int32 (it stays
            #   negative), so use the .NET BitConverter round trip instead.
            function As-U32([int]$v) {
                return [BitConverter]::ToUInt32([BitConverter]::GetBytes($v), 0)
            }
            $eax = As-U32 $r[0]; $ebx = As-U32 $r[1]
            $ecx = As-U32 $r[2]; $edx = As-U32 $r[3]
            Write-Host ("guest-cpuid: leaf=0x{0:X8}  eax=0x{1:X8} ebx=0x{2:X8} ecx=0x{3:X8} edx=0x{4:X8}" -f `
                $leaf, $eax, $ebx, $ecx, $edx)
            if ($leaf -eq 1) {
                $hv = ($ecx -shr 31) -band 1
                Write-Host ("  hypervisor-present bit (ECX.31) = {0}" -f $hv)
            }
            if ($leaf -eq 0x40000000) {
                # Vendor string = EBX||ECX||EDX, 12 ASCII bytes (leaf 0x40000000).
                # Built with an explicit loop: the pipeline form
                #   -join ($vb | ForEach-Object { if ($_ -ne 0) { [char]$_ } })
                # tripped the 5.1 parser inside this switch arm, and there is no
                # reason to depend on a pipeline for twelve characters.
                $sb = New-Object System.Text.StringBuilder
                for ($bi = 0; $bi -lt 12; $bi++) {
                    $src = As-U32 $r[1 + [Math]::Floor($bi / 4)]
                    $byte = ($src -shr (8 * ($bi % 4))) -band 0xFF
                    if ($byte -ne 0) { [void]$sb.Append([char]$byte) }
                }
                $vs = $sb.ToString()
                Write-Host ("  max-leaf={0}  vendor='{1}'" -f $eax, $vs)
                if ($vs.Length -eq 0) { Write-Host '  (all zero: the guest is told no hypervisor exists here)' }
            }
        }
        'guest-rdtsc' {
            # 9.368 (B1/B2, Tier 5.1): read the clock the GUEST sees.
            #
            # usage:
            #   guest-rdtsc                 one reading
            #   guest-rdtsc interval <n>    n spaced readings + the deltas
            #
            # The interval form is the one that matters: a constant TscOffset
            # cancels out of a delta, while a freeze makes every delta zero. Those
            # are different claims and a single reading cannot tell them apart.
            $n = 1
            if ($Arg1 -eq 'interval') {
                $n = 5
                if ($null -ne $Arg2) { $n = [int]$Arg2 }
            }
            $vals = @()
            for ($i = 0; $i -lt $n; $i++) {
                $vals += [YghvTsc]::Read()
                if ($n -gt 1) { Start-Sleep -Milliseconds 20 }
            }
            if ($n -eq 1) {
                Write-Host ("guest-rdtsc: {0} (0x{1:X})" -f $vals[0], $vals[0])
            } else {
                Write-Host ("guest-rdtsc interval: {0} readings" -f $n)
                for ($i = 0; $i -lt $vals.Count; $i++) { Write-Host ("  [{0}] {1}" -f $i, $vals[$i]) }
                $zeros = 0
                for ($i = 1; $i -lt $vals.Count; $i++) {
                    $d = [Int64]($vals[$i] - $vals[$i-1])
                    if ($d -eq 0) { $zeros++ }
                    Write-Host ("  delta[{0}] = {1}" -f $i, $d)
                }
                Write-Host ("  zero deltas: {0} of {1}" -f $zeros, ($vals.Count - 1))
            }
        }
        'sys-guard' {
            # Tier 7.1/7.3 (2026-10-18): self-hiding + integrity self-check.
            #
            # THE EXPOSURE this answers: our tables are ordinary physical pages
            # below 4GB (MmAllocateContiguousMemory). The NPT identity-maps
            # physical memory for the guest, so a guest walking its own address
            # space can read AND WRITE them - an NPT entry, a breakpoint slot, a
            # frame reservation. Nothing detected it and nothing stopped it.
            #
            # Two answers, and they are complementary:
            #
            #   hide  (7.1)  remove the page's leaf entry from the guest's
            #                identity map. The guest's access raises #NPF and we
            #                deliver an access violation, so the range simply is
            #                not there. Our own code never uses the guest
            #                mapping (tables use the recorded direct VA, 9.280),
            #                so this costs us nothing.
            #                HONEST LIMIT: the address space is now
            #                discontinuous. A guest that compares its memory
            #                descriptors against what it can touch sees holes.
            #
            #   hash  (7.3)  FNV-1a over the page at arm time, re-checked on
            #                demand. Hiding is not perfect, so assume someone
            #                gets through: a changed hash names the page.
            #
            # usage:
            #   sys-guard                     diagnostics
            #   sys-guard arm                 hash the Tier-4 tables (no hide)
            #   sys-guard add <hex_pa> [hide] [hash] [tag]
            #   sys-guard del <hex_pa>
            #   sys-guard verify              re-hash everything; count changes
            #   sys-guard hidecore            hide them too (RIP-whitelist ALLOW loop)
            #   sys-guard uncovered <hex> <len>  pages with no guard entry
            #   sys-guard list
            if ($null -eq $Arg1) {
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x826)) -OutputLength 64
                Write-Host ("sys-guard: guarded={0} hidden={1} verified={2} changed={3}" -f `
                    [BitConverter]::ToUInt64($out,0), [BitConverter]::ToUInt64($out,8),
                    [BitConverter]::ToUInt64($out,16), [BitConverter]::ToUInt64($out,24))
                Write-Host ("  probes={0}  hide_fail={1}  hash_fail={2}" -f `
                    [BitConverter]::ToUInt64($out,32), [BitConverter]::ToUInt64($out,40),
                    [BitConverter]::ToUInt64($out,48))
                if ([BitConverter]::ToUInt64($out,24) -gt 0) {
                    Write-Host '  WARNING: a guarded page changed since it was armed'
                }
                return
            }
            $v = "$Arg1".ToLower()
            $op = -1; $a=[uint64]0; $b=[uint64]1; $c=[uint64]1
            $tag = 'user'
            switch ($v) {
                'add'    { $op = 0 }
                'del'    { $op = 1; $b=[uint64]0; $c=[uint64]0 }
                'verify' { $op = 2 }
                'list'   { $op = 3 }
                'arm'    { $op = 4 }
                'uncovered' { $op = 5 }
                'hidecore'  { $op = 6 }
                'why'       { $op = 7 }
                default  { throw 'sys-guard: use arm|hidecore|add|del|verify|list|uncovered|why' }
            }
            if ($op -eq 0) {
                if ($null -eq $Arg2) { throw 'sys-guard add: usage: sys-guard add <hex_pa> [hide] [hash] [tag]' }
                $a = [Convert]::ToUInt64(("$Arg2" -replace '^0[xX]',''),16)
                if ($null -ne $Arg3) { $b = [uint64]::Parse($Arg3) }
                if ($null -ne $Arg4) { $c = [uint64]::Parse($Arg4) }
                if ($null -ne $Arg5) { $tag = "$Arg5" }
            } elseif ($op -eq 1) {
                if ($null -eq $Arg2) { throw 'sys-guard del: usage: sys-guard del <hex_pa>' }
                $a = [Convert]::ToUInt64(("$Arg2" -replace '^0[xX]',''),16)
            } elseif ($op -eq 5) {
                if ($null -eq $Arg2 -or $null -eq $Arg3) { throw 'sys-guard uncovered: usage: sys-guard uncovered <hex_start> <len>' }
                $a = [Convert]::ToUInt64(("$Arg2" -replace '^0[xX]',''),16)
                $b = [uint64]::Parse($Arg3)
            } elseif ($op -eq 7) {
                # 9.366: ask ONE page why a hide would be refused - without
                # performing one. See control_device.c case 7 for the answer's
                # layout and yghv_guard.c for why a bitmask rather than a code.
                if ($null -eq $Arg2) { throw 'sys-guard why: usage: sys-guard why <hex_pa>' }
                $a = [Convert]::ToUInt64(("$Arg2" -replace '^0[xX]',''),16)
                $req7 = New-Object byte[] 32
                [BitConverter]::GetBytes([uint64]7).CopyTo($req7, 0)
                [BitConverter]::GetBytes([uint64]$a).CopyTo($req7, 8)
                # 9.484: 64 BYTES, NOT 40 - the driver refuses anything smaller.
                #
                # control_device.c `case IOCTL_YGHV_GUARD` gates on
                # `out_len >= 8 * sizeof(UINT64)`, i.e. 64 bytes. This call asked
                # for 40 and read offsets 0..32, so the offsets FIT (40 >= 32+8)
                # while the driver's own gate did not - meaning it returned
                # STATUS_BUFFER_TOO_SMALL and none of those offsets were ever
                # delivered.
                #
                # A size that satisfies "what I read" but not "what the driver
                # sends" is the shape that keeps recurring here: the call looks
                # self-consistent and the feature looks broken.
                $o7 = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x826)) -InBytes $req7 -OutputLength 64
                $why    = [BitConverter]::ToUInt64($o7, 0)
                $isTgt  = [BitConverter]::ToUInt64($o7, 8)
                $slot   = [BitConverter]::ToInt64($o7, 16)
                $hidF   = [BitConverter]::ToInt64($o7, 24)
                $mode   = [BitConverter]::ToInt64($o7, 32)
                $reasons = @()
                if ($why -band 0x01) { $reasons += 'span(g_guard..g_guard_count)' }
                if ($why -band 0x02) { $reasons += 'code_start' }
                if ($why -band 0x04) { $reasons += 'code_size' }
                if ($why -band 0x08) { $reasons += 'decoy_pa' }
                if ($why -band 0x10) { $reasons += 'decoy_va' }
                if ($why -band 0x20) { $reasons += 'npt manager' }
                if ($why -band 0x40) { $reasons += 'protect table' }
                Write-Host ("sys-guard why: pa=0x{0:X} page=0x{1:X}" -f $a, ($a -band 0xFFFFF000))
                Write-Host ("  guard state   : {0} (0x{1:X})" -f $(if($why){($reasons -join ' + ')}else{'no'}), $why)
                Write-Host ("  target table  : {0}" -f $(if($isTgt){'yes'}else{'no'}))
                Write-Host ("  guard slot    : {0}  hidden={1}  hide_mode={2}" -f $slot, $hidF, $mode)
                return
            }
            $req = New-Object byte[] 64
            [BitConverter]::GetBytes([uint32]$op).CopyTo($req, 0)
            [BitConverter]::GetBytes([uint64]$a).CopyTo($req, 8)
            [BitConverter]::GetBytes([uint64]$b).CopyTo($req, 16)
            [BitConverter]::GetBytes([uint64]$c).CopyTo($req, 24)
            if ($op -eq 0) {
                $tb = [Text.Encoding]::ASCII.GetBytes($tag)
                $n = [Math]::Min($tb.Length, 15)
                [Array]::Copy($tb, 0, $req, 32, $n)
            }
            if ($op -eq 3) {
                # 9.460: SIZE THE BUFFER FROM THE REPORTED COUNT, NOT A GUESS.
                #
                # 256 bytes was enough for three entries (8 + 3*80 = 248) and
                # produced IndexOutOfRangeException on the fourth. Worse, the
                # first version had a DUPLICATED `for` line, so the exception
                # arrived while the header line had already printed "4 page(s)" -
                # i.e. the count and the rows disagreed and nothing said why.
                #
                # Asking for a size big enough for the header plus the maximum the
                # driver will ever return means the loop can never overrun: YGHV_GUARD_MAX
                # entries at 80 bytes. 8 + 32*80 = 2568.
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x826)) -InBytes $req -OutputLength 2600
                $n = [BitConverter]::ToUInt64($out, 0)
                $maxRows = [math]::Floor(($out.Length - 8) / 80)
                if ($n -gt $maxRows) {
                    Write-Host ("sys-guard list: driver reports {0} page(s) but only {1} fit in the reply" -f $n, $maxRows)
                    $n = $maxRows
                }
                Write-Host ("sys-guard list: {0} page(s)" -f $n)
                for ($i = 0; $i -lt $n; $i++) {
                    # 9.363: stride is 10 qwords = 80 bytes, per the C side
                    # (control_device.c case 3):
                    #   e[0]=gpa  e[1]=checks  e[2]=changes  e[3]=hidden
                    #   e[4]=hash_off|hash_len  e[5..8]=digest  e[9]=tag|armed
                    #
                    # The parser had $o = 8 + $i*48, i.e. 48-byte entries, so it read
                    # checks from +32 and changes from +16 - both wrong, and the tag
                    # from +40..47, which is inside the digest. It looked plausible
                    # only because a stale value happened to sit where it read.
                    # Measured: "pa=0x1F1952000 ... tag=lñµ}Yç" - one real entry
                    # followed by address-shaped garbage, printed with no error.
                    $o = 8 + $i*80
                    $tg = -join (0..7 | ForEach-Object {
                        $ch = $out[$o + 72 + $_]
                        if ($ch -ne 0) { [char]$ch } else { "" }
                    })
                    Write-Host ("  [{0}] pa=0x{1:X} checks={2} changes={3} hidden={4} tag={5}" -f `
                        $i, [BitConverter]::ToUInt64($out,$o),
                        [BitConverter]::ToUInt64($out,$o+8),
                        [BitConverter]::ToUInt64($out,$o+16),
                        [BitConverter]::ToUInt64($out,$o+24), $tg)
                }
                return
            }
            $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x826)) -InBytes $req -OutputLength 8
            $rc = [BitConverter]::ToInt64($out, 0)
            switch ($op) {
                0 { if ($rc -ge 0) { Write-Host ("sys-guard: add OK slot={0}" -f $rc) }
                    else { Write-Host ("sys-guard: add FAILED rc={0}" -f $rc) } }
                1 { Write-Host ("sys-guard: del {0}" -f $(if($rc -gt 0){'OK'}else{'FAILED'})) }
                2 { Write-Host ("sys-guard: verify -> {0} page(s) changed" -f $rc) }
                4 { Write-Host ("sys-guard: armed {0} page(s)" -f $rc) }
                6 { Write-Host ("sys-guard: hid {0} page(s)" -f $rc) }
            }
            if ($op -eq 5) {
                # 9.343 #3: pages in [start, start+len) with no guard entry.
                # Reported separately because it uses the 256-byte output form.
                $out5 = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x826)) -InBytes $req -OutputLength 256
                $n5 = [BitConverter]::ToUInt64($out5, 0)
                Write-Host ("sys-guard uncovered: {0} page(s) in [0x{1:X}, +{2}) have no guard entry" -f $n5, $a, $b)
                $show = [Math]::Min($n5, 6)
                for ($i = 0; $i -lt $show; $i++) {
                    Write-Host ("  0x{0:X}" -f [BitConverter]::ToUInt64($out5, 8 + $i*8))
                }
                if ($n5 -gt $show) { Write-Host ("  ... and {0} more" -f ($n5 - $show)) }
                return
            }
        }
        'sys-health' {
            # Tier 7.2/7.7 (2026-10-18): failure isolation / graceful degradation.
            #
            # Before this existed, every abnormal condition ended in
            # KeBugCheckEx - a feature hitting an unexpected state and the
            # machine being broken looked identical, and the price was a reboot
            # each time (12 of them in one day).
            #
            # Now an unexpected condition is DEGRADED first: unknown VMEXIT is
            # recorded and skipped, an unexpected CR write is dropped, a stalled
            # watchdog is reported. A bugcheck happens only when the framework
            # says nothing safe is left.
            #
            # total    = abnormal conditions seen
            # fatal    = of those, how many could not be survived
            # degraded = 1 once anything was degraded
            if ($null -ne $Arg1 -and "$Arg1" -eq 'reset') {
                $req = New-Object byte[] 32
                Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x825)) -OutputLength 64 | Out-Null
                Write-Host 'sys-health: counters are per-boot; nothing to reset yet'
                return
            }
            $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x825)) -OutputLength 64
            $tot  = [BitConverter]::ToUInt64($out, 0)
            $deg  = [BitConverter]::ToUInt64($out, 8)
            $fat  = [BitConverter]::ToUInt64($out, 16)
            $lr   = [BitConverter]::ToUInt64($out, 24)
            $la   = [BitConverter]::ToUInt64($out, 32)
            $lb   = [BitConverter]::ToUInt64($out, 40)
            $lc   = [BitConverter]::ToUInt64($out, 48)
            $names = @('unknown-vmexit','unexpected-cr','watchdog-stall','npf-unresolved','table-full')
            $nm = if ($lr -lt 5) { $names[[int]$lr] } else { "reason=$lr" }
            Write-Host ("sys-health: degraded={0} total={1} fatal={2}" -f $deg, $tot, $fat)
            if ($tot -gt 0) {
                Write-Host ("  last: {0}  a=0x{1:X} b=0x{2:X} c=0x{3:X}" -f $nm, $la, $lb, $lc)
                Write-Host ("  survived={0}  died={1}" -f ($tot - $fat), $fat)
            } else {
                Write-Host '  no abnormal conditions recorded'
            }
        }
        'mem-frame' {
            # Tier 4.5 (2026-10-17): frame ownership takeover.
            #
            # Reserve copies a frame's content into a private nonpaged buffer
            # that ONLY we hold. The content then survives:
            #   - the owner process exiting (the frame returns to the PFN free list)
            #   - the frame being handed to a different process
            #   - the original page being reused for something else
            #
            # That is the property the host kernel cannot give you: once a
            # frame is freed, Windows may recycle it at any moment, so reading
            # it "later" is a race you lose. Our own copy makes "later" unbounded.
            #
            # NOT claimed: intercepting the free path, or serving forged
            # content to whoever gets the frame next. For the second half use
            # mem-view (Tier 4.1): register the new owner as an observer.
            #
            # usage:
            #   mem-frame                            diagnostics
            #   mem-frame reserve <hex_gpa> [len]    copy the frame, keep it
            #   mem-frame read <slot> [off] [len]    read the retained bytes
            #   mem-frame list                       what is retained
            #   mem-frame release <slot>
            if ($null -eq $Arg1) {
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x824)) -OutputLength 64
                Write-Host ("mem-frame: active={0} reserved={1} released={2} bytes={3} fail={4}" -f `
                    [BitConverter]::ToUInt64($out,16), [BitConverter]::ToUInt64($out,24),
                    [BitConverter]::ToUInt64($out,32), [BitConverter]::ToUInt64($out,40),
                    [BitConverter]::ToUInt64($out,48))
                return
            }
            $v = "$Arg1".ToLower()
            $op = -1; $a=[uint64]0; $b=[uint64]0
            switch ($v) {
                'reserve' { $op = 0 }
                'release' { $op = 1 }
                'read'    { $op = 2 }
                'list'    { $op = 3 }
                default   { throw 'mem-frame: use reserve|read|list|release' }
            }
            if ($op -eq 0) {
                if ($null -eq $Arg2) { throw 'mem-frame reserve: usage: mem-frame reserve <hex_gpa> [len]' }
                $a = [Convert]::ToUInt64(("$Arg2" -replace '^0[xX]',''),16)
                $b = 4096
                if ($null -ne $Arg3) { $b = [uint64]::Parse($Arg3) }
            } elseif ($op -eq 1) {
                if ($null -eq $Arg2) { throw 'mem-frame release: usage: mem-frame release <slot>' }
                $a = [uint64]::Parse($Arg2)
            } elseif ($op -eq 2) {
                if ($null -eq $Arg2) { throw 'mem-frame read: usage: mem-frame read <slot> [off] [len]' }
                $a = [uint64]::Parse($Arg2)
                if ($null -ne $Arg3) { $b = [uint64]::Parse($Arg3) }
            }
            $req = New-Object byte[] 32
            [BitConverter]::GetBytes([uint32]$op).CopyTo($req, 0)
            [BitConverter]::GetBytes([uint64]$a).CopyTo($req, 8)
            [BitConverter]::GetBytes([uint64]$b).CopyTo($req, 16)

            if ($op -eq 0 -or $op -eq 1) {
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x824)) -InBytes $req -OutputLength 8
                $rc = [BitConverter]::ToInt64($out, 0)
                if ($op -eq 0) {
                    if ($rc -ge 0) { Write-Host ("mem-frame: reserve OK slot={0}" -f $rc) }
                    else { Write-Host 'mem-frame: reserve FAILED' }
                } else {
                    Write-Host ("mem-frame: release {0}" -f $(if($rc -gt 0){'OK'}else{'FAILED'}))
                }
                return
            }
            if ($op -eq 3) {
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x824)) -InBytes $req -OutputLength 264
                $n = [BitConverter]::ToUInt64($out, 0)
                Write-Host ("mem-frame list: {0} reservation(s)" -f $n)
                for ($i = 0; $i -lt $n; $i++) {
                    $o = 8 + $i*32
                    Write-Host ("  [{0}] gpa=0x{1:X} seq={2} len={3} alive={4}" -f `
                        $i, [BitConverter]::ToUInt64($out,$o), [BitConverter]::ToUInt64($out,$o+8),
                        [BitConverter]::ToUInt64($out,$o+16), [BitConverter]::ToUInt64($out,$o+24))
                }
                return
            }
            # read
            $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x824)) -InBytes $req -OutputLength 4136
            $got = [BitConverter]::ToUInt64($out, 0)
            Write-Host ("mem-frame read: {0} byte(s)" -f $got)
            if ($got -gt 0) {
                $n = [Math]::Min(64, [int]$got)
                $hex = ($out[8..(8+$n-1)] | ForEach-Object { $_.ToString('X2') }) -join ' '
                Write-Host ("  {0}" -f $hex)
            }
        }
        'mem-bp' {
            # Tier 4.4 (2026-10-17): instruction-level access control, three mechanisms.
            #
            #   npf   page's NPT permission revoked. No code byte changes, no DR
            #         register touched -> invisible to the target. PAGE granularity,
            #         one VMEXIT per access.
            #   int3  0xCC written at the instruction. True instruction granularity,
            #         no per-access exit cost. The code byte REALLY changes (a target
            #         that checksums its own code sees it).
            #   dr    one of the four hardware debug registers. Exact per address,
            #         free until it fires, but only four slots. The guest's DR access
            #         is intercepted, so the target sees our shadow bank, not the real
            #         breakpoint.
            #
            # usage:
            #   mem-bp                                 diagnostics (mode breakdown)
            #   mem-bp add <gpa> <rip|0> [kinds]       NPF mode (default)
            #   mem-bp int3 <gpa> <hex_va> [kinds]     INT3 mode (needs the VA's PA)
            #   mem-bp dr <hex_va> [kinds]             DR mode
            #   mem-bp del <gpa> <rip|0>               remove one
            #   mem-bp clear [1|2|4]                   remove all / one mode
            #   mem-bp drain [max]                     drain the hit ring
            # kinds: r (read), w (write), x (execute); default w for npf, x for int3/dr
            if ($null -eq $Arg1) {
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x823)) -OutputLength 128
                Write-Host ("mem-bp: count={0}  hits={1} recorded={2} refusals={3} missed={4}" -f `
                    [BitConverter]::ToUInt64($out,16), [BitConverter]::ToUInt64($out,24),
                    [BitConverter]::ToUInt64($out,32), [BitConverter]::ToUInt64($out,40),
                    [BitConverter]::ToUInt64($out,72))
                Write-Host ("  by mode: npf={0}  int3={1}  dr={2}" -f `
                    [BitConverter]::ToUInt64($out,48), [BitConverter]::ToUInt64($out,56),
                    [BitConverter]::ToUInt64($out,64))
                Write-Host ("  last add raw: gpa=0x{0:X} va=0x{1:X} rip=0x{2:X} kinds={3} mode={4}" -f `
                    [BitConverter]::ToUInt64($out,80), [BitConverter]::ToUInt64($out,88),
                    [BitConverter]::ToUInt64($out,96),
                    ([BitConverter]::ToUInt64($out,104) -band 0xFFFFFFFF),
                    ([BitConverter]::ToUInt64($out,104) -shr 32))
                # 9.373: leaf flips to/from the shadow frame. For INT3 mode this
                # replaces "the byte changed" as the proof the chain ran: with a
                # shadow the guest page keeps 0xCC and the original byte only
                # exists in the shadow copy.
                if ($out.Length -ge 128) {
                    Write-Host ("  shadow leaf flips={0}" -f [BitConverter]::ToUInt64($out,112))
                }
                return
            }
            $v = "$Arg1".ToLower()
            $op = -1; $a=[uint64]0; $b=[uint64]0; $k=[uint64]0; $va=[uint64]0; $mode=[uint64]0
            $defk = 'w'
            switch ($v) {
                'add'   { $op = 0; $mode = 1 }
                'int3'  { $op = 0; $mode = 2; $defk = 'x' }
                'dr'    { $op = 0; $mode = 4; $defk = 'x' }
                'del'   { $op = 1 }
                'drain' { $op = 2 }
                'clear' { $op = 3 }
                default { throw 'mem-bp: use add|int3|dr|del|drain|clear' }
            }

            if ($op -eq 0) {
                if ($mode -eq 4) {
                    if ($null -eq $Arg2) { throw 'mem-bp dr: usage: mem-bp dr <hex_va> [kinds]' }
                    $va = [Convert]::ToUInt64(("$Arg2" -replace '^0[xX]',''),16)
                    if ($null -ne $Arg3) { $defk = "$Arg3" }
                } elseif ($mode -eq 2) {
                    if ($null -eq $Arg3) { throw 'mem-bp int3: usage: mem-bp int3 <hex_gpa> <hex_va> [kinds]' }
                    $a  = [Convert]::ToUInt64(("$Arg2" -replace '^0[xX]',''),16)
                    $va = [Convert]::ToUInt64(("$Arg3" -replace '^0[xX]',''),16)
                    if ($null -ne $Arg4) { $defk = "$Arg4" }
                } else {
                    if ($null -eq $Arg2) { throw 'mem-bp add: usage: mem-bp add <hex_gpa> <rip|0> [kinds]' }
                    $a = [Convert]::ToUInt64(("$Arg2" -replace '^0[xX]',''),16)
                    if ($null -ne $Arg3) { $b = [Convert]::ToUInt64(("$Arg3" -replace '^0[xX]',''),16) }
                    if ($null -ne $Arg4) { $defk = "$Arg4" }
                }
                $ks = "$defk".ToLower(); $k = 0
                if ($ks -match 'r') { $k = $k -bor 1 }
                if ($ks -match 'w') { $k = $k -bor 2 }
                if ($ks -match 'x') { $k = $k -bor 4 }
                if ($k -eq 0) { $k = 2 }
                if ($mode -eq 1) { $b = $b } else { $b = $va }
            } elseif ($op -eq 1) {
                if ($null -eq $Arg2) { throw 'mem-bp del: usage: mem-bp del <hex_gpa> <rip|0>' }
                $a = [Convert]::ToUInt64(("$Arg2" -replace '^0[xX]',''),16)
                if ($null -ne $Arg3) { $b = [Convert]::ToUInt64(("$Arg3" -replace '^0[xX]',''),16) }
            } elseif ($op -eq 3) {
                if ($null -ne $Arg2) { $a = [uint64]::Parse($Arg2) }
            } elseif ($op -eq 2) {
                $a = 48
                if ($null -ne $Arg2) { $a = [uint64]::Parse($Arg2) }
            }

            if ($env:YGHV_BP_DEBUG) { Write-Host ("  DBG op=$op gpa=0x{0:X} rip=0x{1:X} kinds={2} va=0x{3:X} mode={4}" -f $a,$b,$k,$va,$mode) }
            $req = New-Object byte[] 48
            [BitConverter]::GetBytes([uint32]$op).CopyTo($req, 0)
            [BitConverter]::GetBytes([uint64]$a).CopyTo($req, 8)
            [BitConverter]::GetBytes([uint64]$b).CopyTo($req, 16)
            [BitConverter]::GetBytes([uint64]$k).CopyTo($req, 24)
            [BitConverter]::GetBytes([uint64]$va).CopyTo($req, 32)
            [BitConverter]::GetBytes([uint64]$mode).CopyTo($req, 40)

            if ($op -eq 2) {
                $outLen = 8 + [int]$a * 8
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x823)) -InBytes $req -OutputLength $outLen
                $got = [BitConverter]::ToUInt64($out, 0)
                Write-Host ("mem-bp drain: {0} qword(s)" -f $got)
                $names = @{ 1='NPF'; 2='INT3'; 4='DR' }
                for ($i = 0; $i -lt $got; $i += 6) {
                    $rip = [BitConverter]::ToUInt64($out, 8 + $i*8)
                    $gpa = [BitConverter]::ToUInt64($out, 8 + ($i+1)*8)
                    $kd  = [BitConverter]::ToUInt64($out, 8 + ($i+2)*8)
                    $cr3 = [BitConverter]::ToUInt64($out, 8 + ($i+3)*8)
                    $hva = [BitConverter]::ToUInt64($out, 8 + ($i+4)*8)
                    $seq = [BitConverter]::ToUInt64($out, 8 + ($i+5)*8)
                    $kind = $kd -band 0xFF
                    $cpl  = ($kd -shr 8) -band 0xFF
                    $md   = ($kd -shr 16) -band 0xFF
                    $ks = @(); if ($kind -band 1) { $ks += 'R' }
                    if ($kind -band 2) { $ks += 'W' }; if ($kind -band 4) { $ks += 'X' }
                    Write-Host ("  #{0} {1,-4} {2} cpl={3} rip=0x{4:X} va=0x{5:X} gpa=0x{6:X} cr3=0x{7:X}" -f `
                        $seq, $names[[int]$md], ($ks -join ''), $cpl, $rip, $hva, $gpa, $cr3)
                }
                return
            }
            $out2 = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x823)) -InBytes $req -OutputLength 8
            $rc = [BitConverter]::ToInt64($out2, 0)
            $stage = switch ($rc) {
                1  { 'OK' }
                -1 { 'FAILED (stage -1: wrong IRQL, must be PASSIVE)' }
                -2 { 'FAILED (stage -2: engine is not PTE - run mem-engine pte first)' }
                -3 { 'FAILED (stage -3: could not split 2MB page to 4K)' }
                -4 { 'FAILED (stage -4: could not set NPT permission)' }
                -5 { 'FAILED (stage -5: breakpoint table full)' }
                -6 { 'FAILED (stage -6: bad mode or arguments)' }
                -7 { 'FAILED (stage -7: INT3 needs a VA)' }
            -11 { 'REFUSED (stage -11: the breakpoint registry page is hidden, so registration cannot complete; arming is refused rather than leaving an armed page with no handler)' }
                -8 { 'FAILED (stage -8: no DR slot free)' }
                -12 { 'REFUSED (stage -12: DR mode is not functional - arming writes no VMCB Dr7, and no #DB path calls on_dr; see 9.506/9.507)' }
                -9 { 'FAILED (stage -9: could not patch the instruction byte)' }
                -10 { 'FAILED (stage -10: kinds is 0)' }
                0  { 'FAILED (stage 0: bad arguments)' }
                default { "FAILED (rc=$rc)" }
            }
            if ($op -eq 0) { Write-Host ("mem-bp: {0} {1}" -f $v, $stage) }
            else {
                if ($rc -gt 0) { Write-Host ("mem-bp: {0} OK ({1})" -f $v, $rc) }
                else { Write-Host ("mem-bp: {0} FAILED" -f $v) }
            }
        }
        'mem-watch' {
            # Tier 4.6 (2026-10-17): NPF watchpoints - the debug-register superset.
            #
            # x86 gives a debugger FOUR hardware breakpoints (DR0-DR3), and
            # software breakpoints need int3 (a code byte the target can see).
            # This takes the page's NPT permission away instead: any access
            # raises #NPF, the island handler records (CR3, RIP, GPA, kind,
            # CPL) into a ring, and the ring is drained here. Unlimited
            # watchpoints, no code modified, DR registers untouched.
            #
            # Granularity is per-PAGE, not per-byte: a watch reports any
            # access to the page. (x86 NPT has no finer execute fault.)
            # usage:
            #   mem-watch                      diagnostics
            #   mem-watch add <hex_gpa> <r|w|rw>
            #   mem-watch del <hex_gpa>
            #   mem-watch drain [max]      consume: the events are gone after this
            #   mem-watch snapshot [max]   non-destructive: read the same window again
            #   mem-watch reset
            if ($null -eq $Arg1) {
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x820)) -OutputLength 88
                Write-Host ("mem-watch: pages={0} hits={1} recorded={2} dropped={3} seq={4}" -f `
                    [BitConverter]::ToUInt64($out,0), [BitConverter]::ToUInt64($out,8),
                    [BitConverter]::ToUInt64($out,16), [BitConverter]::ToUInt64($out,24),
                    [BitConverter]::ToUInt64($out,32))
                return
            }
            $v = "$Arg1".ToLower()
            $op = -1; $a=[uint64]0; $b=[uint64]0
            switch ($v) {
                'add'   { $op = 0 }
                'del'   { $op = 1 }
                'drain' { $op = 2 }
                'reset' { $op = 3 }
                'snapshot' { $op = 4 }
                default { throw 'mem-watch: use add|del|drain|snapshot|reset' }
            }
            if ($op -eq 0) {
                if ($null -eq $Arg2 -or $null -eq $Arg3) { throw 'mem-watch add: usage: mem-watch add <hex_gpa> <r|w|rw>' }
                $a = [Convert]::ToUInt64(("$Arg2" -replace '^0[xX]',''),16)
                $k = "$Arg3".ToLower()
                $b = 0
                if ($k -match 'r') { $b = $b -bor 1 }
                if ($k -match 'w') { $b = $b -bor 2 }
                if ($k -match 'x') { $b = $b -bor 4 }
                if ($b -eq 0) { $b = 3 }
            } elseif ($op -eq 1) {
                if ($null -eq $Arg2) { throw 'mem-watch del: usage: mem-watch del <hex_gpa>' }
                $a = [Convert]::ToUInt64(("$Arg2" -replace '^0[xX]',''),16)
            } elseif ($op -eq 2 -or $op -eq 4) {
                $a = 32
                if ($null -ne $Arg2) { $a = [uint64]::Parse($Arg2) }
            }

            $req = New-Object byte[] 32
            [BitConverter]::GetBytes([uint32]$op).CopyTo($req, 0)
            [BitConverter]::GetBytes([uint64]$a).CopyTo($req, 8)
            [BitConverter]::GetBytes([uint64]$b).CopyTo($req, 16)

            if ($op -eq 2 -or $op -eq 4) {
                # 9.368 (A3, Tier 5.6): op 4 uses the SAME layout as op 2 and one
                # parse path serves both. The difference is not in the bytes, it is
                # in out[1]:
                #   op 2 drain     -> out[1] is always 0 (a drain either got them or
                #                     the ring was empty)
                #   op 4 snapshot  -> out[1] = ring depth BEFORE the copy, so
                #                     `copied < depth` names a truncated buffer
                #                     instead of looking like an empty ring
                $outLen = 16 + [int]$a * 32
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x820)) -InBytes $req -OutputLength $outLen
                $got = [BitConverter]::ToUInt64($out, 0)
                $depth = [BitConverter]::ToUInt64($out, 8)
                if ($op -eq 4) {
                    Write-Host ("mem-watch snapshot: {0} event(s) of {1} retained (non-destructive)" -f $got, $depth)
                    if ($got -lt $depth) {
                        Write-Host ("  TRUNCATED: pass a larger max (currently {0}) to read all {1}" -f $a, $depth)
                    }
                } else {
                    Write-Host ("mem-watch drain: {0} event(s)" -f $got)
                }
                for ($i = 0; $i -lt $got; $i++) {
                    $o = 16 + $i*32
                    $cr3  = [BitConverter]::ToUInt64($out, $o)
                    $rip  = [BitConverter]::ToUInt64($out, $o+8)
                    $gpa  = [BitConverter]::ToUInt64($out, $o+16)
                    $kd   = [BitConverter]::ToUInt64($out, $o+24)
                    $kind = $kd -band 0xFF
                    $cpl  = ($kd -shr 8) -band 0xFF
                    $ks = @()
                    if ($kind -band 1) { $ks += 'R' }
                    if ($kind -band 2) { $ks += 'W' }
                    if ($kind -band 4) { $ks += 'X' }
                    Write-Host ("  [{0}] {1} cr3=0x{2:X} rip=0x{3:X} gpa=0x{4:X} cpl={5}" -f `
                        $i, ($ks -join ''), $cr3, $rip, $gpa, $cpl)
                }
                return
            }
            $out2 = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x820)) -InBytes $req -OutputLength 8
            Write-Host ("mem-watch: {0} {1}" -f $v, $(if([BitConverter]::ToUInt64($out2,0)){'OK'}else{'FAILED'}))
        }
        'mem-time' {
            # Tier 4.2 (2026-10-17): time-dimension control - O(1) snapshot/rollback.
            #
            # Keep several physical copies of one page and switch between them
            # by rewriting ONE NPT entry. Rollback costs a store, not a 4KB
            # copy, so a page can be reverted instantly and repeatedly.
            #
            # The host cannot express this: in the host one VA has exactly one
            # content, so "version 0 and version 1 both exist for this
            # address" is not representable. Only the translation layer can.
            # usage:
            #   mem-time                       diagnostics
            #   mem-time track <hex_gpa>
            #   mem-time untrack <hex_gpa>
            #   mem-time snapshot <hex_gpa>    capture current content as new version
            #   mem-time goto <hex_gpa> <ver>  O(1) switch to that version
            #   mem-time current <hex_gpa>
            if ($null -eq $Arg1) {
                # Zero-length input: the driver treats that as "read diagnostics".
                # (Note: the parameter is -InBytes; an unknown name is silently
                # ignored by PowerShell and the call goes out with no input.)
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x821)) -OutputLength 88
                Write-Host ("mem-time: pages={0} snapshots={1} switches={2} failures={3}" -f `
                    [BitConverter]::ToUInt64($out,16), [BitConverter]::ToUInt64($out,24),
                    [BitConverter]::ToUInt64($out,32), [BitConverter]::ToUInt64($out,40))
                return
            }
            $v = "$Arg1".ToLower()
            $op = -1; $a=[uint64]0; $b=[uint64]0
            switch ($v) {
                'track'    { $op = 0 }
                'untrack'  { $op = 1 }
                'snapshot' { $op = 2 }
                'goto'     { $op = 3 }
                'current'  { $op = 4 }
                default    { throw 'mem-time: use track|untrack|snapshot|goto|current' }
            }
            if ($null -eq $Arg2) { throw 'mem-time: needs a hex_gpa' }
            $a = [Convert]::ToUInt64(("$Arg2" -replace '^0[xX]',''),16)
            if ($op -eq 3) {
                if ($null -eq $Arg3) { throw 'mem-time goto: usage: mem-time goto <hex_gpa> <ver>' }
                $b = [uint64]::Parse($Arg3)
            }
            $req = New-Object byte[] 32
            [BitConverter]::GetBytes([uint32]$op).CopyTo($req, 0)
            [BitConverter]::GetBytes([uint64]$a).CopyTo($req, 8)
            [BitConverter]::GetBytes([uint64]$b).CopyTo($req, 16)
            $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x821)) -InBytes $req -OutputLength 88
            $ok  = [BitConverter]::ToUInt64($out, 0)
            $cur = [BitConverter]::ToInt64($out, 8)
            Write-Host ("mem-time: {0} {1}  current={2} pages={3} snapshots={4} switches={5}" -f `
                $v, $(if($ok){'OK'}else{'FAILED'}), $cur,
                [BitConverter]::ToUInt64($out,16), [BitConverter]::ToUInt64($out,24),
                [BitConverter]::ToUInt64($out,32))
        }
        'mem-view' {
            # Tier 4.1/4.3 (2026-10-11): split-view memory + cross-process aliasing.
            #
            # 4.1: the SAME guest-physical page shows different content
            #      depending on who reads it - the target sees its real frame,
            #      a registered observer sees a decoy frame.
            # 4.3: process A's VA points at process B's frame, established
            #      unilaterally (no IPC, no handle, neither process told).
            #
            # Both reduce to one primitive, and 9.311 measured that it works
            # on this CPU: rewriting an NPT entry so a GPA resolves to a
            # different SPA IS observed. (9.297/9.298's "NPT ignores software
            # flushes" was about PERMISSION flips - a different mechanism.)
            #
            # Selection is per-CORE (VMCB is per-VCPU), not per-access.
            # usage:
            #   mem-view                                   diagnostics
            #   mem-view init | stop
            #   mem-view split <hex_gpa> <hex_real> <hex_decoy>
            #   mem-view unsplit <hex_gpa>
            #   mem-view observer <hex_cr3>
            #   mem-view unobserver <hex_cr3>
            #   mem-view pick <hex_cr3>                    which NPT that CR3 gets
            if ($null -eq $Arg1) {
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x81F)) -OutputLength 64
                Write-Host ("mem-view: ready={0} main=0x{1:X} alt=0x{2:X}" -f `
                    [BitConverter]::ToUInt64($out,0), [BitConverter]::ToUInt64($out,8), [BitConverter]::ToUInt64($out,16))
                Write-Host ("  pages={0} observers={1} to_alt={2} to_main={3} failures={4}" -f `
                    [BitConverter]::ToUInt64($out,24), [BitConverter]::ToUInt64($out,32),
                    [BitConverter]::ToUInt64($out,40), [BitConverter]::ToUInt64($out,48),
                    [BitConverter]::ToUInt64($out,56))
                return
            }
            $v = "$Arg1".ToLower()
            $op = -1; $a=[uint64]0; $b=[uint64]0; $c2=[uint64]0
            switch ($v) {
                'init'        { $op = 0 }
                'stop'        { $op = 1 }
                'split'       { $op = 2 }
                'unsplit'     { $op = 3 }
                'observer'    { $op = 4 }
                'unobserver'  { $op = 5 }
                'pick'        { $op = 6 }
                default       { throw 'mem-view: use init|stop|split|unsplit|observer|unobserver|pick' }
            }
            if ($null -ne $Arg2) { $a = [Convert]::ToUInt64(("$Arg2" -replace '^0[xX]',''),16) }
            if ($null -ne $Arg3) { $b = [Convert]::ToUInt64(("$Arg3" -replace '^0[xX]',''),16) }
            if ($null -ne $Arg4) { $c2 = [Convert]::ToUInt64(("$Arg4" -replace '^0[xX]',''),16) }

            $req = New-Object byte[] 32
            [BitConverter]::GetBytes([uint32]$op).CopyTo($req, 0)
            [BitConverter]::GetBytes([uint64]$a).CopyTo($req, 8)
            [BitConverter]::GetBytes([uint64]$b).CopyTo($req, 16)
            [BitConverter]::GetBytes([uint64]$c2).CopyTo($req, 24)

            $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x81F)) -InBytes $req -OutputLength 64
            if ($op -eq 6) {
                Write-Host ("mem-view: cr3=0x{0:X} -> ncr3=0x{1:X}" -f $a, [BitConverter]::ToUInt64($out,0))
            } else {
                Write-Host ("mem-view: {0} {1}" -f $v, $(if([BitConverter]::ToUInt64($out,0)){'OK'}else{'FAILED'}))
            }
        }
        'link-hide' {
            # 9.452 (Tier 7.1): take the \\.\ name off the enumeration surface.
            #
            # usage: link-hide            (read back)
            #        link-hide on         (remove IoDeleteSymbolicLink)
            #        link-hide off        (restore IoCreateSymbolicLink)
            #
            # WHAT THIS BUYS AND WHAT IT DOES NOT
            #   buys     a `\\.\`-style enumeration no longer resolves the name -
            #            that is where user mode and most scanners look.
            #   does NOT hide the DEVICE. It stays in the driver's device list at
            #            \Device\YuanGuardHV. Calling that "invisible" would be the
            #            overclaim Tier 7.1 keeps warning about.
            #
            # WARNING: the rest of this client goes through \\.\YuanGuardHV. After
            # `on`, the client cannot reach the driver - use `off` from a session
            # that still holds the handle, or reload the driver.
            $op = 0
            if ($null -ne $Arg1) {
                $v = "$Arg1".ToLower()
                if ($v -eq 'on') { $op = 1 }
                elseif ($v -eq 'off') { $op = 2 }
                else { throw 'link-hide: use on|off' }
            }

            if ($op -ne 0) {
                $req = [BitConverter]::GetBytes([uint32]$op)
                $r = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x82E)) `
                                      -InBytes $req -OutputLength 32
            } else {
                $r = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x82E)) -OutputLength 32
            }

            $lp = [BitConverter]::ToUInt64($r, 0)
            $dp = [BitConverter]::ToUInt64($r, 8)
            $rc = [BitConverter]::ToInt64($r, 16)
            $ch = [BitConverter]::ToUInt64($r, 24)

            Write-Host ("link-hide: link_present={0}  device_present={1}  changes={2}" -f $lp, $dp, $ch)
            if ($rc -ne 0) {
                Write-Host ("  last op status = 0x{0:X8}" -f $rc)
            }
            if ($lp -eq 1) {
                Write-Host '  \\.\YuanGuardHV RESOLVES - enumerable.'
            } else {
                Write-Host '  \\.\YuanGuardHV does not resolve - name removed.'
                Write-Host '  (the DEVICE is still at \Device\YuanGuardHV - not hidden)'
            }
        }
        'share' {
            # 9.451 (Tier 2.4): the zero-copy shared window.
            #
            # The NPT is an identity map, so a frame the driver allocates is the
            # SAME frame a guest reaches by touching its physical address.
            # Nothing is copied - what this adds is the handshake and the proof.
            #
            # usage: share            (stats: PA, counters)
            #        share init       (bring the window up)
            #        share read       (consume one guest message)
            #        share write <kind> [arg]
            #        share reset
            #
            # NOTE ON TIER 7.1: this page is DELIBERATELY guest-visible. 7.1 hides
            # pages from the guest; this one exists so the guest can touch it. The
            # PA is printed so it can be audited against the 7.1 exclusion list
            # rather than quietly contradicting it.
            $op = 0; $ka = [uint64]0; $kb = [uint64]0
            if ($null -ne $Arg1) {
                $v = "$Arg1".ToLower()
                if ($v -eq 'init') { $op = 0 }
                elseif ($v -eq 'read') { $op = 1 }
                elseif ($v -eq 'reset') { $op = 3 }
                elseif ($v -eq 'write') {
                    $op = 2
                    if ($null -eq $Arg2) { throw 'share: write needs <kind> [arg]' }
                    # 9.479: ACCEPT 0x LIKE EVERY OTHER COMMAND IN THIS CLIENT.
                    #
                    # `[uint64]::Parse('0xCAFEBABE')` throws FormatException, so
                    # `share write 0x11 0xCAFEBABE` failed with a .NET parser error
                    # instead of doing the write - while `npt-view 0x...`,
                    # `sys-guard add 0x...` and `exit-mask` all accept hex. One
                    # command reading hex and its neighbour not doing so is how a
                    # client teaches people to distrust it.
                    #
                    # Done inline (not via a helper function) because a `function`
                    # declared inside this switch arm would leak into the caller's
                    # scope and stay defined for the rest of the session.
                    $ka = if ($Arg2 -match '^0[xX]') {
                              [uint64]::Parse($Arg2.Substring(2),
                                  [System.Globalization.NumberStyles]::HexNumber)
                          } else { [uint64]::Parse($Arg2) }
                    if ($null -ne $Arg3) {
                        $kb = if ($Arg3 -match '^0[xX]') {
                                  [uint64]::Parse($Arg3.Substring(2),
                                      [System.Globalization.NumberStyles]::HexNumber)
                              } else { [uint64]::Parse($Arg3) }
                    }
                }
                else { throw 'share: use init|read|write <kind> [arg]|reset' }
            }

            if ($op -ne 0 -or $null -ne $Arg1) {
                $req = New-Object byte[] 32
                [BitConverter]::GetBytes([uint32]$op).CopyTo($req, 0)
                [BitConverter]::GetBytes([uint64]$ka).CopyTo($req, 8)
                [BitConverter]::GetBytes([uint64]$kb).CopyTo($req, 16)
                $r = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x82D)) `
                                      -InBytes $req -OutputLength 72
            } else {
                $r = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x82D)) -OutputLength 72
            }

            $o0 = [BitConverter]::ToUInt64($r, 0)
            $o1 = [BitConverter]::ToUInt64($r, 8)
            $o2 = [BitConverter]::ToUInt64($r, 16)
            $o3 = [BitConverter]::ToUInt64($r, 24)
            $o4 = [BitConverter]::ToUInt64($r, 32)
            $o5 = [BitConverter]::ToUInt64($r, 40)
            $o6 = [BitConverter]::ToUInt64($r, 48)
            $o7 = [BitConverter]::ToUInt64($r, 56)
            $o8 = [BitConverter]::ToUInt64($r, 64)

            if ($op -eq 1) {
                if ($o0 -eq 1) {
                    Write-Host ("share read: GOT message kind={0} arg0={1} arg1={2} seq={3}" -f $o1, $o2, $o3, $o4)
                } else {
                    Write-Host 'share read: nothing new (seq unchanged or a write raced the read)'
                }
                Write-Host ("  window pa=0x{0:X}" -f $o6)
            } else {
                Write-Host ("share: up={0}  pa=0x{1:X}  page={2}  payload_max={3}" -f $o0, $o1, $o7, $o8)
                Write-Host ("  host writes={0} reads={1}  guest writes seen={2}  seq-mismatch={3}" -f $o2, $o3, $o5, $o4)
                Write-Host ("  last seq={0}" -f $o6)
                if ($o0 -eq 0) {
                    Write-Host '  window is DOWN - run: share init'
                }
            }
        }
        'sections' {
            # 9.455 (Tier 7.1): the driver's own PE sections, read-only.
            #
            # The last 7.1 item is "exclude the driver's data pages from the guest
            # NPT". That CANNOT be done for CODE pages - present=0 stops our own
            # fetch and there is no ALLOW loop (9.450b) - so the hide has to know
            # which sections are executable. This answers that and nothing else.
            #
            # parsed=0 means "treat every page as unhideable", NOT "nothing to
            # hide": guessing which pages are data is how a code page gets
            # excluded and the machine dies in a fetch fault.
            $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x82F)) -OutputLength 784

            $parsed = [BitConverter]::ToUInt64($out, 0)
            $pflag  = [BitConverter]::ToUInt64($out, 8)
            $nsec   = [BitConverter]::ToUInt64($out, 16)
            $isize  = [BitConverter]::ToUInt64($out, 24)
            $iunp   = [BitConverter]::ToUInt64($out, 32)

            $kind = switch ($pflag) { 1 {'PE32'} 2 {'PE32+'} default {'unknown'} }
            Write-Host ("sections: parsed={0}  fmt={1}  image=0x{2:X} byte  sections={3}" -f $parsed, $kind, $isize, $nsec)

            if ($parsed -eq 0) {
                Write-Host '  PARSE FAILED - treat every page as UNHIDEABLE (do not guess)'
                return
            }

            # 9.474: the IAT was located but NO section contains it. section-hide
            # refuses everything in that state (0x28), because hiding a page whose
            # IAT role we cannot see is exactly how 9.458 crashed
            # (rip=0 from `call [IAT slot]` reading a zeroed entry).
            if ($iunp -eq 1) {
                Write-Host '  *** IAT NOT PLACED IN ANY SECTION - section-hide will REFUSE everything'
                Write-Host '      (truncated section table or unusual linker layout; hiding is'
                Write-Host '       unsafe while we cannot tell which page holds the IAT)'
            }

            $hideable = 0
            $locked = 0
            for ($i = 0; $i -lt [int]$nsec; $i++) {
                # 7 qwords per entry (9.458 added has_iat), 8 bytes each.
                $b = 40 + $i * 56
                $lo = [BitConverter]::ToUInt64($out, $b)
                $hi = [BitConverter]::ToUInt64($out, $b + 8)
                $nm = ''
                for ($k = 0; $k -lt 8; $k++) {
                    $ch = ($lo -shr (8 * $k)) -band 0xFF
                    if ($ch -ne 0) { $nm += [char]$ch }
                }
                for ($k = 0; $k -lt 4; $k++) {
                    $ch = ($hi -shr (8 * $k)) -band 0xFF
                    if ($ch -ne 0) { $nm += [char]$ch }
                }
                $va  = [BitConverter]::ToUInt64($out, $b + 16)
                $sz  = [BitConverter]::ToUInt64($out, $b + 24)
                $ex  = [BitConverter]::ToUInt64($out, $b + 32)
                $iat = [BitConverter]::ToUInt64($out, $b + 48)

                $pages = [math]::Ceiling($sz / 4096)
                if ($ex -eq 1) {
                    $verdict = 'CODE - must NOT be hidden'
                    $locked += [int]$pages
                } elseif ($iat -eq 1) {
                    $verdict = 'HOLDS THE IAT - must NOT be hidden'
                    $locked += [int]$pages
                } else {
                    $verdict = 'data - hideable'
                    $hideable += [int]$pages
                }

                Write-Host ("  [{0}] {1,-9} va=0x{2:X6} size=0x{3:X6} ({4,3} pages)  {5}" -f $i, $nm, $va, $sz, $pages, $verdict)
            }
            Write-Host ("  totals: hideable={0} page(s)  must-stay-visible={1} page(s)" -f $hideable, $locked)
        }
        'section-hide' {
            # 9.456 (Tier 7.1): hide ONE data section of our own image.
            #
            # usage: section-hide <index>     (index comes from `sections`)
            #
            # WHY ONE AT A TIME, BY NAME
            #   Hiding all 62 data pages at once is one experiment with 62 chances
            #   to lock ourselves out, and a failure arrives as a bugcheck in
            #   whatever code touched the page first - not as "section X is unsafe".
            #
            # SUGGESTED ORDER: .reloc first (the loader consumed it, nothing reads
            # it afterwards), then .rdata/.pdata, and .data LAST (it holds live
            # state and is the one that can actually hurt).
            #
            # Refusals are named: 0x21 not parsed / 0x22 bad index /
            # 0x23 section is EXECUTABLE (refused on purpose) / 0x25 size zero /
            # 0x26 guard_add_va: APC disabled / 0x27 guard_add_va: bad VA.
            if ($null -eq $Arg1) { throw 'section-hide: usage: section-hide <index>' }
            $idx = [uint64]::Parse($Arg1)

            $req = New-Object byte[] 16
            [BitConverter]::GetBytes($idx).CopyTo($req, 0)

            $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x830)) `
                                    -InBytes $req -OutputLength 32

            $ok  = [BitConverter]::ToUInt64($out, 0)
            $hid = [BitConverter]::ToUInt64($out, 8)
            $rc  = [BitConverter]::ToUInt64($out, 16)
            $e   = [BitConverter]::ToUInt64($out, 24)

            if ($ok -eq 1) {
                Write-Host ("section-hide: index={0} -> hid {1} page(s)" -f $e, $hid)
                if ($rc -ne 0) {
                    $rx = switch ($rc) {
                        0x25 { 'section size is ZERO - nothing to hide (wrong index?)' }
                        0x26 { 'guard_add_va refused: APC disabled in this context' }
                        0x27 { 'guard_add_va refused: bad VA (MmGetPhysicalAddress returned 0)' }
                        default { ("code 0x{0:X}" -f $rc) }
                    }
                    Write-Host ("  WHY ZERO: {0}" -f $rx)
                } else {
                    Write-Host '  verify with: sys-guard list   and   npt-view <pa of the page>'
                }
            } else {
                $why = switch ($rc) {
                    0x21 { 'not parsed - image headers unreadable' }
                    0x22 { 'index out of range' }
                    0x23 { 'section is EXECUTABLE - refused (no ALLOW loop to serve a fetch fault)' }
                    0x24 { 'guard table full' }
                    0x28 { 'section HOLDS OUR IAT - refused (hiding it would zero our own import calls)' }
                    0x29 { 'IAT NOT LOCATED in any section - refusing everything (no index will work)' }
                    default { ("code 0x{0:X}" -f $rc) }
                }
                Write-Host ("section-hide: index={0} REFUSED {1}" -f $e, $why)
            }
        }
        'pa-section' {
            # 9.508 (Tier 7.1): which of OUR OWN sections holds this physical page.
            #
            # usage: pa-section <hex_pa>
            #
            # WHY IT EXISTS: the .data hide needs to know what a page IS before
            # touching it. `sections` answers in RVAs; the NPT works in PAs; only
            # the kernel can join them, because only it knows where the image was
            # loaded. Guessing which page is data is exactly how 9.458 crashed
            # (hid its own IAT, then `call [IAT slot]` read 0 and jumped to 0).
            #
            # The answer is a CANDIDATE range, not a verdict - see the header note.
            # Confirm with npt-view before hiding anything.
            if ($null -eq $Arg1) { throw 'pa-section: usage: pa-section <hex_pa>' }
            $pa = [Convert]::ToUInt64(("$Arg1" -replace '^0[xX]',''), 16)
            $pa = $pa -band (-bnot 0xFFF)

            $req = New-Object byte[] 8
            [BitConverter]::GetBytes([uint64]$pa).CopyTo($req, 0)
            # 6 header + 32 sections * 5 = 166 qwords, rounded up generously.
            $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x831)) `
                                    -InBytes $req -OutputLength 1408

            $parsed = [BitConverter]::ToUInt64($out, 0)
            $pflag  = [BitConverter]::ToUInt64($out, 8)
            $nsec   = [BitConverter]::ToUInt64($out, 16)
            $basepa = [BitConverter]::ToUInt64($out, 24)
            $endpa  = [BitConverter]::ToUInt64($out, 32)
            $found  = [BitConverter]::ToUInt64($out, 40)

            Write-Host ("pa-section: pa=0x{0:X}  parsed={1}  sections={2}" -f $pa, $parsed, $nsec)
            if ($parsed -eq 0) {
                Write-Host '  PARSE FAILED - no image span, so this page cannot be attributed.'
                return
            }
            if ($basepa -ne 0) {
                Write-Host ("  image PA span = [0x{0:X}, 0x{1:X})" -f $basepa, $endpa)
            }

            # 9.508c: NOT `-eq 0xFFFFFFFF`. PowerShell parses that literal as
            # Int32 -1, so an Int64 4294967295 never matches it and this branch
            # silently did not run - the script then used 4294967295 as a section
            # index and died on the cast. Compare against [uint64] 4294967295.
            $NONE = [uint64]4294967295
            if ($found -eq $NONE) {
                if ($pa -ge $basepa -and $pa -lt $endpa) {
                    Write-Host '  VERDICT: inside the image span but in NO parsed section'
                    Write-Host '           (a gap between sections, or the section table does not cover it)'
                } else {
                    Write-Host '  VERDICT: OUTSIDE the image - not ours'
                }
                return
            }

            $b = 48 + [int]$found * 40
            $lo   = [BitConverter]::ToUInt64($out, $b)
            $hi   = [BitConverter]::ToUInt64($out, $b + 8)
            $rva  = [BitConverter]::ToUInt64($out, $b + 16)
            $sz   = [BitConverter]::ToUInt64($out, $b + 24)
            $fl   = [BitConverter]::ToUInt64($out, $b + 32)

            $ex = ($fl -band 1) -ne 0
            $wr = ($fl -band 2) -ne 0
            $ia = ($fl -band 4) -ne 0

            # names are not in this reply; the index is, and `sections` prints names
            Write-Host ("  section [{0}]  rva=0x{1:X}  size=0x{2:X}  PA=[0x{3:X}, 0x{4:X})" -f
                $found, $rva, $sz, $lo, $hi)
            Write-Host ("  flags: executable={0}  writable={1}  has_iat={2}" -f $ex, $wr, $ia)

            if ($ex) {
                Write-Host '  VERDICT: CODE - must NOT be hidden (present=0 stops our own fetch)'
            } elseif ($ia) {
                Write-Host '  VERDICT: HOLDS THE IAT - must NOT be hidden (we call through it)'
            } else {
                Write-Host '  VERDICT: data candidate - hideable IN PRINCIPLE; confirm with npt-view first'
            }
        }
        'npt-raw' {
            # 9.510 (Tier 7.1): the RAW NPT entry for one page - and the observer
            # 9.509 said was missing.
            #
            # usage: npt-raw <hex_pa>            (read)
            #        npt-raw <hex_pa> a          (clear accessed)
            #        npt-raw <hex_pa> d          (clear dirty)
            #        npt-raw <hex_pa> ad         (clear both)
            #        npt-raw <hex_pa> p          (attempt to clear present -> REFUSED)
            #
            # WHY THIS MATTERS: a DECOY-hidden page keeps present=1, so a guest read
            # raises no #NPF and no counter moves. But the CPU SETS the entry's
            # A(accessed) bit when it walks it. So "was this hidden page read?"
            # is answerable - read A, clear it, wait, read again.
            #
            # Clearing is restricted to A and D by the driver: anything else comes
            # back as a refusal with the mask echoed, never applied.
            if ($null -eq $Arg1) { throw 'npt-raw: usage: npt-raw <hex_pa> [a|d|ad]' }
            $pa = [Convert]::ToUInt64(("$Arg1" -replace '^0[xX]',''), 16)
            $pa = $pa -band (-bnot 0xFFF)

            $clr = [uint64]0
            $what = 'read'
            if ($null -ne $Arg2) {
                $what = "$Arg2".ToLower()
                if ($what -match 'a') { $clr = $clr -bor (1L -shl 5) }
                if ($what -match 'd') { $clr = $clr -bor (1L -shl 6) }
                if ($clr -eq 0) {
                    # not a|d: pass a mask outside A|D on purpose so the driver refuses
                    # and echoes it - the refusal is what this command demonstrates.
                    $clr = [uint64]1
                }
            }

            $req = New-Object byte[] 16
            [BitConverter]::GetBytes([uint64]$pa).CopyTo($req, 0)
            [BitConverter]::GetBytes([uint64]$clr).CopyTo($req, 8)

            $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x832)) `
                                    -InBytes $req -OutputLength 32

            $ok     = [BitConverter]::ToUInt64($out, 0)
            $before = [BitConverter]::ToUInt64($out, 8)
            $after  = [BitConverter]::ToUInt64($out, 16)
            $done   = [BitConverter]::ToUInt64($out, 24)

            if ($ok -eq 0) {
                Write-Host ("npt-raw: pa=0x{0:X}  NOT REACHABLE  (requested clear=0x{1:X})" -f $pa, $clr)
                if ($clr -ne 0) {
                    Write-Host '  (a mask outside A|D is refused; 0x1 here means the driver echoed it)'
                }
                return
            }

            $acc  = ($after  -band (1L -shl 5)) -ne 0
            $dty  = ($after  -band (1L -shl 6)) -ne 0
            $prs  = ($after  -band 1L) -ne 0
            $wrt  = ($after  -band 2L) -ne 0
            $nx   = ($after  -band (([uint64]1 -shl 63))) -ne 0
            $fr   = $after -band 0x000FFFFFFFFFF000
            $accB = ($before -band (1L -shl 5)) -ne 0
            $dtyB = ($before -band (1L -shl 6)) -ne 0

            Write-Host ("npt-raw: pa=0x{0:X}  mode={1}" -f $pa, $what)
            Write-Host ("  before = 0x{0:X16}  A={1} D={2}" -f $before, $accB, $dtyB)
            Write-Host ("  after  = 0x{0:X16}  A={1} D={2}" -f $after, $acc, $dty)
            Write-Host ("  entry: present={0} writable={1} nx={2} frame=0x{3:X}" -f $prs, $wrt, $nx, $fr)
            if ($done -ne 0) { Write-Host ("  cleared mask = 0x{0:X}  (A={1} D={2})" -f $done,
                (($done -band (1L -shl 5)) -ne 0), (($done -band (1L -shl 6)) -ne 0)) }
            if ($done -eq 0 -and $clr -ne 0) { Write-Host '  clear NOT applied (write refused, or large page)' }
        }
        'npt-scan' {
            # 9.511 (Tier 7.1): read (or clear) the A/D bits over a PAGE RANGE.
            #
            # usage: npt-scan <hex_start> <pages>          (read)
            #        npt-scan <hex_start> <pages> clear    (clear A|D per page)
            #
            # WHY IT EXISTS: npt-raw is one page per call, so measuring the 54
            # .data pages cost 109 round trips (~1.5 h). This does it in one.
            #
            # WHAT THE NUMBERS MEAN: A(accessed) is set BY THE CPU when it walks
            # that NPT entry. A DECOY-hidden page stays present, so a guest read
            # still walks it and still sets A - that is the whole observer.
            # D(dirty) is set on a WRITE. Neither is set for our own accesses
            # (we use host CR3), so a set bit means a GUEST touched the page.
            #
            # THE PREREQUISITE, and it is not optional: TLB hits SUPPRESS the walk,
            # so a page already in the TLB will never re-set A. Clear A, then force
            # a flush (npt-remap any page sets TlbControl=1), then observe.
            if ($null -eq $Arg1 -or $null -eq $Arg2) {
                throw 'npt-scan: usage: npt-scan <hex_start> <pages> [clear]' }
            $start = [Convert]::ToUInt64(("$Arg1" -replace '^0[xX]',''), 16)
            $start = $start -band (-bnot 0xFFF)
            $pages = [uint64]::Parse($Arg2)
            if ($pages -lt 1 -or $pages -gt 512) { throw 'npt-scan: pages must be 1..512' }

            $op = [uint64]0
            $what = 'read'
            if ($null -ne $Arg3 -and "$Arg3".ToLower() -eq 'clear') { $op = [uint64]1; $what = 'clear' }

            $req = New-Object byte[] 24
            [BitConverter]::GetBytes([uint64]$start).CopyTo($req, 0)
            [BitConverter]::GetBytes([uint64]$pages).CopyTo($req, 8)
            [BitConverter]::GetBytes([uint64]$op).CopyTo($req, 16)

            # 2 header + 512 entries = 514 qwords = 4112 bytes; ask for 8192.
            $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x833)) `
                                    -InBytes $req -OutputLength 8192

            $visited = [BitConverter]::ToUInt64($out, 0)
            $requested = [BitConverter]::ToUInt64($out, 8)

            Write-Host ("npt-scan: start=0x{0:X}  pages={1}  mode={2}" -f $start, $pages, $what)
            if ($visited -lt $requested) {
                Write-Host ("  *** CLAMPED: visited {0} of {1} - the reply ran out of room" -f $visited, $requested)
            }

            if ($op -eq 1) {
                Write-Host ("  cleared A|D on {0} page(s); the values below are what the hardware" -f $visited)
                Write-Host '  had recorded BEFORE the clear.'
            }

            $accCount = 0
            $dtyCount = 0
            $unreach = 0
            for ($i = 0; $i -lt [int]$visited; $i++) {
                $e = [BitConverter]::ToUInt64($out, 16 + $i * 8)
                $pa = $start + ([uint64]$i * 4096)
                if ($e -eq 0) {
                    $unreach++
                    Write-Host ("  [0x{0:X}] --  NOT REACHABLE" -f $pa)
                    continue
                }
                $a = ($e -band (1L -shl 5)) -ne 0
                $d = ($e -band (1L -shl 6)) -ne 0
                $p = ($e -band 1L) -ne 0
                if ($a) { $accCount++ }
                if ($d) { $dtyCount++ }
                Write-Host ("  [0x{0:X}] A={1,-5} D={2,-5} present={3,-5} frame=0x{4:X}" -f
                    $pa, $a, $d, $p, ($e -band 0x000FFFFFFFFFF000))
            }

            Write-Host ("  summary: visited={0}/{1}  A-set={2}  D-set={3}  unreachable={4}" -f
                $visited, $requested, $accCount, $dtyCount, $unreach)
            if ($accCount -eq 0 -and $op -eq 0) {
                Write-Host '  NOTE: zero A-set. That is a claim, not a default - check the TLB'
                Write-Host '        prerequisite above before believing it.'
            }
        }
        'npt-view' {
            # 9.450 (Tier 7.1): what would a GUEST see at this physical page.
            #
            # The NPT is an identity map, so one PA is one GPA. Read-only: this
            # never writes an entry, so it is safe to call on a page mid-hide.
            #
            # usage: npt-view <hex_pa>
            #
            # HOW TO READ IT
            #   guest    present=0 means the guest CANNOT reach this frame - that is
            #            what hiding means here.
            #   host     must still be 1, or we locked OURSELVES out. The pair
            #            (guest=0, host=1) is correct hiding; (0,0) is the 9.343
            #            failure; (1,*) means not hidden at all.
            if ($null -eq $Arg1) { throw 'npt-view: usage: npt-view <hex_pa>' }
            $pa = [Convert]::ToUInt64(($Arg1 -replace '^0x',''), 16)
            $req = [BitConverter]::GetBytes([uint64]$pa)
            $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x82C)) `
                                     -InBytes $req -OutputLength 56

            $pres = [BitConverter]::ToUInt64($out, 0)
            $perm = [BitConverter]::ToUInt64($out, 8)
            $frame = [BitConverter]::ToUInt64($out, 16)
            $large = [BitConverter]::ToUInt64($out, 24)
            $hostv = [BitConverter]::ToUInt64($out, 32)
            $code  = [BitConverter]::ToUInt64($out, 40)
            $decoy = [BitConverter]::ToUInt64($out, 48)

            if ($code -ne 0) {
                Write-Host ("npt-view: pa=0x{0:X} -> state=0x{1:X} (not identity mapped)" -f $pa, $code)
                return
            }

            $permStr = @()
            if ($perm -band 1) { $permStr += 'R' }
            if ($perm -band 2) { $permStr += 'W' }
            if ($perm -band 4) { $permStr += 'X' }
            if ($permStr.Count -eq 0) { $permStr = @('none') }

            Write-Host ("npt-view: pa=0x{0:X}" -f $pa)
            Write-Host ("  guest   : present={0}  perm={1}" -f $pres, ($permStr -join ''))
            Write-Host ("  frame   : 0x{0:X}  large={1}" -f $frame, $large)
            Write-Host ("  host    : {0}" -f $hostv)

            # 9.459: a hidden page can keep present=1 (DECOY). The test is not
            # "is it present" but "does it still resolve to its own frame".
            if ($large -eq 1) {
                Write-Host '  VERDICT : LARGE PAGE - not split, so the 4K page is not hidden'
                Write-Host '            (frame shown is the 2MB base, not this page)'
            } elseif ($pres -eq 0) {
                if ($hostv -eq 1) {
                    Write-Host '  VERDICT : HIDDEN (UNMAP) - guest excluded, our access intact.'
                } else {
                    Write-Host '  VERDICT : LOCKED OUT - guest and host both excluded (9.343).'
                }
            } elseif ($decoy -eq 1) {
                Write-Host '  VERDICT : HIDDEN (DECOY) - entry stays present but points at a'
                Write-Host '            zeroed frame; the guest reads zeros, not our data.'
            } else {
                Write-Host '  VERDICT : VISIBLE to the guest - not hidden.'
            }
        }
        'npt-remap' {
            # Tier 4 prerequisite (2026-10-11): rewrite one NPT entry so a GPA
            # points at a different frame, then let the caller observe whether
            # the guest actually sees the change.
            #
            # This answers a question rather than shipping a feature. 9.297/
            # 9.298 proved NPT *permission* flips are not observed on this CPU;
            # a remap changes the same GPA->SPA translation the CPU may have
            # cached, so split-view (4.1), cross-process aliasing (4.3) and
            # frame takeover (4.5) all rest on whether this works.
            #
            # usage: npt-remap <hex_gpa> <hex_spa> [alt]
            if ($null -eq $Arg1 -or $null -eq $Arg2) {
                throw 'npt-remap: usage: npt-remap <hex_gpa> <hex_spa> [alt]'
            }
            $gpa = [Convert]::ToUInt64(("$Arg1" -replace '^0[xX]',''), 16)
            $spa = [Convert]::ToUInt64(("$Arg2" -replace '^0[xX]',''), 16)
            $mode = 0
            if ($null -ne $Arg3 -and "$Arg3" -eq 'alt') { $mode = 1 }

            $req = New-Object byte[] 24
            [BitConverter]::GetBytes([uint64]$gpa).CopyTo($req, 0)
            [BitConverter]::GetBytes([uint64]$spa).CopyTo($req, 8)
            [BitConverter]::GetBytes([uint32]$mode).CopyTo($req, 16)

            $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x81E)) `
                                    -InBytes $req -OutputLength 24
            $ok   = [BitConverter]::ToUInt64($out, 0)
            $prev = [BitConverter]::ToUInt64($out, 8)
            $new  = [BitConverter]::ToUInt64($out, 16)
            Write-Host ("npt-remap: gpa=0x{0:X} prev=0x{1:X} -> spa=0x{2:X} ok={3} mode={4}" -f
                        $gpa, $prev, $new, $ok, $(if($mode){'alt'}else{'main'}))
        }
        'mem-island' {
            # Phase 6 (2026-10-11): island (direct-map) access.
            #
            # The NPF handler runs at DISPATCH with GIF=0 and cannot call any
            # of the transfer primitives: MmCopyMemory is APC_LEVEL max,
            # MmMapLockedPagesSpecifyCache and MmMapIoSpace are PASSIVE, and
            # MmGetVirtualForPhysical only maps system pages. What is left is
            # the kernel's direct map - reading a frame becomes a plain load
            # at (base + pa), which works at any IRQL.
            #
            # The base is not exported, so it is derived from two (VA, PA)
            # pairs that must agree, then cached.
            #   mem-island                          read diagnostics
            #   mem-island init <va_a> <pa_a> <va_b> <pa_b>
            #   mem-island read <hex_pa> [len]      island read probe
            if ($null -eq $Arg1) {
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x81D)) -OutputLength 64
                $rdy = [BitConverter]::ToUInt64($out, 0)
                $base = [BitConverter]::ToUInt64($out, 8)
                Write-Host ("mem-island: ready={0} base=0x{1:X}" -f $rdy, $base)
                Write-Host ("  reads={0} writes={1} refusals={2} probes={3}" -f `
                    [BitConverter]::ToUInt64($out,16), [BitConverter]::ToUInt64($out,24),
                    [BitConverter]::ToUInt64($out,32), [BitConverter]::ToUInt64($out,40))
                return
            }
            $v = "$Arg1".ToLower()
            if ($v -eq 'init') {
                if ($null -eq $Arg5) { throw 'mem-island init: usage: mem-island init <va_a> <pa_a> <va_b> <pa_b>' }
                $req = New-Object byte[] 32
                [BitConverter]::GetBytes([uint64][Convert]::ToUInt64(("$Arg2" -replace '^0[xX]',''),16)).CopyTo($req, 0)
                [BitConverter]::GetBytes([uint64][Convert]::ToUInt64(("$Arg3" -replace '^0[xX]',''),16)).CopyTo($req, 8)
                [BitConverter]::GetBytes([uint64][Convert]::ToUInt64(("$Arg4" -replace '^0[xX]',''),16)).CopyTo($req, 16)
                [BitConverter]::GetBytes([uint64][Convert]::ToUInt64(("$Arg5" -replace '^0[xX]',''),16)).CopyTo($req, 24)
                Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x81D)) -InBytes $req | Out-Null
                Write-Host 'mem-island: base discovered and validated'
                return
            }
            if ($v -eq 'read') {
                if ($null -eq $Arg2) { throw 'mem-island read: usage: mem-island read <hex_pa> [len]' }
                $pa = [Convert]::ToUInt64(("$Arg2" -replace '^0[xX]',''),16)
                $len = 8
                if ($null -ne $Arg3) { $len = [uint32]::Parse($Arg3) }
                $req = New-Object byte[] 16
                [BitConverter]::GetBytes([uint64]$pa).CopyTo($req, 0)
                [BitConverter]::GetBytes([uint64]$len).CopyTo($req, 8)
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x81D)) -InBytes $req -OutputLength 64
                $ok = [BitConverter]::ToUInt64($out, 0)
                $n  = [BitConverter]::ToUInt64($out, 8)
                if (-not $ok) { Write-Host ("mem-island read: REFUSED pa=0x{0:X}" -f $pa); return }
                $hex = (16..(16+$n-1) | ForEach-Object { '{0:X2}' -f $out[$_] }) -join ' '
                Write-Host ("mem-island read: pa=0x{0:X} n={1} bytes={2}" -f $pa, $n, $hex)
                return
            }
            throw 'mem-island: usage: mem-island [init ...|read <pa> [len]]'
        }
        'mem-scan-rev' {
            # Phase 4b (2026-10-11): reverse scan - find the VAs mapping a frame.
            # Forward scan asks "what is at this VA"; this asks "who points at
            # this frame". Engine A cannot express the second question: it has
            # no physical-address interface to search with.
            # usage: mem-scan-rev <pid> <hex_pa> [max] [start] [end]
            if ($null -eq $Arg1 -or $null -eq $Arg2) {
                throw 'mem-scan-rev: usage: mem-scan-rev <pid> <hex_pa> [max] [start] [end]'
            }
            $tpid = [uint32]::Parse($Arg1)
            $tpa  = [Convert]::ToUInt64(($Arg2 -replace '^0[xX]',''), 16)
            $maxR = 32
            if ($null -ne $Arg3) { $maxR = [uint32]::Parse($Arg3) }
            $rs = [uint64]0
            $re = [uint64]0
            if ($null -ne $Arg4 -and "$Arg4" -ne '') { $rs = [Convert]::ToUInt64(("$Arg4" -replace '^0[xX]',''),16) }
            if ($null -ne $Arg5 -and "$Arg5" -ne '') { $re = [Convert]::ToUInt64(("$Arg5" -replace '^0[xX]',''),16) }

            $req = New-Object byte[] 32
            [BitConverter]::GetBytes($tpid).CopyTo($req, 0)
            [BitConverter]::GetBytes([uint32]$maxR).CopyTo($req, 4)
            [BitConverter]::GetBytes([uint64]$tpa).CopyTo($req, 8)
            [BitConverter]::GetBytes([uint64]$rs).CopyTo($req, 16)
            [BitConverter]::GetBytes([uint64]$re).CopyTo($req, 24)

            $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x81C)) `
                                    -InBytes $req -OutputLength (24 + $maxR * 8)
            $ok   = [BitConverter]::ToUInt64($out, 0)
            $hits = [BitConverter]::ToUInt64($out, 8)
            $vis  = [BitConverter]::ToUInt64($out, 16)
            if (-not $ok) { Write-Host 'mem-scan-rev: FAILED'; return }
            Write-Host ("mem-scan-rev: pa=0x{0:X} hits={1} pages_visited={2}" -f $tpa, $hits, $vis)
            for ($i = 0; $i -lt $hits; $i++) {
                $va = [BitConverter]::ToUInt64($out, 24 + $i*8)
                Write-Host ("  va=0x{0:X}" -f $va)
            }
        }
        'mem-coop' {
            # Phase 7 (2026-10-11): guard cooperation diagnostics.
            # A write to a page the sync guard protects is normally undone by
            # its next poll (the poll compares against a snapshot and restores
            # on any difference). The coop path tells the guard the change is
            # deliberate and refreshes its baseline first, so our write
            # persists while protection stays armed. This reports how often
            # that happened.
            if ($null -eq $Arg1) {
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x81B)) -OutputLength 40
                Write-Host ("mem-coop: notifies={0} guarded={1} unguarded={2} rollbacks_avoided={3}" -f `
                    [BitConverter]::ToUInt64($out,0), [BitConverter]::ToUInt64($out,8),
                    [BitConverter]::ToUInt64($out,16), [BitConverter]::ToUInt64($out,24))
                return
            }
            if ("$Arg1" -eq 'reset') {
                Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x81B)) `
                                 -InBytes ([BitConverter]::GetBytes([uint32]2)) | Out-Null
                Write-Host 'mem-coop: stats reset'
                return
            }
            throw 'mem-coop: usage: mem-coop [reset]'
        }
        'scan-pid' {
            # 206-C5 pilot helper: enumerate a foreign process's committed
            # PRIVATE read-write pages (VirtualQueryEx; kernel-mediated read,
            # never hits the NPT write protection). Pure userland - the ctl
            # device handle is opened only because this tool always requires
            # the driver service to be present.
            if ($null -eq $Arg1) { throw 'scan-pid: usage: scan-pid <pid> [count]' }
            $pidVal = [uint32]::Parse($Arg1)
            $max = if ($null -ne $Arg2) { [int]$Arg2 } else { 16 }
            $h = [YghvMem]::OpenProcess(0x0438, 0, $pidVal)  # QUERY|VM_READ|VM_OP
            if ($h -eq [IntPtr]::Zero) {
                throw ("scan-pid: OpenProcess {0} failed, err {1}" -f
                    $pidVal, [Runtime.InteropServices.Marshal]::GetLastWin32Error())
            }
            try {
                $addr = [UInt64]0
                $found = 0
                $scanned = 0
                while ($found -lt $max -and $scanned -lt 100000) {
                    $scanned++
                    $mbi = [YghvMem]::VirtualQueryEx($h, [IntPtr][Int64]$addr)
                    $rsize = [UInt64]$mbi.RegionSize.ToInt64()
                    if ($rsize -eq 0) { break }
                    $state = $mbi.State
                    $prot = $mbi.Protect
                    $type = $mbi.Type
                    $base = [UInt64]$mbi.BaseAddress.ToInt64()
                    if ($state -eq 0x1000 -and $type -eq 0x20000 -and
                        ($prot -eq 0x04 -or $prot -eq 0x40) -and
                        $base -ge 0x10000) {
                        # MEM_COMMIT + MEM_PRIVATE + PAGE_READWRITE(4) or
                        # PAGE_READWRITE+GUARD-adjacent RWX(40); skip low VAs
                        Write-Host ("scan-pid: va=0x{0:X} size=0x{1:X}" -f
                            $base, $rsize)
                        $found++
                    }
                    $addr = $base + $rsize
                    if ($addr -ge 0x800000000000) { break }
                }
                Write-Host ("scan-pid: pid={0} regions={1}" -f $pidVal, $found)
            } finally {
                [YghvMem]::CloseHandle($h) | Out-Null
            }
        }
        'wpm-write' {
            # 206-C5 pilot: cross-process write via the kernel API
            # (WriteProcessMemory). Under the current verdict semantics this
            # is a cpl=0 write with the TARGET's CR3 attached (MmCopyVirtual
            # Memory), so it takes the cpl==0/own-CR3 ALLOW branch and LANDS
            # by design - run_c17 documents this as the known kernel-mediated
            # trust gap, not as a failure.
            if ($null -eq $Arg1 -or $null -eq $Arg2 -or $null -eq $Arg3) {
                throw 'wpm-write: usage: wpm-write <pid> <hex_va> <hex_val>'
            }
            $pidVal = [uint32]::Parse($Arg1)
            $va = [Convert]::ToUInt64(($Arg2 -replace '^0[xX]', ''), 16)
            $val = [Convert]::ToUInt64(($Arg3 -replace '^0[xX]', ''), 16)
            $h = [YghvMem]::OpenProcess(0x0438, 0, $pidVal)
            if ($h -eq [IntPtr]::Zero) {
                throw ("wpm-write: OpenProcess {0} failed, err {1}" -f
                    $pidVal, [Runtime.InteropServices.Marshal]::GetLastWin32Error())
            }
            try {
                $bytes = [BitConverter]::GetBytes([UInt64]$val)
                $written = [UInt32]0
                $ok = [YghvMem]::WriteProcessMemory($h, [IntPtr][Int64]$va,
                    $bytes, [UInt32]8, [ref]$written)
                if ($ok) {
                    Write-Host ("wpm-write: pid={0} va=0x{1:X} WROTE {2} bytes" -f
                        $pidVal, $va, $written)
                } else {
                    Write-Host ("wpm-write: pid={0} va=0x{1:X} FAILED err=0x{2:X}" -f
                        $pidVal, $va,
                        [Runtime.InteropServices.Marshal]::GetLastWin32Error())
                }
                $rb = New-Object byte[] 8
                $read = [UInt32]0
                if ([YghvMem]::ReadProcessMemory($h, [IntPtr][Int64]$va, $rb,
                        [UInt32]8, [ref]$read)) {
                    Write-Host ("wpm-write: readback=0x{0:X}" -f
                        [BitConverter]::ToUInt64($rb, 0))
                } else {
                    Write-Host ("wpm-write: readback FAILED err=0x{0:X}" -f
                        [Runtime.InteropServices.Marshal]::GetLastWin32Error())
                }
            } finally {
                [YghvMem]::CloseHandle($h) | Out-Null
            }
        }
        'wpm-read' {
            # 9.299 (C line): pure read of a target's 8 bytes - NO write.
            # Needed to verify the sync guard's rollback: a write-based probe
            # (wpm-write with value 0) perturbs the page it is trying to
            # measure, and reads back before the 50ms poll can act.
            if ($null -eq $Arg1 -or $null -eq $Arg2) {
                throw 'wpm-read: usage: wpm-read <pid> <hex_va>'
            }
            $pidVal = [uint32]::Parse($Arg1)
            $va = [Convert]::ToUInt64(($Arg2 -replace '^0[xX]', ''), 16)
            $h = [YghvMem]::OpenProcess(0x0410, 0, $pidVal)   # PROCESS_VM_READ
            if ($h -eq [IntPtr]::Zero) {
                throw ("wpm-read: OpenProcess {0} failed, err {1}" -f
                    $pidVal, [Runtime.InteropServices.Marshal]::GetLastWin32Error())
            }
            try {
                $rb = New-Object byte[] 8
                $read = [UInt32]0
                if ([YghvMem]::ReadProcessMemory($h, [IntPtr][Int64]$va, $rb,
                    [UInt32]8, [ref]$read)) {
                    Write-Host ("wpm-read: pid={0} va=0x{1:X} value=0x{2:X}" -f
                        $pidVal, $va, [BitConverter]::ToUInt64($rb, 0))
                } else {
                    Write-Host ("wpm-read: pid={0} va=0x{1:X} READ FAILED err=0x{2:X}" -f
                        $pidVal, $va, [Runtime.InteropServices.Marshal]::GetLastWin32Error())
                }
            } finally {
                [YghvMem]::CloseHandle($h) | Out-Null
            }
        }
        default {
            throw ("unknown command: {0}" -f $Command)
        }
    }
} finally {
    if ($handle -ne [IntPtr]::Zero) {
        [YghvCtlNative]::CloseHandle($handle) | Out-Null
        $handle = [IntPtr]::Zero
    }
}
