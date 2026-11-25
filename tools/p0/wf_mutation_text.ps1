# Mutation anchors ignore layout outside quoted C literals, so running
# clang-format over the sources cannot silently break (or, worse, shift) a mutant.
# Rules, chosen so the pattern can never match text the exact anchor would not:
#   - whitespace between two identifier characters stays mandatory (`return 0`
#     never matches `return0`);
#   - two identifier characters that touch in the anchor must touch in the source;
#   - around punctuation any amount of whitespace (including line breaks) is allowed;
#   - an anchor that starts or ends with an identifier character may not be
#     embedded in a longer identifier.
# Every caller still asserts the exact number of matches it expects.
function Get-WfAnchorPattern([string]$Text) {
  $word='[A-Za-z0-9_]'
  $text=$Text.Trim()
  if(-not $text.Length){throw 'Empty mutation anchor'}
  $pattern=New-Object System.Text.StringBuilder
  $prev=$null
  $gap=$false
  $quote=$null
  $escaped=$false
  foreach($ch in $text.ToCharArray()){
    if($null -ne $quote){
      [void]$pattern.Append([regex]::Escape([string]$ch))
      if($escaped){$escaped=$false}
      elseif($ch -eq '\'){$escaped=$true}
      elseif([string]$ch -eq $quote){$quote=$null}
      $prev=[string]$ch
      $gap=$false
      continue
    }
    if([char]::IsWhiteSpace($ch)){ $gap=$true; continue }
    if($null -ne $prev){
      $prevWord=$prev -match $word
      $curWord=([string]$ch) -match $word
      if($prevWord -and $curWord){
        if($gap){[void]$pattern.Append('\s+')}
      } else {
        [void]$pattern.Append('\s*')
      }
    }
    [void]$pattern.Append([regex]::Escape([string]$ch))
    if($ch -eq '"' -or $ch -eq "'"){$quote=[string]$ch}
    $prev=[string]$ch
    $gap=$false
  }
  $first=$text.Substring(0,1); $last=$text.Substring($text.Length-1)
  $head=if($first -match $word){'(?<!'+$word+')'}else{''}
  $tail=if($last -match $word){'(?!'+$word+')'}else{''}
  $head+$pattern.ToString()+$tail
}
function Measure-WfAnchor([string]$Source,[string]$Old) {
  [regex]::Matches($Source,(Get-WfAnchorPattern $Old)).Count
}
function Edit-WfAnchor([string]$Source,[string]$Old,[string]$New) {
  [regex]::Replace($Source,(Get-WfAnchorPattern $Old),$New.Replace('$','$$'))
}

# A mutant is KILLED only when the test failed with exit status 1 AND printed
# an assertion line beginning with FAIL: a crash or a hang is not a
# kill. The test's stdout must already be in <Directory>\test.log.
function Test-WfMutant([string]$Name,[string]$Directory,[int]$ExitCode) {
  $failLine=Select-String -LiteralPath (Join-Path $Directory 'test.log') -Pattern '^FAIL ' -CaseSensitive -Quiet
  $failed=$ExitCode -eq 1
  $result=[pscustomobject]@{name=$Name;result=$(if($failed -and $failLine){'KILLED'}else{'SURVIVED'});exit_code=$ExitCode;directory=$Directory}
  Write-Host "${Name}: $($result.result)"
  $result
}

# Writes mutations.json, optionally copies the per-mutant logs to a new evidence
# directory, and returns the script's exit status (0 only if every mutant died).
function Complete-WfMutations($Results,[string]$Root,[string]$Evidence) {
  if(-not @($Results | Where-Object {$null -ne $_}).Count){throw 'No mutants were evaluated'}
  $Results | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 -LiteralPath (Join-Path $Root 'mutations.json')
  if($Evidence){
    if(Test-Path -LiteralPath $Evidence){throw 'Refusing existing mutation evidence'}
    New-Item -ItemType Directory -Path $Evidence | Out-Null
    Copy-Item -LiteralPath (Join-Path $Root 'mutations.json') -Destination $Evidence
    foreach($m in $Results){
      $target=Join-Path $Evidence $m.name
      New-Item -ItemType Directory -Path $target | Out-Null
      Copy-Item -LiteralPath (Join-Path $m.directory 'build.log'),(Join-Path $m.directory 'test.log') -Destination $target
    }
  }
  if(@($Results | Where-Object result -ne 'KILLED').Count){return 1}
  return 0
}
