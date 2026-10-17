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
