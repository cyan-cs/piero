# CTest may be launched from a plain shell after a developer-shell configure.
# Import only this process's VS/SDK environment when the compiler environment
# is absent. No machine/user environment variables are persisted.
function Initialize-WfToolchain {
  if($env:INCLUDE -and $env:LIB){return}
  . (Join-Path $PSScriptRoot 'wf_vs.ps1')
  $wfVsRoot=Find-WfVisualStudio
  $wfDevShell=Join-Path $wfVsRoot 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll'
  if(!(Test-Path -LiteralPath $wfDevShell)){throw 'VS developer environment unavailable; mutation compilation is BLOCKED, never a kill'}
  Import-Module $wfDevShell
  Enter-VsDevShell -VsInstallPath $wfVsRoot -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
  if(!$env:INCLUDE -or !$env:LIB){throw 'VS/SDK environment initialization failed'}
}
