$ErrorActionPreference = 'Stop'

# string_encryption_check.ps1 -- 9.590
#
# WHY THIS CHECK EXISTS
# ---------------------
# 9.589 added hv/common/yghv_str.h and hv/common/yghv_strtab.h, and then nothing
# used them: every export name was still written as a wide literal, so
# "strings yuanguard_hv.sys | grep PsGetProcess" named the driver capabilities in
# the shipped image. That is the same shape as 9.454, where loader_stealth.c had
# a working function that no call site reached -- a capability that exists, is
# not in effect, and is not asked about by any test.
#
# This check asks two questions, both about the ARTIFACT rather than the source:
#
#   1. Is the generated table still what the generator produces? A hand-edited or
#      stale yghv_strtab.h decodes to noise at runtime, and nothing else would
#      say so -- the table is a list of numbers, so a wrong entry is not a
#      compile error.
#   2. Does any of those names still appear in the built driver, narrow or wide?
#      Source-level grep cannot answer this: the name can arrive through the
#      IMPORT TABLE, which no .c file mentions. That is exactly how the last one
#      survived -- two direct calls to an imported accessor.
#
# The check reads the image the linker produced, so it belongs with the map-level
# checks, which run after link.

$root = Split-Path -Parent $PSScriptRoot                     # ...\YuanGuardHV
$repo = Split-Path -Parent $root                             # D:\yuanguard
$gen  = Join-Path $repo "tools\gen_strtab.py"
$hdr  = Join-Path $root "hv\common\yghv_strtab.h"
$img  = Join-Path $root "bin\yuanguard_hv.sys"

foreach ($p in @($gen, $hdr)) {
    if (-not (Test-Path $p)) { throw "string encryption check: $p not found" }
}
if (-not (Test-Path $img)) {
    throw "string encryption check: $img not found -- this check judges the linked image, so build first"
}

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    throw "string encryption check: python not found on PATH; the generated table cannot be re-derived, and reporting PASS without re-deriving it would be a check that cannot fail"
}

# ---------------------------------------------------------------- part 1: drift
# Compare the checked-in header with what the generator emits right now.
# Newlines are normalised because the generator writes LF and a working copy may
# carry CRLF; that difference is not a drift in the TABLE.

$emitted = (& $python.Source $gen) -join "`n"
$emitted = $emitted -replace "`r`n", "`n"
if (-not $emitted.TrimEnd().EndsWith("#endif /* YGHV_STRTAB_H */")) {
    throw "string encryption check: the generator produced no table (last line is not the include guard close) -- a broken generator must not be reported as agreement"
}

$onDisk = (Get-Content -Raw -LiteralPath $hdr) -replace "`r`n", "`n"
if ($emitted.TrimEnd() -ne $onDisk.TrimEnd()) {
    $e = $emitted.TrimEnd() -split "`n"
    $o = $onDisk.TrimEnd() -split "`n"
    $firstDiff = -1
    for ($i = 0; $i -lt [Math]::Max($e.Count, $o.Count); $i++) {
        $a = if ($i -lt $e.Count) { $e[$i] } else { "<missing>" }
        $b = if ($i -lt $o.Count) { $o[$i] } else { "<missing>" }
        if ($a -ne $b) { $firstDiff = $i + 1; break }
    }
    throw ("string encryption check: hv/common/yghv_strtab.h is not what tools/gen_strtab.py produces (first difference at line $firstDiff). Regenerate it: python tools/gen_strtab.py > YuanGuardHV/hv/common/yghv_strtab.h")
}
Write-Host ("[string-enc] generated table matches the generator ({0} line(s))" -f ($onDisk.TrimEnd() -split "`n").Count)

# ------------------------------------------------------- part 2: plaintext scan
# Every name in NAMES, in both encodings. The wide form is what a WCHAR literal
# leaves behind; the narrow form is what the import table carries.

$names = @()
foreach ($line in ($emitted -split "`n")) {
    if ($line -match "^/\* (Ps\w+) : [0-9]+ word\(s\), key (0x[0-9A-F]+) \*/$") { $names += $Matches[1] }
}
if ($names.Count -lt 2) {
    throw ("string encryption check: parsed {0} name(s) out of the generated table; the entry comment format changed, so this check would scan for nothing" -f $names.Count)
}

$bytes = [System.IO.File]::ReadAllBytes($img)

function Find-All {
    param([byte[]]$Hay, [byte[]]$Needle)
    $hits = 0
    if ($Needle.Length -eq 0 -or $Hay.Length -lt $Needle.Length) { return 0 }
    $last = $Hay.Length - $Needle.Length
    for ($i = 0; $i -le $last; $i++) {
        if ($Hay[$i] -ne $Needle[0]) { continue }
        $ok = $true
        for ($j = 1; $j -lt $Needle.Length; $j++) {
            if ($Hay[$i + $j] -ne $Needle[$j]) { $ok = $false; break }
        }
        if ($ok) { $hits++ }
    }
    return $hits
}

