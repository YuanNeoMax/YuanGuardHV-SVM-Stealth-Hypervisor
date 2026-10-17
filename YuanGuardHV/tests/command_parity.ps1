$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$psClient = Join-Path $root 'tools\yghv_ctl.ps1'
$javaClient = Join-Path $root 'tools\yghv_client\YghvCtl.java'
$readme = Join-Path $root 'tools\yghv_client\README.md'

# 9.336: read with [IO.File]::ReadAllText, not Get-Content -Raw.
# PowerShell 5.1's Get-Content -Raw handled this file such that
# '(?m)^yghv_ctl.ps1' missed one entry (measured: the same file and the same
# regex yielded 51 command names under powershell.exe and 52 under pwsh).
# Reading the bytes directly removes the difference.

function Get-PSCommands {
    $text = [IO.File]::ReadAllText($psClient)
    $set = @{}
    foreach ($m in [regex]::Matches($text, '(?m)^\s*''([a-z0-9-]+)''\s*\{\s*$')) {
        $set[$m.Groups[1].Value.ToLower()] = $true
    }
    return @($set.Keys | Sort-Object)
}

function Get-JavaCommands {
    $text = [IO.File]::ReadAllText($javaClient)
    $set = @{}
    foreach ($m in [regex]::Matches($text, 'case "([a-z0-9-]+)"')) {
        $set[$m.Groups[1].Value.ToLower()] = $true
    }
    return @($set.Keys | Sort-Object)
}

function Get-ReadmeJavaCommands {
    $text = [IO.File]::ReadAllText($readme)
    $set = @{}
    foreach ($m in [regex]::Matches($text, '(?m)^run\.bat\s+([a-z0-9-]+)')) {
        $set[$m.Groups[1].Value.ToLower()] = $true
    }
    return @($set.Keys | Sort-Object)
}

function Get-ReadmePSCommands {
    $text = [IO.File]::ReadAllText($readme)
    $set = @{}
    foreach ($m in [regex]::Matches($text, '(?m)^yghv_ctl\.ps1[ \t]+([a-z0-9-]+)')) {
        $set[$m.Groups[1].Value.ToLower()] = $true
    }
    return @($set.Keys | Sort-Object)
}

$ps = Get-PSCommands
$java = Get-JavaCommands
$readmeJava = Get-ReadmeJavaCommands
$readmePS = Get-ReadmePSCommands

$dJava = Compare-Object @($java | Sort-Object) @($readmeJava | Sort-Object)
if ($dJava) {
    throw "Command parity FAIL: Java client vs README mismatch`nJava: $($java -join ',')`nREADME: $($readmeJava -join ',')"
}
$dPS = Compare-Object @($ps | Sort-Object) @($readmePS | Sort-Object)
if ($dPS) {
    throw "Command parity FAIL: PowerShell client vs README mismatch`nPS: $($ps -join ',')`nREADME: $($readmePS -join ',')"
}

Write-Host "[PASS] Command parity: PowerShell=$($ps.Count) Java=$($java.Count)"
