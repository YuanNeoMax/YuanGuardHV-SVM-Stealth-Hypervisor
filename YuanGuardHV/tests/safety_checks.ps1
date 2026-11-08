$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$protect = Join-Path $root 'hv\protect.c'
$readme = Join-Path $root 'tools\yghv_client\README.md'

function Get-EnclosingFunction {
    param([string[]]$Lines, [int]$Index)
    for ($i = $Index; $i -ge 0; $i--) {
        $line = $Lines[$i].Trim()
        if ($line -match '^(if|for|while|switch|else|do)\b') {
            continue
        }
        if ($line -match '^[A-Za-z_][A-Za-z0-9_ \*]*\([^;]*\)\s*\{\s*$') {
            return $line
        }
    }
    return ''
}

$lines = Get-Content -LiteralPath $protect
$allowed = @('yghv_protect_install_hook_locked', 'yghv_protect_remove_hook_locked')
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -notmatch 'svm_core_pause_residents_for_patch\(\)') {
        continue
    }
    $funcLine = Get-EnclosingFunction -Lines $lines -Index $i
    if ($funcLine -notmatch '([A-Za-z_][A-Za-z0-9_]*)\s*\(') {
        throw "Safety check FAIL: cannot determine enclosing function for pause at line $($i + 1)"
    }
    $name = $Matches[1]
    if ($allowed -notcontains $name) {
        throw "Safety check FAIL: pause-under-lock in unexpected function '$name' at line $($i + 1)"
    }
}

$protectText = Get-Content -LiteralPath $protect -Raw
if ($protectText -match 'STATUS_DEVICE_BUSY') {
    throw 'Safety check FAIL: protect.c must not return STATUS_DEVICE_BUSY'
}

# 9.564: the 9.561 docs reorganisation moved three of these into docs\archive\,
# and this checker then died on `Get-Content` instead of checking anything.
# A path is now resolved, not assumed: try docs\ then docs\archive\, and treat
# "not found anywhere" as a FAILURE. Guarding it with `-ErrorAction SilentlyContinue`
# would have turned this into the tree's oldest defect shape - a check that reads
# nothing and reports PASS (9.500: four commands that never worked because no
# checker looked at that part of the file).
$checkFiles = @(
    'docs\YUANMOD_NEXT_WINDOW_PROMPT.md',
    'docs\TASKS.md',
    'docs\YGHV_HOOK_LOCK_AND_0x5AA_REDESIGN_20260813.md',
    'docs\YUANMOD_HANDOFF_CURRENT.md',
    'tools\yghv_client\README.md'
)
foreach ($rel in $checkFiles) {
    $file = $null
    foreach ($prefix in @($root, (Join-Path $root '..'))) {
        $candidate = Join-Path $prefix $rel
        if (Test-Path -LiteralPath $candidate) { $file = $candidate; break }
        $archived = Join-Path $prefix ($rel -replace '^docs\\', 'docs\archive\')
        if (Test-Path -LiteralPath $archived) { $file = $archived; break }
    }
    if (-not $file) {
        throw "Safety check FAIL: none of the documented paths exists for '$rel'"
    }
    $text = Get-Content -LiteralPath $file -Raw
    if ($text -match 'ERROR_BUSY\s*\(0x5AA\)') {
        throw "Safety check FAIL: '$file' still labels 0x5AA as ERROR_BUSY"
    }
}

$readmeText = Get-Content -LiteralPath $readme -Raw
if ($readmeText -notmatch '(?m)^run\.bat unprotect\s*$') {
    throw 'Safety check FAIL: README must document run.bat unprotect'
}
if ($readmeText -notmatch '(?m)^run\.bat scan\s+<pid>') {
    throw 'Safety check FAIL: README must document run.bat scan <pid>'
}

Write-Host '[PASS] Safety checks: pause-under-lock scope, STATUS_DEVICE_BUSY, 0x5AA naming, README unprotect/scan'
