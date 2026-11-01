$ErrorActionPreference = 'Stop'

# client_undefined_vars.ps1 — 9.491
#
# POWERSHELL DOES NOT COMPLAIN ABOUT AN UNDEFINED VARIABLE - IT MAKES IT $null.
#
# MEASURED INSTANCE: `mem-bench`'s INVALID test read `$brid` where the variable is
# `$brd`. `$brid` appears exactly once in the whole file, is assigned nowhere, and
# evaluates to $null. So
#
#     if ($pv -eq 0 -or $brid -eq 0)      # '$null -eq 0' is FALSE
#
# silently became "if B visited no pages" - the second half of the test was dead.
# A run where the walk resolved but every page missed printed no warning at all,
# and the meaningless ratio above it was quoted as a speedup.
#
# This is the same failure family as the rest of this review: a guard that looks
# present, is syntactically valid, passes every other checker, and does nothing.
#
# THE RULE: every `$name` read in the client must be assigned somewhere in the
# same file, OR be one of the documented ambient names below. A typo'd name is then
# a build failure instead of a silently-dead branch.
#
# WHAT THIS CANNOT SEE: scope-shadowing bugs (a variable assigned inside one
# `if` block and read outside it is assigned SOMEWHERE, so this passes) and
# automatic variables that arrive from the host. The ambient list covers the
# latter; the former needs reading, and that is why this is a net, not a proof.

$repoRoot = Split-Path -Parent $PSScriptRoot
$ps1 = Join-Path $repoRoot 'tools\yghv_ctl.ps1'

if (-not (Test-Path $ps1)) { throw "client_undefined_vars: cannot find $ps1" }

$text = Get-Content $ps1 -Raw

# Names provided by PowerShell itself, by the host, or by the script's param block.
$ambient = @(
    # automatic / host
    '_', 'args', 'input', 'this', 'PSCmdlet', 'PSItem', 'PSCommandPath',
    'PSScriptRoot', 'PSVersionTable', 'Error', 'LASTEXITCODE', 'Matches',
    'true', 'false', 'null', 'env', 'Host', 'PID', 'PSHome', 'PSEdition',
    'MyInvocation', 'ExecutionContext', 'StackTrace', 'foreach', 'switch',
    'HOME', 'PROFILE', 'PSSession', 'ShellId', 'ErrorActionPreference',
    # this script's own parameters / helpers
    'Command', 'Arg1', 'Arg2', 'Arg3', 'Arg4',
    # variables produced by -f / -match operators the regex below cannot attribute
    'matches'
)

# Every assignment form: `$x =`, `$x +=`, `$x++`, `foreach ($x in`, `param($x`,
# `function f($x`, `$x,` inside a multi-assignment, and `[type]$x =`.
$assigned = @{}
foreach ($m in [regex]::Matches($text, '(?m)\$([A-Za-z_][A-Za-z0-9_]*)\s*(?:=[^=]|[-+*/%]=|\+\+|--)')) {
    $assigned[$m.Groups[1].Value] = $true
}
foreach ($m in [regex]::Matches($text, '(?m)foreach\s*\(\s*\$([A-Za-z_][A-Za-z0-9_]*)\s+in')) {
    $assigned[$m.Groups[1].Value] = $true
}
foreach ($m in [regex]::Matches($text, '(?m)function\s+[A-Za-z0-9_-]+\s*\(\s*\$\s*([A-Za-z_][A-Za-z0-9_]*)')) {
    $assigned[$m.Groups[1].Value] = $true
}
# 9.491: `param()` declares EVERY parameter, not just the first - and the first
# version of this parser only captured one, so `param([int]$Type, [string]$Value)`
# left $Value looking undefined. A checker that reports a correct tree is worse
# than none, so this walks every `[type]$name` inside each param block.
foreach ($m in [regex]::Matches($text, '(?s)param\s*\(\s*\r?\n?([\s\S]*?)\r?\n?\s*\)')) {
    foreach ($p in [regex]::Matches($m.Groups[1].Value, '\[[^\]]+\]\s*\$([A-Za-z_][A-Za-z0-9_]*)')) {
        $assigned[$p.Groups[1].Value] = $true
    }
}

if ($assigned.Keys.Count -eq 0) {
    throw 'client_undefined_vars: found no assignments at all - the parser is wrong, not the code'
}

# Every READ of $name, excluding the assignment sites themselves, strings, and
# comments. Stripping strings/comments matters: a `$name` inside a message would
# otherwise look like a read and produce noise.
$stripped = $text -replace '(?s)<#.*?#>', ''
$stripped = ($stripped -split "`n" | ForEach-Object { ($_ -replace '#.*$', '') }) -join "`n"
$stripped = $stripped -replace '(?s)"[^"]*"', '""' -replace "(?s)'(?:[^']|'')*'", "''"

$findings = @()
$reads = 0
foreach ($m in [regex]::Matches($stripped, '\$([A-Za-z_][A-Za-z0-9_]*)')) {
    $name = $m.Groups[1].Value
    if ($ambient -contains $name) { continue }
    $reads += 1
    if (-not $assigned.ContainsKey($name)) {
        $line = ($stripped.Substring(0, $m.Index) -split "`n").Count
        $findings += ('line {0}: `${1}` is read but assigned nowhere - PowerShell makes it $null and the branch goes dead' -f
                      $line, $name)
    }
}

# 9.491: `$assigned.Count` RETURNED A BOOLEAN, NOT A NUMBER - AND THE REASON IS
# A REAL HAZARD THIS CHECKER ITSELF CREATED.
#
# The scan records every assigned name as a KEY in $assigned. The client contains
# text like `$out.Count = ...`? No - it contains `$x.Count` READS, but the key that
# broke this is the literal name "Count": one of the file's variables is itself
# called ... nothing, because the match came from a property access. Either way,
# once a key named `Count` exists in a Hashtable, `$assigned.Count` returns THAT
# VALUE (the $true stored under key "Count") instead of the hashtable's element
# count - PowerShell resolves the key first for Hashtable.
#
# Consequence: the line printed "True assigned name(s)" instead of a number, and
# the emptiness guard `if ($assigned.Count -eq 0)` was comparing $true -eq 0 =
# $false, i.e. it never fired. Both are cosmetic-and-harmless HERE, but the same
# mistake in a real branch is the exact class this checker exists to catch.
#
# Fix: count through the dictionary interface, which cannot be shadowed by a key.
$nAssigned = $assigned.Keys.Count
Write-Host ("  {0} assigned name(s); {1} name read(s) checked" -f $nAssigned, $reads)

if ($findings.Count -gt 0) {
    # One finding per distinct name: a typo repeated five times is one bug.
    $seen = @{}
    $unique = @()
    foreach ($f in $findings) {
        $key = ($f -split '`')[1]
        if (-not $seen.ContainsKey($key)) { $seen[$key] = $true; $unique += $f }
    }
    foreach ($f in $unique) { Write-Host "[FAIL] $f" }
    throw "client_undefined_vars: $($unique.Count) undefined variable(s)"
}

Write-Host '[PASS] client variables: every read name is assigned somewhere in the file'
