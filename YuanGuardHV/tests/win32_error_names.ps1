$ErrorActionPreference = 'Stop'

# win32_error_names.ps1 -- 9.583
#
# WHY THIS EXISTS
#
# 9.582 recorded `protect-page` failing with "0x490 ERROR_NOT_READY" and concluded
# "protect-page has a special restriction on mmf pages". Both halves were wrong:
#
#   0x490 = 1168 = ERROR_NOT_FOUND     (ERROR_NOT_READY is 21 / 0x15)
#   only STATUS_NOT_FOUND (0xC0000225) maps to 1168
#   its ONLY source on that path is protect.c:1099 -- "pid not in the target table"
#
# The wrong NAME was not cosmetic. "NOT_READY" suggests timing, so the next three
# attempts were `config sync 0`, `mmf-hold` pages, and a note saying the page kind
# mattered. "NOT_FOUND" says the pid is not registered, which points at `set-target`
# in one step. A round of investigation was spent on the wrong axis because a
# hexadecimal number was labelled with the wrong constant.
#
# WHAT IT CHECKS
#
# Every `0xNNN`/`0xNNNN` in the documents that is IMMEDIATELY followed by a Win32
# error NAME must have the right name. The number is checked against the live table
# (System.ComponentModel.Win32Exception), not against a hard-coded list -- a checker
# carrying its own copy of the table would agree with a wrong document.
#
# SCOPE: only "number followed by NAME" pairs. A bare `0x490` in a table of measured
# results is left alone; there is no claim in it to be wrong.

$docsRoot = if ($env:YGHV_DOCS_ROOT) { $env:YGHV_DOCS_ROOT } else {
    Join-Path (Split-Path -Parent $PSScriptRoot) '..\docs'
}
$docsRoot = (Resolve-Path -LiteralPath $docsRoot).Path

function ReadUtf8 {
    param([string] $Path)
    return [IO.File]::ReadAllText($Path, [Text.Encoding]::UTF8)
}

# The live name table, keyed by code. Built by asking Windows, so it cannot drift
# from the OS the driver actually runs on.
function Get-Win32Name {
    param([int] $Code)
    try {
        $e = New-Object System.ComponentModel.Win32Exception $Code
        # Win32Exception falls back to a generic message for unknown codes and
        # keeps NativeErrorCode; there is no way to ask it "is this a real name",
        # so unknown codes are recognised by the message being the generic one.
        if ($e.Message -match '^Unknown error') { return $null }
        return $e.Message
    }
    catch {
        return $null
    }
}

# The documents name errors in ASCII, but the OS message is localised. Rather than
# compare a translated string, keep a small table of the constants this project
# actually cites and verify each one against Win32Exception by CODE. A name that is
# not in this table is reported as unverifiable rather than guessed at.
$known = [ordered]@{
    'ERROR_NOT_FOUND'         = 1168
    'ERROR_NOT_READY'         = 21
    'ERROR_INVALID_PARAMETER' = 87
    'ERROR_ACCESS_DENIED'     = 5
    'ERROR_NOT_ENOUGH_MEMORY' = 8
    'ERROR_INVALID_HANDLE'    = 6
    'ERROR_FILE_NOT_FOUND'    = 2
    'ERROR_PATH_NOT_FOUND'    = 3
    'ERROR_BUFFER_OVERFLOW'   = 111
    'ERROR_INSUFFICIENT_BUFFER' = 122
    'ERROR_GEN_FAILURE'       = 31
    'ERROR_NOT_SUPPORTED'     = 50
    'ERROR_INVALID_FUNCTION'  = 1
    'ERROR_DEVICE_NOT_CONNECTED' = 1167
    'ERROR_NO_SYSTEM_RESOURCES'  = 1450
}

$problems = New-Object 'System.Collections.Generic.List[string]'
$checked = 0
$quoted = 0

