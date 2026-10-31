$ErrorActionPreference = 'Stop'

# ioctl_buffer_size.ps1 — 9.483
#
# WHY THIS EXISTS: three separate defects in this tree were the same shape - a
# reply-size constant grew on the driver side and a client call site did not
# follow. Measured instances:
#
#   sys-guard list   256 bytes while the driver replied 8 + 32*80 = 2568. The 4th
#                    entry threw IndexOutOfRangeException AFTER the header had
#                    printed "4 page(s)", so the count and the rows disagreed with
#                    no explanation.
#   exit-mask        40 bytes on the SET path while the read-back path used 88.
#                    The driver requires 11 qwords, so the set returned
#                    STATUS_BUFFER_TOO_SMALL - the mask WAS applied to 12 live
#                    VMCBs and the client reported a failure.
#
# Both report FAILURE while the driver is healthy, which is the worst way to be
# wrong: the user concludes "that feature does not work".
#
# THE RULE, AND WHERE THE FIRST TWO VERSIONS GOT IT WRONG
#
# Naive rule: "every client OutputLength for code X must be >= X's OUT_QWORDS*8".
# That is WRONG and produced 19 false failures on a healthy tree, because many
# calls deliberately ask for ONE qword: `sys-guard add` returns a slot number,
# `del` returns a boolean, `sentry` sub-ops return a count. The driver writes its
# full reply only when the caller has room for it (`if (out_len >= N * 8)`), and
# for those calls a 1-qword ask is the CORRECT contract - the status code carries
# the answer.
#
# Second rule also wrong: "require full size everywhere" fails the tree above.
#
# THE ACTUAL RULE - what matters is whether the caller READS BEYOND the first
# qword. A call that later does `[BitConverter]::ToUInt64($out, 8)` or reads offset
# 16/24/32... from the same buffer NEEDS the larger size, and a call that only
# inspects `out[0]` or the returned status does not. So the check follows each
# call's variable to the offsets it actually reads, and only complains when a
# read exceeds what was requested.
#
# This is a CROSS-FILE, CROSS-LANGUAGE consistency question - the constant lives in
# a C header, the reads live in PowerShell and Java, and neither compiler can see
# the other side. That is exactly what an automated check is for.

$repoRoot = Split-Path -Parent $PSScriptRoot
$hdr = Join-Path $repoRoot 'hv\common\control_ioctl.h'
$ps1 = Join-Path $repoRoot 'tools\yghv_ctl.ps1'

foreach ($f in @($hdr, $ps1)) {
    if (-not (Test-Path $f)) { throw "ioctl_buffer_size: cannot find $f" }
}

$hdrText = Get-Content $hdr -Raw

# ---------------------------------------------------------------------------
# Every *_OUT_QWORDS constant, and every IOCTL code's OUT_QWORDS name.
# ---------------------------------------------------------------------------
$qwords = @{}
foreach ($m in [regex]::Matches($hdrText, '(?m)^\s*#define\s+(YGHV_[A-Z0-9_]*_OUT_QWORDS)\s+(\d+)')) {
    $qwords[$m.Groups[1].Value] = [int]$m.Groups[2].Value
}
if ($qwords.Count -eq 0) {
    throw 'ioctl_buffer_size: found no YGHV_*_OUT_QWORDS constants - the parser is wrong, not the code'
}

$ioctlCodes = @{}          # IOCTL short name -> 0x code
foreach ($m in [regex]::Matches($hdrText, '(?s)#define\s+IOCTL_YGHV_([A-Z0-9_]+)\s*\\?\s*\r?\n?\s*YGHV_CTL_CODE\([^,]+,\s*(0x[0-9A-Fa-f]+)')) {
    $ioctlCodes[$m.Groups[1].Value] = $m.Groups[2].Value.ToLower()
}

function Get-RequiredBytes([string]$code) {
    foreach ($k in $ioctlCodes.Keys) {
        if ($ioctlCodes[$k] -ne $code) { continue }
        $name = "YGHV_${k}_OUT_QWORDS"
        if ($qwords.ContainsKey($name)) { return @{ Name = $name; Bytes = $qwords[$name] * 8 } }
    }
    return $null
}

