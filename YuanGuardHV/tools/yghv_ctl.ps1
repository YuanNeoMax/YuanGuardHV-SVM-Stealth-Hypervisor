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
    [Parameter(Position = 7)]$Arg7 = $null
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

    public static void UnmapFile(YghvMapView v)
    {
        if (v == null) return;
        if (v.View != IntPtr.Zero) UnmapViewOfFile(v.View);
        if (v.Map != IntPtr.Zero) CloseHandle(v.Map);
        if (v.File != IntPtr.Zero) CloseHandle(v.File);
        v.View = v.Map = v.File = IntPtr.Zero;
    }

    /* 9.286: the AVE must be caught INSIDE this C# frame — PowerShell's
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

    /* 9.284: CSE-protected — a read right after a DENY-injected #PF can
     * still catch an in-flight AV; without the attribute the mmf-write
     * child dies unhandled before printing BLOCKED (run_c18 gate2). */
    [System.Runtime.ExceptionServices.HandleProcessCorruptedStateExceptions]
    [System.Security.SecurityCritical]
    public static long ReadVal(IntPtr view)
    {
        return Marshal.ReadInt64(view);
    }

    public static void WriteVal(IntPtr view, long value)
    {
        Marshal.WriteInt64(view, value);
    }

    /* One machine store, then report. The DENY verdict arrives as an injected
     * #PF (present, write, user) on this store; .NET raises it as an
     * AccessViolationException, which is a corrupted-state exception — only
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

# 206-C5: cross-process memory inspection helpers for the real-process pilot
# (scan-pid / wpm-write). Kernel-mediated reads/writes never hit the NPT write
# protection as cpl=3 user stores — wpm-write deliberately demonstrates that
# trust gap.
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

$sysCommands = @('set-auto-start', 'unset-auto-start', 'harden-service', 'unharden-service')
if ($sysCommands -contains $Command.ToLower()) {
    switch ($Command.ToLower()) {
        'set-auto-start' {
            & sc.exe config yuanguard start= auto | Out-Null
            if ($LASTEXITCODE -ne 0) {
                throw ("set-auto-start: sc config failed rc={0}" -f $LASTEXITCODE)
            }
            Write-Host 'set-auto-start: OK (start=auto)'
            exit 0
        }
        'unset-auto-start' {
            & sc.exe config yuanguard start= demand | Out-Null
            if ($LASTEXITCODE -ne 0) {
                throw ("unset-auto-start: sc config failed rc={0}" -f $LASTEXITCODE)
            }
            Write-Host 'unset-auto-start: OK (start=demand)'
            exit 0
        }
        'harden-service' {
            $backup = (& sc.exe sdshow yuanguard | Select-Object -Last 1).Trim()
            if (-not $backup) {
                throw 'harden-service: sdshow returned nothing'
            }
            Set-Content -LiteralPath 'D:\aaaaaavm\yghv_service_sddl_backup.txt' `
                -Value $backup -Encoding UTF8
            $hardenSddl = 'D:(D;;SDWP;;;BA)(A;;CCLCSWRPWPDTLOCRRC;;;SY)' +
                '(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)' +
                '(A;;CCLCSWLOCRRC;;;IU)(A;;CCLCSWLOCRRC;;;SU)' +
                'S:(AU;FA;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;WD)'
            $out = & sc.exe sdset yuanguard $hardenSddl
            if (($out -join ' ') -notmatch 'SUCCESS') {
                throw ("harden-service: sdset failed: {0}" -f ($out -join ' '))
            }
            Write-Host 'harden-service: OK (service delete denied, backup saved)'
            exit 0
        }
        'unharden-service' {
            $backupPath = 'D:\aaaaaavm\yghv_service_sddl_backup.txt'
            if (-not (Test-Path $backupPath)) {
                throw 'unharden-service: backup SDDL not found'
            }
            $backup = (Get-Content -LiteralPath $backupPath -Raw).Trim()
            $out = & sc.exe sdset yuanguard $backup
            if (($out -join ' ') -notmatch 'SUCCESS') {
                throw ("unharden-service: sdset failed: {0}" -f ($out -join ' '))
            }
            Write-Host 'unharden-service: OK (backup SDDL restored)'
            exit 0
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
            # 9.258 (206-C3): watchdog test — arm a page then die WITHOUT any
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
                Write-Host 'selftest-abort: start OK — dying WITHOUT cleanup'
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
            # 9.299 (C line): SYNC GUARD — snapshot + poll, no NPT permission
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
            # 9.302 (A phase): a PASSIVE target — maps the section, publishes
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
            # Arming those made the guard roll back live code/data — that is how
            # the 0x7E crash was reached, and it was a test-design error.
            $mapSize = if ($null -ne $Arg4) { [uint32]::Parse($Arg4) } else { [uint32]4096 }
            $map = [YghvMmf]::MapFile($path, $mapSize)
            $pidVal = [YghvCtlNative]::GetCurrentProcessId()
            # Seed a recognisable, constant pattern ONCE so the page has a
            # known baseline (the guard snapshots it at arm time).
            $seed = [long]0x00C4C40000000000
            try { [YghvMmf]::WriteVal($map.View, $seed) } catch {}
            $line = ("mmf-hold: pid={0} va=0x{1:X} seed=0x{2:X} size=0x{3:X}" -f $pidVal, $map.View.ToInt64(), $seed, $mapSize)
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
            # Resolves a VA through the target's CR3 by hand —
            # CR3 -> PML4 -> PDPT -> PD -> PT — reading only the table pages
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
        'mmf-read' {
            # 9.366: THE GUEST'S OWN READ OF THE PAGE — the half `mmf-write`
            # never covered.
            #
            # Why a separate command: `mem-read` reads through the DRIVER
            # (Engine A/B, our image's RIP), so it cannot distinguish a page the
            # guest can see from one it cannot — both return whatever our side
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
            # alias a target frame, so a later access is a plain load/store —
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
            # HONEST: this changes the GUEST's behaviour too — uncached memory
            # is dramatically slower for the guest, and a target that measures
            # its own throughput can detect it. This is not a stealth feature.
            # "Per-observer" needs the split-view mechanism: set the mode in
            # the ALT table and register an observer (mem-view).
            # usage: mem-cache [set <gpa> <mode> | setalt <gpa> <mode> | reset <gpa>]
            if ($null -eq $Arg1) {
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x822)) -OutputLength 48
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
            # entry point and handed a PHYSICAL address to mem-write instead —
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
            Write-Host ("sys-crypto: SHA-256 selftest " + $(if($ok -eq 1){'PASS (fips-180-4 vectors match)'}else{'FAIL — hashes are NOT trustworthy'}))
        }
        'sys-sentry' {
            # Tier 7.4/7.5/7.6 (2026-10-18): three sides of one subject — how
            # the hypervisor behaves when someone is looking at it or attacking.
            #
            # 7.4 WHAT A GUEST CAN MEASURE ABOUT US
            #   Time-based detection is the probe a guest can always run on
            #   itself: rdtsc around a hypervisor call, cpuidex leaf 1 /
            #   0x40000000. The cost of a VMEXIT is what leaks. This keeps a
            #   running worst case so a regression that makes us slow — and
            #   therefore loud — is visible here first.
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
                    Write-Host "  (no exit measurements yet — the VMEXIT path records them)"
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
                # inventory would have been TRUNCATED SILENTLY — the count would
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
            throw 'sys-sentry: use surface|reset'
        }
        'sys-guard' {
            # Tier 7.1/7.3 (2026-10-18): self-hiding + integrity self-check.
            #
            # THE EXPOSURE this answers: our tables are ordinary physical pages
            # below 4GB (MmAllocateContiguousMemory). The NPT identity-maps
            # physical memory for the guest, so a guest walking its own address
            # space can read AND WRITE them — an NPT entry, a breakpoint slot, a
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
                # 9.366: ask ONE page why a hide would be refused — without
                # performing one. See control_device.c case 7 for the answer's
                # layout and yghv_guard.c for why a bitmask rather than a code.
                if ($null -eq $Arg2) { throw 'sys-guard why: usage: sys-guard why <hex_pa>' }
                $a = [Convert]::ToUInt64(("$Arg2" -replace '^0[xX]',''),16)
                $req7 = New-Object byte[] 32
                [BitConverter]::GetBytes([uint64]7).CopyTo($req7, 0)
                [BitConverter]::GetBytes([uint64]$a).CopyTo($req7, 8)
                $o7 = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x826)) -InBytes $req7 -OutputLength 40
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
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x826)) -InBytes $req -OutputLength 256
                $n = [BitConverter]::ToUInt64($out, 0)
                Write-Host ("sys-guard list: {0} page(s)" -f $n)
                for ($i = 0; $i -lt $n; $i++) {
                for ($i = 0; $i -lt $n; $i++) {
                    # 9.363: stride is 10 qwords = 80 bytes, per the C side
                    # (control_device.c case 3):
                    #   e[0]=gpa  e[1]=checks  e[2]=changes  e[3]=hidden
                    #   e[4]=hash_off|hash_len  e[5..8]=digest  e[9]=tag|armed
                    #
                    # The parser had $o = 8 + $i*48, i.e. 48-byte entries, so it read
                    # checks from +32 and changes from +16 — both wrong, and the tag
                    # from +40..47, which is inside the digest. It looked plausible
                    # only because a stale value happened to sit where it read.
                    # Measured: "pa=0x1F1952000 ... tag=lñµ}Yç" — one real entry
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
            # KeBugCheckEx — a feature hitting an unexpected state and the
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
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x823)) -OutputLength 112
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
            # Tier 4.6 (2026-10-17): NPF watchpoints — the debug-register superset.
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
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x820)) -OutputLength 48
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
            # Tier 4.2 (2026-10-17): time-dimension control — O(1) snapshot/rollback.
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
                $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x821)) -OutputLength 48
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
            $out = Invoke-YghvIoctl -Code ([YghvCtlNative]::IoCtl(0x821)) -InBytes $req -OutputLength 48
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
            #      depending on who reads it — the target sees its real frame,
            #      a registered observer sees a decoy frame.
            # 4.3: process A's VA points at process B's frame, established
            #      unilaterally (no IPC, no handle, neither process told).
            #
            # Both reduce to one primitive, and 9.311 measured that it works
            # on this CPU: rewriting an NPT entry so a GPA resolves to a
            # different SPA IS observed. (9.297/9.298's "NPT ignores software
            # flushes" was about PERMISSION flips — a different mechanism.)
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
            # the kernel's direct map — reading a frame becomes a plain load
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
            # Phase 4b (2026-10-11): reverse scan — find the VAs mapping a frame.
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
            # never hits the NPT write protection). Pure userland — the ctl
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
            # by design — run_c17 documents this as the known kernel-mediated
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
            # 9.299 (C line): pure read of a target's 8 bytes — NO write.
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
