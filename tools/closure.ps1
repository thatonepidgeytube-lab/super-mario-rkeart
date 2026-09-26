# closure.ps1 -- iteratively grow the disassembly entry set until it stops growing.
#
# inputs:
#   build/entries.txt  -- starting code addresses, one hex addr per line
#   build/tables.txt   -- dispatch tables, lines of "BB:AAAA,N"
# output:
#   build/closure.asm  -- the listing
#   build/entries.txt  -- grown entry set (rewritten each iteration)
param(
  [string]$Rom   = "C:\smk recomp\Super Mario Kart (USA).sfc",
  [string]$Bin   = "C:\smk recomp\bin\smk-disasm.exe",
  [string]$Work  = "C:\smk recomp\build",
  [int]$MaxIter = 8,
  [int]$Limit = 4000000
)
$entries = Join-Path $Work "entries.txt"
$tables  = Join-Path $Work "tables.txt"
$listing = Join-Path $Work "closure.asm"

$tableArgs = @()
if (Test-Path $tables) {
  Get-Content $tables | ForEach-Object {
    $t = $_.Trim()
    if ($t -and -not $t.StartsWith('#')) { $tableArgs += @('--table', $t) }
  }
}

$known = New-Object 'System.Collections.Generic.HashSet[string]'
for ($it = 1; $it -le $MaxIter; $it++) {
  Get-Content $entries | ForEach-Object { $t = $_.Trim(); if ($t) { [void]$known.Add($t) } }
  $args = @($Rom, "--entries-file", $entries, "--limit", "$Limit") + $tableArgs + @("-o", $listing)
  & $Bin @args
  $before = $known.Count
  Select-String -Path $listing -Pattern 'loc_([0-9A-F]{6})' -AllMatches | ForEach-Object { $_.Matches } | ForEach-Object { [void]$known.Add($_.Groups[1].Value) }
  $new = $known.Count - $before
  Write-Output "iter $it : entries=$before -> $($known.Count) (+$new)"
  if ($new -eq 0) { break }
  $known | Sort-Object | Set-Content -Path $entries -Encoding ascii
}
$known | Sort-Object | Set-Content -Path $entries -Encoding ascii
Write-Output "final entry count: $($known.Count)"