# ---------------------------------------------------------------------------
# 9.483b: which IOCTLs ADAPT their reply to out_len instead of gating on it.
#
# `case IOCTL_YGHV_SENTRY` writes `for (i = 0; i < n && (1 + (i+1)*3) <= (out_len /
# sizeof(UINT64)); i++)`, i.e. it returns as many entries as fit and reports how
# many it wrote. That is a DELIBERATE partial reply, so a client asking for 24
# bytes is correct there - and the naive rule (which demanded the full
# OUT_QWORDS size) produced false failures on it.
#
# Detected structurally from the driver source rather than hard-coded by code
# number, so adding an adaptive handler later does not silently look like a bug:
# a case body that divides `out_len` by sizeof(UINT64) inside a loop bound is
# adaptive. A case that tests `out_len >= N * sizeof(UINT64)` as a gate is not.
# ---------------------------------------------------------------------------
$drvText = Get-Content (Join-Path $repoRoot 'hv\control_device.c') -Raw
$adaptiveCodes = @{}
#
# 9.483c: SCAN BY LINES, NOT BY A LOOKAHEAD REGEX.
#
# The first version used a `(?s)case ... (?=\r?\n\s*case ...)` lookahead and
# matched NOTHING - measured, `[regex]::Match($drv, 'case\s+IOCTL_YGHV_SENTRY:...')`
# returned Success = False. Inside a single-quoted PowerShell string `\r?\n` is
# literal regex, which should have worked; the lookahead alternation spanning the
# `default:` arm is what failed. Rather than keep debugging a regex whose failure
# mode is "silently finds nothing", the case bodies are delimited by line numbers.
#
# A case body is [case line, next case/default line). If that body divides out_len
# by sizeof(UINT64) inside a loop bound, the handler ADAPTS its reply to the
# caller's buffer instead of gating on it.
#
$drvLines = Get-Content (Join-Path $repoRoot 'hv\control_device.c')
$caseStarts = @()
for ($i = 0; $i -lt $drvLines.Count; $i++) {
    if ($drvLines[$i] -match '^\s+case\s+IOCTL_YGHV_([A-Z0-9_]+)\s*:') {
        $caseStarts += [pscustomobject]@{ Name = $matches[1]; Line = $i }
    }
}
for ($k = 0; $k -lt $caseStarts.Count; $k++) {
    $end = if ($k + 1 -lt $caseStarts.Count) { $caseStarts[$k + 1].Line } else { $drvLines.Count }
    $body = ($drvLines[$caseStarts[$k].Line..([Math]::Max($caseStarts[$k].Line, $end - 1))] -join "`n")
    if ($body -match 'out_len\s*/\s*sizeof\s*\(\s*UINT64\s*\)') {
        $adaptiveCodes[$caseStarts[$k].Name] = $true
    }
}
Write-Host "  adaptive handlers detected: $($adaptiveCodes.Keys -join ', ')"

$psText = Get-Content $ps1 -Raw
$findings = @()
$checked = 0

