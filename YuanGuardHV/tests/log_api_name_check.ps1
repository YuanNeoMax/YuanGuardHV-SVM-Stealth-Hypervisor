$ErrorActionPreference = 'Stop'

# log_api_name_check.ps1 -- 9.608
#
# WHY THIS EXISTS
#
# LOG_INFO / LOG_ERROR / LOG_WARN compile to ((void)0) unless YGHV_DEBUG_LOG is
# defined. The STRING LITERAL, however, is still emitted into .rdata -- dead,
# unreferenced, and perfectly readable by "strings driver.sys | grep Ps".
#
# Measured this session: "obguard: ObRegisterCallbacks failed 0x%x" put that API
# name back into an image whose entire point is that no capability name survives.
# The string_encryption_check found it. Six more were found by the same search,
# all in dead log literals:
#
#   IoCreateDevice, IoCreateSymbolicLink, IoDeleteSymbolicLink,
#   IoAllocateWorkItem, ZwTerminateProcess, ZwOpenProcess
#
# A name written into a literal cannot be hidden by yghv_strtab, so it must not
# be written there. The message can say what happened without naming the API.
#
# This is a SOURCE check, not an image check: it runs before compiling and names
# the file and line, so the fix is obvious without reading a hex dump.

$root = Split-Path -Parent $PSScriptRoot
$hv = Join-Path $root 'hv'

$apiPattern = '\b(Ps|Ob|Mm|Ke|Zw|Io|Rtl|Ex|Cm|Se|Hal)[A-Z][A-Za-z0-9]+'
$logPattern = 'LOG_(?:INFO|ERROR|WARN)\(\s*"([^"]*)"'

$problems = New-Object 'System.Collections.Generic.List[string]'
$scanned = 0

$files = @(Get-ChildItem -LiteralPath $hv -Filter '*.c' -File) +
         @(Get-ChildItem -LiteralPath $hv -Filter '*.cpp' -File)

foreach ($f in $files) {
    $lines = [IO.File]::ReadAllLines($f.FullName)
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        $m = [regex]::Match($line, $logPattern)
        if (-not $m.Success) { continue }
        $scanned++
        $lit = $m.Groups[1].Value
        $hit = [regex]::Match($lit, $apiPattern)
        if ($hit.Success) {
            $problems.Add(("{0}:{1} log literal names the API: {2}" -f $f.Name, ($i + 1), $hit.Value))
        }
    }
}

# ---------------------------------------------------------------- self-test
# A checker that has never failed is a checker nobody can trust.
$fixture = Join-Path ([IO.Path]::GetTempPath()) ("logapi_" + [guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $fixture -Force | Out-Null
$bad = Join-Path $fixture 'bad.c'
$good = Join-Path $fixture 'good.c'
[IO.File]::WriteAllText($bad,  'LOG_ERROR("x: PsLookupProcessByProcessId failed 0x%x", st);' + [char]10)
[IO.File]::WriteAllText($good, 'LOG_ERROR("x: lookup failed 0x%x", st);' + [char]10)

function Test-Fixture([string]$path) {
    $lines = [IO.File]::ReadAllLines($path)
    foreach ($line in $lines) {
        $m = [regex]::Match($line, $logPattern)
        if (-not $m.Success) { continue }
        if ([regex]::Match($m.Groups[1].Value, $apiPattern).Success) { return $true }
    }
    return $false
}

$selfFailures = New-Object 'System.Collections.Generic.List[string]'
if (-not (Test-Fixture $bad)) { $selfFailures.Add('the planted literal was NOT caught') }
else { Write-Host '[PASS] self-test: a planted API name in a log literal was caught' }
if (Test-Fixture $good) { $selfFailures.Add('a clean literal was reported as a problem') }
else { Write-Host '[PASS] self-test: a clean log literal stays silent' }
Remove-Item $fixture -Recurse -Force -ErrorAction SilentlyContinue

if ($selfFailures.Count -gt 0) {
    foreach ($s in $selfFailures) { Write-Host "[FAIL] $s" }
    throw 'log_api_name_check: self-test failed'
}

if ($problems.Count -gt 0) {
    foreach ($p in $problems) { Write-Host "[FAIL] $p" }
    throw ("log_api_name: {0} dead log literal(s) name an API -- the literal is emitted into .rdata even though LOG_* compiles to nothing" -f $problems.Count)
}

Write-Host ("[PASS] log literals: {0} LOG_* format string(s) scanned, none names an API" -f $scanned)