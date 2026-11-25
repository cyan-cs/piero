# Match every result against the manifest captured before CTest ran.
function Read-WfCtestPassed([string]$Manifest, [string]$Report) {
  $limit = 16MB
  foreach ($path in @($Manifest, $Report)) {
    if ((Get-Item -LiteralPath $path).Length -gt $limit) { throw 'Oversized CTest evidence' }
  }
  $registered = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
  if ($registered.kind -cne 'ctestInfo' -or $registered.version.major -ne 1) { throw 'Unexpected CTest manifest schema' }
  $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
  foreach ($test in $registered.tests) {
    if (!$test.name -or !$names.Add([string]$test.name)) { throw 'Empty or duplicate registered test' }
  }
  if (!$names.Count) { throw 'Empty CTest manifest' }

  $settings = [Xml.XmlReaderSettings]::new()
  $settings.DtdProcessing = [Xml.DtdProcessing]::Prohibit
  $settings.XmlResolver = $null
  $settings.MaxCharactersInDocument = $limit
  $reader = [Xml.XmlReader]::Create($Report, $settings)
  try {
    $document = [Xml.XmlDocument]::new()
    $document.XmlResolver = $null
    $document.Load($reader)
  } finally { $reader.Dispose() }
  $suite = $document.DocumentElement
  if ($suite.get_Name() -cne 'testsuite') { throw 'Unexpected CTest report schema' }
  foreach ($attribute in @('failures', 'errors', 'skipped', 'disabled')) {
    if ($suite.HasAttribute($attribute) -and $suite.GetAttribute($attribute) -cne '0') { throw 'CTest report contains failed or skipped tests' }
  }
  $cases = @($suite.SelectNodes('testcase'))
  if ($suite.GetAttribute('tests') -cne [string]$names.Count -or $cases.Count -ne $names.Count) { throw 'Incomplete CTest report' }
  foreach ($case in $cases) {
    if (!$names.Remove($case.GetAttribute('name'))) { throw 'Unexpected or duplicate CTest result' }
    if ($case.GetAttribute('status') -cne 'run' -or $case.SelectNodes('failure|error|skipped').Count) { throw 'CTest case did not pass' }
  }
  if ($names.Count) { throw 'Missing CTest result' }
  return $cases.Count
}