# ---------------------------------------------------------------------------
# Walk every `IoCtl(0xNNN)` call site. For each, capture:
#   - the OutputLength requested (may be on a following line)
#   - the variable the result is assigned to
#   - every numeric offset that variable is later read at, within a bounded window
#
# A site is only a finding when an offset read exceeds the requested length.
# ---------------------------------------------------------------------------
# Match the assignment and the IoCtl call together: `$var = Invoke-YghvIoctl -Code
# (...IoCtl(0xNNN)) ... -OutputLength N`
foreach ($m in [regex]::Matches($psText, '(?s)\$([A-Za-z_][A-Za-z0-9_]*)\s*=\s*Invoke-YghvIoctl\s*-Code\s*\(\s*\[YghvCtlNative\]::IoCtl\((0x[0-9A-Fa-f]+)\)\s*\)[\s\S]{0,300}?-OutputLength\s+(\d+)')) {
    $var  = $m.Groups[1].Value
    $code = $m.Groups[2].Value.ToLower()
    $len  = [int]$m.Groups[3].Value
    $checked += 1

    $req = Get-RequiredBytes $code
    if ($null -eq $req) { continue }

    # What does this call read out of the buffer? Look ahead from the END of this
    # match for reads on the SAME variable.
    #
    # 9.483: THE WINDOW MUST START AFTER THE MATCH, NOT 10 CHARACTERS INTO IT.
    #
    # The first version did `$tail.IndexOf('IoCtl(', 10)` on a tail that began at
    # the match START - i.e. it searched for the next IoCtl from inside the
    # assignment it had just matched, found its own `IoCtl(`, and cut the window
    # to nothing. Result: the checker saw no reads at all, reported PASS on a
    # tree containing the exact defect it was written to catch.
    #
    # Starting at the match END means the first `IoCtl(` it can find belongs to a
    # genuinely later call.
    $winStart = $m.Index + $m.Length
    $winEnd = [Math]::Min($psText.Length, $winStart + 3000)
    $tail = $psText.Substring($winStart, $winEnd - $winStart)

    # Stop at the next call site so another call's reads are not attributed here.
    $nextCall = $tail.IndexOf('IoCtl(')
    if ($nextCall -gt 0) { $tail = $tail.Substring(0, $nextCall) }

    $maxOffset = 0
    foreach ($r in [regex]::Matches($tail, "\`$$var\s*,\s*(\d+)|\`$$var\s*\[\s*(\d+)\s*\]")) {
        $off = if ($r.Groups[1].Success) { [int]$r.Groups[1].Value }
               else { [int]$r.Groups[2].Value }
        if ($off -gt $maxOffset) { $maxOffset = $off }
    }

    # A read at offset K needs at least K+8 bytes.
    $neededByOffset = $maxOffset + 8

    # 9.483: TWO SEPARATE TESTS, AND THE SECOND IS THE ONE THAT MATTERS HERE.
    #
    #   (1) does the buffer cover the offsets this call reads?
    #   (2) does the buffer satisfy the driver's own `out_len >= N * 8` gate?
    #
    # The 9.482 case passes (1) and fails (2): reading offset 32 needs exactly 40
    # bytes, which is what the broken call asked for - so an offset-only rule sees
    # a perfectly consistent call. The driver, however, writes its whole reply only
    # when the caller has room for ALL of it (11 qwords = 88), and otherwise
    # returns STATUS_BUFFER_TOO_SMALL. So "fits what I read" is not the contract;
    # "fits what the driver sends" is.
    #
    # Requiring (2) unconditionally would reintroduce the 19 false failures from
    # the naive version, because a call that only inspects the status code
    # legitimately asks for 1 qword. So (2) applies ONLY to calls that read
    # beyond offset 0 - those are the ones that depend on the full reply.
    if ($maxOffset -gt 0 -and $len -lt $req.Bytes) {
        # 9.483b: an adaptive handler returns whatever fits, so a smaller ask is
        # a valid request for a partial reply - not a defect.
        $isAdaptive = $false
        foreach ($k in $ioctlCodes.Keys) {
            if ($ioctlCodes[$k] -eq $code -and $adaptiveCodes.ContainsKey($k)) { $isAdaptive = $true }
        }
        if (-not $isAdaptive) {
            $line = ($psText.Substring(0, $m.Index) -split "`n").Count
            $findings += ("line $line : `$$var <- IOCTL 0x$code requests $len bytes " +
                          "and reads offset $maxOffset, but $($req.Name) = " +
                          "$($req.Bytes) bytes and the driver refuses a smaller reply")
        }
    }
    elseif ($neededByOffset -gt $len) {
        $line = ($psText.Substring(0, $m.Index) -split "`n").Count
        $findings += ("line $line : `$$var <- IOCTL 0x$code requests $len bytes " +
                      "but reads offset $maxOffset (needs $neededByOffset)")
    }
}

Write-Host "  $checked PowerShell call site(s) with an OutputLength; $($qwords.Count) OUT_QWORDS constant(s)"

if ($checked -eq 0) {
    throw 'ioctl_buffer_size: matched no call site - the parser is wrong, not the code'
}

if ($findings.Count -gt 0) {
    foreach ($f in $findings) { Write-Host "[FAIL] $f" }
    throw "ioctl_buffer_size: $($findings.Count) undersized client buffer(s)"
}

Write-Host '[PASS] IOCTL buffer sizes: every client buffer covers the offsets it reads'