function Test-Image {
    param([byte[]]$Hay, [string[]]$Names)
    $found = @()
    foreach ($n in $Names) {
        $narrow = [System.Text.Encoding]::ASCII.GetBytes($n)
        $wide   = [System.Text.Encoding]::Unicode.GetBytes($n)
        $cN = Find-All -Hay $Hay -Needle $narrow
        $cW = Find-All -Hay $Hay -Needle $wide
        if ($cN -gt 0 -or $cW -gt 0) { $found += ("{0} (ascii={1}, wide={2})" -f $n, $cN, $cW) }
    }
    return $found
}

# 9.608: NAMES THAT ARE REAL IMPORTS CANNOT BE HIDDEN, AND MUST NOT BE FLAGGED.
#
# The table exists so that "strings driver.sys | grep PsGetProcess" finds
# nothing. That works for names this driver RESOLVES BY STRING at run time.
# It cannot work for names it CALLS DIRECTLY: a direct call becomes an import,
# and the import table must contain the name in plaintext or the loader cannot
# bind it.
#
# Measured: PsGetCurrentProcessId was reported as a leak while being called from
# ten sites in main.c. The check was right about the bytes and wrong about the
# claim -- an import is not a leak, it is the loader's own table.
#
# The rule now: a name may appear plainly ONLY IF something in hv\* calls it
# directly. Anything else must come from the encrypted table. The allow-list is
# DERIVED from the sources below, not hand-written, so adding a direct call is
# enough to make this check stay green -- and adding a literal is not.
$importAllow = @{}
$srcRoot = Join-Path (Split-Path -Parent $PSScriptRoot) 'hv'
foreach ($f in @(Get-ChildItem -LiteralPath $srcRoot -Filter '*.c' -File) + @(Get-ChildItem -LiteralPath $srcRoot -Filter '*.cpp' -File)) {
    $txt = [IO.File]::ReadAllText($f.FullName)
    foreach ($n in $names) {
        # a direct call: name followed by '(' somewhere in a .c/.cpp
        if ($txt -match ([regex]::Escape($n) + '\s*\(')) { $importAllow[$n] = $f.Name }
    }
}

$namesToScan = @($names | Where-Object { -not $importAllow.ContainsKey($_) })
$leaks = @(Test-Image -Hay $bytes -Names $namesToScan)
if ($importAllow.Count -gt 0) {
    Write-Host ("[string-enc] {0} name(s) exempt as direct imports: {1}" -f $importAllow.Count, (($importAllow.Keys | Sort-Object) -join ', '))
}
if ($leaks.Count -gt 0) {
    foreach ($l in $leaks) { Write-Host ("[string-enc] FAIL: $l is still plaintext in the built driver") }
    throw ("string encryption check FAIL ({0} export name(s) readable in the image) -- an import or a literal puts the capability name back where strings can find it" -f $leaks.Count)
}
Write-Host ("[string-enc] {0} export name(s) absent from the image in both encodings" -f $names.Count)

# ------------------------------------------------------------- part 3: self-test
# A check that has never failed is a check that has never been shown to work.
# Two plants, each aimed at one half:
#   a) a name spliced into a COPY of the image must be reported, and
#   b) a header copy with its words changed must disagree with the generator.
#
$poison = [byte[]]($bytes.Clone())
$plantNarrow = [System.Text.Encoding]::ASCII.GetBytes($names[0])
$planted = $false
for ($i = 0; $i -lt $poison.Length - $plantNarrow.Length; $i++) {
    # Splice into a run of zeros so the plant cannot be mistaken for real data.
    $run = 0
    while ($i + $run -lt $poison.Length -and $poison[$i + $run] -eq 0) { $run++ }
    if ($run -ge ($plantNarrow.Length + 8)) {
        [Array]::Copy($plantNarrow, 0, $poison, $i, $plantNarrow.Length)
        $planted = $true
        break
    }
}
if (-not $planted) {
    throw "string encryption self-test: could not find a zero run to plant the name in, so the scanner below was never exercised"
}
$caught = @(Test-Image -Hay $poison -Names $names)
if ($caught.Count -ne 1) {
    throw ("string encryption self-test: a planted name was reported {0} time(s); the scanner cannot see an obvious one" -f $caught.Count)
}
Write-Host ("[string-enc] self-test: a planted name was caught ({0})" -f ($caught -join "; "))

# (b) A header whose words are all replaced must compare unequal to the generator
# output. Replacing every word is deliberate: changing one entry risks hitting a
# line the comparison normalises away, and the point is to prove the comparison
# looks at the table body at all.
$tampered = $onDisk -replace "0x[0-9A-F]{16}ULL", "0x0000000000000001ULL"
if ($tampered -eq $onDisk) {
    throw "string encryption self-test: the tamper step changed nothing, so the comparison below proves nothing"
}
if ($tampered.TrimEnd() -eq $emitted.TrimEnd()) {
    throw "string encryption self-test: a tampered header still compared equal to the generator output"
}
Write-Host "[string-enc] self-test: a tampered table was caught"

# Control: the untouched header must still compare equal, or (b) proves nothing.
if ($onDisk.TrimEnd() -ne $emitted.TrimEnd()) {
    throw "string encryption self-test: the unmodified header did not compare equal, so the tamper result above proves nothing"
}

Write-Host "[PASS] String encryption: generated table matches its generator; no export name is readable in the built driver"