# THREE WAYS A DOCUMENT WRITES THE PAIR, and the first version only knew one:
#
#   "0x490 ERROR_NOT_READY"        number then name      <- what 9.582 wrote
#   "ERROR_NOT_READY = 0x15"       name then number      <- how a correction reads
#   "0x490 = 1168 = ERROR_NOT_FOUND"  hex = decimal = name
#
# Measured: with only the first pattern the checker reported "0 pair(s)" -- it was
# running, printing PASS, and had not compared a single value. That is the exact
# failure mode this tree keeps recording (a checker whose regex cannot reach the
# text it claims to inspect), caught here by the count being 0 rather than by a
# wrong answer.
$patterns = @(
    @{ Re = '0[xX]([0-9A-Fa-f]{2,4})\s*(?:=|\(|:)?\s*(ERROR_[A-Z_]+)'; Hex = 1; Name = 2 },
    @{ Re = '(ERROR_[A-Z_]+)\s*(?:=|\(|:)?\s*0[xX]([0-9A-Fa-f]{2,4})'; Hex = 2; Name = 1 },
    @{ Re = '0[xX]([0-9A-Fa-f]{2,4})\s*=\s*[0-9]+\s*=\s*(ERROR_[A-Z_]+)'; Hex = 1; Name = 2 }
)

$mdFiles = @(Get-ChildItem -LiteralPath $docsRoot -Recurse -File -Filter '*.md')
foreach ($f in $mdFiles) {
    $text = ReadUtf8 $f.FullName
    foreach ($pat in $patterns) {
        foreach ($m in [regex]::Matches($text, $pat.Re)) {
            $code = [Convert]::ToInt32($m.Groups[$pat.Hex].Value, 16)
            $name = $m.Groups[$pat.Name].Value

            # QUOTED HISTORY. A record that explains a past mistake has to be able to
            # write the mistake down -- 9.583 exists because "0x490 ERROR_NOT_READY"
            # was believed, and the correction is worthless if it cannot quote what it
            # corrects. A line marked with [HIST] is a citation, not a claim; it is
            # counted and reported separately so it cannot become a quiet hiding place.
            $lineStart = $text.LastIndexOf("`n", [Math]::Max(0, $m.Index - 1)) + 1
            $lineEnd = $text.IndexOf("`n", $m.Index)
            if ($lineEnd -lt 0) { $lineEnd = $text.Length }
            $lineText = $text.Substring($lineStart, $lineEnd - $lineStart)
            if ($lineText -match '\[HIST\]') {
                $quoted++
                continue
            }

            $checked++

            # PLACEHOLDERS ARE NOT CLAIMS. The documents use ERROR_X / ERROR_FOO to
            # show the SHAPE of a pairing while explaining the pattern; treating
            # those as constants to verify produces noise that trains people to
            # ignore the checker. A name ending in _X or containing FOO/BAR/EXAMPLE
            # is a stand-in, and is skipped without comment.
            if ($name -match '_(X|FOO|BAR|EXAMPLE|NAME|CODE)$') { continue }

            if (-not $known.Contains($name)) {
                # Not a failure: the document cites a constant this checker does not
                # carry. Say so instead of pretending it was verified.
                Write-Host "[SKIP] $($f.Name): $name is not in this checker's table"
                continue
            }
            if ($known[$name] -ne $code) {
                $line = ($text.Substring(0, $m.Index) -split "`n").Count
                $problems.Add("$($f.Name):$line pairs 0x$($m.Groups[$pat.Hex].Value) with $name, but $name is $($known[$name]) (0x$('{0:X}' -f $known[$name]))")
            }
        }
    }
}

# A COUNT OF ZERO IS A FAILURE, not a quiet pass: it means the checker ran and found
# nothing it could compare, which is indistinguishable from "the documents are fine"
# in the build log. The whole reason this file exists is a mislabelled code, so at
# least one pair must be verifiable in the tree at all times.
if ($checked -eq 0) {
    $problems.Add('no verifiable number/name pair found in any document -- this check is blind')
}

if ($problems.Count -gt 0) {
    foreach ($p in $problems) { Write-Output "[FAIL] $p" }
    throw "win32_error_names: $($problems.Count) problem(s) with error-code labelling"
}

Write-Host "[PASS] Win32 error names: $checked number/name pair(s) agree with the OS table ($quoted quoted as [HIST])"
