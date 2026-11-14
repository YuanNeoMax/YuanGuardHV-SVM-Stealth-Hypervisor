# struct_layout_check.ps1 — 9.605: verify C struct sizes match PS byte layouts
# Each IOCTL request struct must be the exact size the PS client allocates.
$ErrorActionPreference = 'Stop'

$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$hdr = Join-Path $root 'YuanGuardHV/hv/common/control_ioctl.h'
$ps  = Join-Path $root 'YuanGuardHV/tools/yghv_ctl.ps1'

$hdrText = Get-Content -Raw $hdr
$psText  = Get-Content -Raw $ps

$failures = @()

# --- C struct sizes (from the header) ---
function Get-CStructSize([string]$name) {
    $pattern = "typedef struct (?:\w+\s+)?\{([^}]+)\} $name;"
    if ($hdrText -match $pattern) {
        $body = $matches[1]
        $size = 0
        foreach ($line in ($body -split "
")) {
            $line = $line.Trim()
            if ($line -match '^(ULONG|ULONG_PTR|HANDLE|UCHAR|UINT64|uint64_t|uint32_t|uint16_t)\s+') {
                $type = $matches[1]
                $field = $line -replace '^(\w+)\s+', ''
                # Extract array size if present
                if ($field -match '\[(\d+)\]') {
                    $arrSize = [int]$matches[1]
                    $field = $field -replace '\[\d+\]', ''
                } else {
                    $arrSize = 1
                }
                # Field size
                switch ($type) {
                    'UCHAR'     { $sz = 1 }
                    'uint16_t'  { $sz = 2 }
                    'uint32_t'  { $sz = 4 }
                    'ULONG'     { $sz = 4 }
                    'HANDLE'    { $sz = 8 }
                    'ULONG_PTR' { $sz = 8 }
                    'UINT64'    { $sz = 8 }
                    'uint64_t'  { $sz = 8 }
                    default     { $sz = 8 }
                }
                $size += $sz * $arrSize
            }
        }
        return $size
    }
    return -1
}

# --- PS byte allocations (from the client) ---
function Get-PSAllocSize([string]$pattern) {
    if ($psText -match $pattern) {
        $expr = $matches[1]
        # Evaluate simple arithmetic expressions like "40 + $n"
        $expr = $expr -replace '\$n', '0'
        $expr = $expr -replace '\(\[uint32\]\)', ''
        $expr = $expr.Trim()
        if ($expr -match '^(\d+)\s*\+\s*(\d+)$') {
            return [int]$matches[1] + [int]$matches[2]
        }
        return [int]$expr
    }
    return -1
}

# Test cases: struct name, expected PS allocation
$tests = @(
    @{ Name='yghv_ioctl_proc_req_t';    Pattern='New-ProcReq.*?New-Object byte\[\] (\d+)';  Expect=56 }
    @{ Name='yghv_ioctl_inject_req_t';  Pattern='New-InjReq.*?New-Object byte\[\] (\d+)';  Expect=40 }
    @{ Name='yghv_ioctl_pt_req_t';      Pattern='New-PtReq.*?New-Object byte\[\] (\d+)';   Expect=32 }
    @{ Name='yghv_ioctl_win_req_t';     Pattern='New-WinReq.*?New-Object byte\[\] (\d+)';  Expect=40 }
)

foreach ($t in $tests) {
    $cSize = Get-CStructSize $t.Name
    if ($cSize -eq -1) {
        $failures += "struct $($t.Name): NOT FOUND in header"
        continue
    }
    if ($cSize -ne $t.Expect) {
        $failures += "struct $($t.Name): C size is $cSize, expected $($t.Expect)"
        continue
    }
    # PS allocation check (best-effort)
    $psSize = Get-PSAllocSize $t.Pattern
    if ($psSize -ge 0 -and $psSize -ne $cSize) {
        $failures += "struct $($t.Name): PS allocates $psSize bytes, C struct is $cSize bytes"
        continue
    }
    Write-Output "[PASS] $($t.Name): C=$cSize bytes, PS allocates $psSize bytes"
}

# --- Op code uniqueness check ---
$opPattern = '#define YGHV_PROC_OP_(\w+)\s+(\d+)'
$opCodes = @{}
foreach ($m in [regex]::Matches($hdrText, $opPattern)) {
    $name = $m.Groups[1].Value
    $code = [int]$m.Groups[2].Value
    if ($opCodes.ContainsKey($code)) {
        $failures += "op code $code used by both $($opCodes[$code]) and $name"
    } else {
        $opCodes[$code] = $name
    }
}
Write-Output "[PASS] op codes: $($opCodes.Count) unique values"

# --- Error code uniqueness check ---
$errPattern = '#define YGHV_\w+_E_(\w+)\s+(\d+)'
$errCodes = @{}
foreach ($m in [regex]::Matches($hdrText, $errPattern)) {
    $name = $m.Groups[1].Value
    $code = [int]$m.Groups[2].Value
    if ($errCodes.ContainsKey($code)) {
        $failures += "error code $code used by both $($errCodes[$code]) and $name"
    } else {
        $errCodes[$code] = $name
    }
}
Write-Output "[PASS] error codes: $($errCodes.Count) unique values"

# --- IOCTL function number uniqueness ---
$ioctlPattern = 'YGHV_CTL_CODE\(YGHV_IOCTL_DEVICE_TYPE,\s*(0x[0-9A-Fa-f]+)'
$fnCodes = @{}
foreach ($m in [regex]::Matches($hdrText, $ioctlPattern)) {
    $code = $m.Groups[1].Value
    if ($fnCodes.ContainsKey($code)) {
        $failures += "IOCTL function $code used more than once"
    } else {
        $fnCodes[$code] = 1
    }
}
Write-Output "[PASS] IOCTL function numbers: $($fnCodes.Count) unique values"

if ($failures.Count -gt 0) {
    foreach ($f in $failures) { Write-Output "[FAIL] $f" }
    throw "$($failures.Count) struct layout check(s) failed"
}
Write-Output "[PASS] struct layout: all checks passed"
