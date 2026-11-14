$hv = @(Get-ChildItem -LiteralPath 'D:/yuanguard/YuanGuardHV/hv' -File -Filter '*' | Where-Object { $_.Name -notmatch '^run_' }).Count
$cm = @(Get-ChildItem -LiteralPath 'D:/yuanguard/YuanGuardHV/hv/common' -File -Filter '*.h').Count
$ts = @(Get-ChildItem -LiteralPath 'D:/yuanguard/YuanGuardHV/tests' -File -Filter '*.ps1' | Where-Object { $_.Name -notmatch '^run_' }).Count
$cmds = ([regex]::Matches((Get-Content -Raw 'D:/yuanguard/YuanGuardHV/tools/yghv_ctl.ps1'), '(?m)^\s*''([a-z0-9-]+)''\s*\{\s*$')).Count
Write-Output "hv=$hv common=$cm tests=$ts ps_cmds=$cmds"
