$ErrorActionPreference = 'Stop'

# count_as_extent.ps1 — 9.486
#
# A TABLE THAT IS NOT COMPACTED MUST NOT BE SCANNED UP TO ITS LIVE COUNT.
#
# Five tables in this tree share one shape:
#
#     int n = (int)InterlockedCompareExchange(&g_xxx_count, 0, 0);
#     for (i = 0; i < n && i < XXX_MAX; i++)
#         if (InterlockedCompareExchange(&g_xxx[i].active, 0, 0)) { ... }
#
# The counter is incremented on install and decremented on remove, but the array
# is never compacted. Removing a slot in the middle therefore leaves
# `g_xxx[i].active == 0` with a LIVE slot above it, and `i < n` stops before that
# live slot. Measured consequence in the breakpoint table (9.485): arm three, remove
# the middle one, and the third stops firing - while every counter and every list
# still agrees. A table that silently stops reporting one of its entries is worse
# than one that errors, and this project's whole purpose is answering "is this
# page/breakpoint still doing what I asked".
#
# THE RULE: a loop whose bound is a LIVE COUNT must either
#   (a) not exist - scan the full array and test `active` per slot, or
#   (b) be proven safe by compacting the table on removal.
#
# (b) is real: yghv_protect_remove_page_for_locked does
#         t->pages[i] = t->pages[t->page_count - 1];
#         t->page_count--;
# so protect.c's count IS an extent and is allowed.
#
# So the check reports every `for (... < n ...)` whose `n` came from a
# InterlockedCompareExchange of a *_count, UNLESS the same file compacts that
# table on removal. "Compacts" = contains an assignment of the form
# `<arr>[i] = <arr>[<cnt> - 1]`.
#
# Explicitly allowed today, and why:
#   hv/mem_bp.c     now scans full array (9.485 fixed it)
#   hv/mem_cache.c  fixed in 9.486
#   hv/mem_time.c   fixed in 9.486
#   hv/mem_view.c   fixed in 9.486
#   hv/mem_watch.c  fixed in 9.486
#   hv/protect.c    compacts on removal - count is a true extent

$repoRoot = Split-Path -Parent $PSScriptRoot
$hvDir = Join-Path $repoRoot 'hv'

if (-not (Test-Path $hvDir)) { throw "count_as_extent: cannot find $hvDir" }

$findings = @()
$scanned = 0
$allowed = 0

foreach ($file in Get-ChildItem -Path $hvDir -Filter *.c) {
    $lines = Get-Content $file.FullName
    $text = $lines -join "`n"

    # Does this file compact any table on removal? If so its counts are extents.
    $compacts = $text -match '\[\s*\w+\s*\]\s*=\s*\w+\[\s*\w+\s*->?\s*\w*count\s*-\s*1\s*\]'

    for ($i = 0; $i -lt $lines.Count; $i++) {
        # `int n = (int)InterlockedCompareExchange(&SOME_COUNT, 0, 0);`
        if ($lines[$i] -notmatch 'int\s+n\s*=\s*\(int\)InterlockedCompareExchange\(&(g_[A-Za-z0-9_]+),') {
            continue
        }
        $counter = $matches[1]

        # Look ahead a few lines for the loop that uses this n.
        $loopLine = -1
        for ($j = $i + 1; $j -lt [Math]::Min($i + 6, $lines.Count); $j++) {
            if ($lines[$j] -match 'for\s*\(\s*\w+\s*=\s*0\s*;\s*\w+\s*<\s*n\s*') { $loopLine = $j; break }
        }
        if ($loopLine -lt 0) { continue }

        $scanned += 1

        if ($compacts) { $allowed += 1; continue }

        # 9.486: this was one long `-f` format string concatenated across two lines.
        # `"a" + "b" -f args` binds `-f` to the SECOND string only, so the first half
        # printed its placeholders literally: measured output was
        #   `[FAIL] {0}:{1} scans i < n where n = live {2}, ...`
        # which names no file and no line - i.e. a finding you cannot act on.
        # Build the message in one piece.
        $findings += ('{0}:{1} scans i<n where n = live {2}, and this file does not ' -f
                      $file.Name, ($loopLine + 1), $counter) +
                     ' compact its table on removal - entries above a hole are unreachable'
    }
}

Write-Host "  $scanned live-count-bounded loop(s) in hv\*.c; $allowed allowed by compaction"

if ($scanned -eq 0) {
    # Not an error by itself, but it means the parser matched nothing - which is
    # the failure mode that makes a checker silently useless.
    Write-Host '[WARN] matched no live-count-bounded loop at all - parser may be stale'
}
elseif ($findings.Count -gt 0) {
    foreach ($f in $findings) { Write-Host "[FAIL] $f" }
    throw "count_as_extent: $($findings.Count) un-compacted table scan(s)"
}

Write-Host '[PASS] live-count scans: no un-compacted table is bounded by its counter'
