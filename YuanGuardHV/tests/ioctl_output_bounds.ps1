$ErrorActionPreference = 'Stop'

# ioctl_output_bounds.ps1 — 9.488
#
# EVERY STORE INTO THE SYSTEM BUFFER NEEDS AN out_len CHECK BEFORE IT.
#
# Under METHOD_BUFFERED, `irp->AssociatedIrp.SystemBuffer` is max(in_len, out_len)
# bytes. A handler that writes reply qwords without first proving the caller asked
# for that much writes past the end of the allocation - and the amount written is
# decided by the CALLER, not by us. That is a kernel-heap overwrite with a
# user-controlled length.
#
# MEASURED INSTANCE: `case IOCTL_YGHV_SECTION_HIDE` did
#
#     out[3] = (UINT64)idx;        // unconditional
#
# with no out_len test anywhere in the case. The shipped client always sends 32
# bytes, so it never fired - "the current client is polite" is not an interface
# property. Fixed in 9.488 by gating on YGHV_SECTION_HIDE_OUT_QWORDS.
#
# THE RULE: every `case IOCTL_YGHV_*` body that writes through `out[`, `*out`,
# `out->`, or `buf[` must mention `out_len` somewhere in that same body.
#
# WHY THIS IS CHECKABLE AND NOT TOO STRICT: the reply-size constants already exist
# as YGHV_*_OUT_QWORDS, so a compliant handler is one line. Every other handler in
# this file already does it - the audit that produced this checker found exactly
# one violation among 49 cases.
#
# WHAT THIS DOES NOT PROVE: that the check uses the RIGHT constant. A handler that
# tests `out_len < 8` and then writes six qwords passes this. That is covered by
# ioctl_buffer_size.ps1 on the client side and by reading the constant on the
# driver side; this check's job is narrower and honest - "there is a guard at all".

$repoRoot = Split-Path -Parent $PSScriptRoot
$dev = Join-Path $repoRoot 'hv\control_device.c'

if (-not (Test-Path $dev)) { throw "ioctl_output_bounds: cannot find $dev" }

$lines = Get-Content $dev
$cases = @()
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^\s+case\s+IOCTL_YGHV_([A-Z0-9_]+)') {
        $cases += [pscustomobject]@{ Name = $matches[1]; Line = $i }
    }
}

if ($cases.Count -eq 0) {
    throw 'ioctl_output_bounds: found no case IOCTL_YGHV_* - the parser is wrong, not the code'
}

$findings = @()
$checked = 0

for ($k = 0; $k -lt $cases.Count; $k++) {
    $end = if ($k + 1 -lt $cases.Count) { $cases[$k + 1].Line } else { $lines.Count }
    # A case may fall through into the next one (several handlers share a body);
    # judging the group from its FIRST label would read a body that belongs to a
    # later label. So the body is taken from THIS case to the next DIFFERENT
    # case - which is what "case X: case Y: body" already is; the body text simply
    # appears under both. Reading to `$end` is therefore correct for the LAST label
    # of a group and harmless for the earlier ones (they see the same body).
    $body = ($lines[$cases[$k].Line..([Math]::Max($cases[$k].Line, $end - 1))] -join "`n")

    $writes = ($body -match '\bout\s*\[\s*\d|\*out\s*=|\bout\s*->|(?<![a-zA-Z_])(buf)\s*\[')
    if (-not $writes) { continue }
    $checked += 1

    <#
      9.488: STRIP COMMENTS BEFORE LOOKING FOR THE out_len TEST.

      Measured failure of the first version: it injected the SECTION_HIDE defect
      back and reported PASS, because the case body contains an explaining comment
      that mentions `out_len` - "the check exists" was satisfied by the PROSE
      saying a check should exist. A guard is an `if`, not a sentence.

      Block comments are removed wholesale, then line comments from `//` onward.
    #>
    $code = $body -replace '(?s)/\*.*?\*/', ''
    $code = ($code -split "`n" | ForEach-Object { ($_ -replace '//.*$', '') }) -join "`n"

    if ($code -notmatch 'out_len') {
        $findings += ('{0} (line {1}) writes the reply buffer but has no out_len test in code (comments do not count)' -f
                      $cases[$k].Name, ($cases[$k].Line + 1))
    }
}

Write-Host "  $checked handler(s) write the reply buffer; $($cases.Count) IOCTL case(s) total"

if ($findings.Count -gt 0) {
    foreach ($f in $findings) { Write-Host "[FAIL] $f" }
    throw "ioctl_output_bounds: $($findings.Count) unbounded reply write(s)"
}

Write-Host '[PASS] IOCTL output bounds: every handler that writes the buffer tests out_len'
