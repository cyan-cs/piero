param([Parameter(Mandatory)][string]$Build)
$ErrorActionPreference='Stop'
$buildRoot=(Resolve-Path -LiteralPath $Build).Path
$run='copy-acceptance-'+[guid]::NewGuid().ToString('N').Substring(0,8)
$scratch=Join-Path $buildRoot ($run+'-scratch')
$results=Join-Path $buildRoot ($run+'-evidence')
New-Item -ItemType Directory -Path $scratch | Out-Null
& (Join-Path $buildRoot 'p0_s02s03.exe') --work-base $scratch --results $results `
 --child (Join-Path $buildRoot 'p0_copy_op.exe') --quick --label $run
if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
$rows=@(Get-ChildItem -LiteralPath $results -Filter reps.csv -File -Recurse | ForEach-Object {
 Import-Csv -LiteralPath $_.FullName
})
if($rows.Count -ne 74 -or @($rows|Where-Object ok -ne '1').Count){throw 'Copy acceptance/crash oracle failed'}
Write-Output "Copy source/ownership/HMAC integration: $($rows.Count) PASS; commit remains BLOCKED; no power-loss claim"
# A refused child is a failed experiment, never a successful collector exit.
# Deliberately exceed the existing fixed-size Journal parent field, while
# keeping the test harness paths within its own checked limits.
$refusal=Join-Path $buildRoot ('refusal-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $refusal | Out-Null
$padding=160-$refusal.Length-1
if($padding -lt 1){throw 'Build root too long for deterministic refusal fixture'}
$deep=Join-Path $refusal ('d'*$padding)
New-Item -ItemType Directory -Path $deep | Out-Null
$negative=Join-Path $refusal 'evidence'
& (Join-Path $buildRoot 'p0_s02s03.exe') --work-base $deep --results $negative `
 --child (Join-Path $buildRoot 'p0_copy_op.exe') --quick --label path-refusal
$code=$LASTEXITCODE
$refused=@(Get-ChildItem -LiteralPath $negative -Filter reps.csv -File -Recurse | ForEach-Object {
 Import-Csv -LiteralPath $_.FullName
})
if($code -ne 1 -or $refused.Count -ne 74 -or @($refused|Where-Object ok -eq '1').Count){
 throw 'Refused child did not produce a failed collector exit and 74 rejected observations'
}
Write-Output 'Refused child: collector exit=1, 74 rejected observations; no false PASS'
