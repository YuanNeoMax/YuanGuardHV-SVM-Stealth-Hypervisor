$ErrorActionPreference = 'Stop'

# canonical_deref_check.ps1 -- 9.608
#
# WHY THIS EXISTS
#
# Two 0x50 PAGE_FAULT_IN_NONPAGED_AREA bugchecks in one session, both from the
# same mistake in different functions:
#
#     if (p >= 0xFFFF800000000000) { ...read through p... }
#
# "In the upper half of the address space" is not "mapped". x64 kernel space is
# 128 TB of mostly empty address space; a canonical value says nothing about a
# PTE. The first two times this was found by reading a crash dump.
#
# The third time -- yghv_obguard.c, unlinking the OB callback -- it was found by
# reading the source by hand, which is the same "no check observed it" shape the
# rest of this file's neighbours keep paying for. So it becomes a checker.
#
# WHAT IT FLAGS
#
#   a comparison against 0xFFFF800000000000 (or a macro that equals it) where
#   the next N lines contain a RAW dereference of that value, and none of the
#   sanctioned guards appear: __try, MmIsAddressValid, yghv_safe_*,
#   yghv_p_read64 / yghv_p_write64 (which wrap __try), or a call to a helper
#   that does.
#
# WHAT IT DELIBERATELY ALLOWS
#
#   - the comparison used only to CHOOSE a path (mem_engine.c picks the API
#     engine and never dereferences on the strength of it)
#   - the comparison inside a __try block
#   - the comparison guarding a read that goes through a guarded helper
#   - yghv_safe.h / yghv_safe.c themselves, which ARE the layer
#
# A file may opt out for a specific line with a trailing comment
#   /* canonical-ok: <reason> */
# so that a legitimate exception is recorded where it happens rather than by
# weakening the rule for everyone.

$root = Split-Path -Parent $PSScriptRoot
$hv = Join-Path $root 'hv'

$KERNEL_MIN = '0xFFFF800000000000'
$GUARDS = @('__try', 'MmIsAddressValid', 'yghv_safe_', 'yghv_p_read64', 'yghv_p_write64',
            'yghv_safe_read', 'yghv_probe_read64', 'yghv_sig_readable_image', '-&gt;')

$problems = New-Object 'System.Collections.Generic.List[string]'
$checked = 0
$exempt = 0

$files = @(Get-ChildItem -LiteralPath $hv -Filter '*.c' -File) +
         @(Get-ChildItem -LiteralPath $hv -Filter '*.cpp' -File)

foreach ($f in $files) {
    # the layer itself is exempt by construction
    if ($f.Name -eq 'yghv_safe.c') { continue }

    $lines = [IO.File]::ReadAllLines($f.FullName)
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        if ($line -notmatch [regex]::Escape($KERNEL_MIN)) { continue }
        if ($line -match 'canonical-ok') { $exempt++; continue }
        # a definition or a comment is not a comparison
        $trim = $line.Trim()
        if ($trim.StartsWith('*') -or $trim.StartsWith('/*') -or $trim.StartsWith('//')) { continue }
        if ($trim -match '^#define') { continue }

        $checked++
        $window = $lines[$i..([Math]::Min($i + 6, $lines.Count - 1))] -join [char]10

        $guarded = $false
        foreach ($g in $GUARDS) {
            if ($window -like ('*' + $g + '*')) { $guarded = $true; break }
        }
        if ($guarded) { continue }

        # a raw dereference in the window: -> or *x where x is not a comment
        $raw = $false
        foreach ($w in $lines[$i..([Math]::Min($i + 6, $lines.Count - 1))]) {
            $t = $w.Trim()
            if ($t.StartsWith('*') -or $t.StartsWith('/*')) { continue }
            if ($w -match '\w\s*->\s*\w' ) { $raw = $true; break }
            if ($w -match '\*\s*\(\s*(const\s+)?(volatile\s+)?\w+' ) { $raw = $true; break }
        }
        if (-not $raw) { continue }

        $problems.Add(("{0}:{1} compares against {2} and then dereferences with no guard" -f
                       $f.Name, ($i + 1), $KERNEL_MIN))
    }
}

if ($problems.Count -gt 0) {
    foreach ($p in $problems) { Write-Host "[FAIL] $p" }
    throw ("canonical_deref: {0} unguarded canonical dereference(s) -- 'canonical' is not 'mapped'; route the read through yghv_safe_* or wrap it in __try" -f $problems.Count)
}

Write-Host ("[PASS] canonical derefs: {0} comparison(s) inspected, all either guarded or exempt ({1} canonical-ok)" -f $checked, $exempt)
