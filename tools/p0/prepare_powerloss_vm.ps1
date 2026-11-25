# Provision only a fresh, stopped VM skeleton in an isolated VirtualBox profile.
# No guest install, VM start, guest shutdown, or hard power-off is performed.
param([Parameter(Mandatory)][string]$Output)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../wf_hash.ps1')
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$evidence=[IO.Path]::GetFullPath($Output)
if(!$evidence.StartsWith($repo+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Evidence must be inside the workspace'}
if(Test-Path -LiteralPath $evidence){throw 'Refusing an existing evidence directory'}
$run=[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')+'-vm-preparation-'+[guid]::NewGuid().ToString('N').Substring(0,8)
$root=Join-Path $repo ('out/'+$run)
if(Test-Path -LiteralPath $root){throw 'Refusing an existing VM area'}
$profile=Join-Path $root 'virtualbox-profile'
New-Item -ItemType Directory -Path $profile,(Join-Path $evidence 'artifacts') | Out-Null
$vbox=Join-Path $env:ProgramFiles 'Oracle/VirtualBox/VBoxManage.exe'
$uuid=[guid]::NewGuid().ToString()
$name='wfile-p0-s14-'+$uuid
$oldProfile=$env:VBOX_USER_HOME
$created=@();$failure='';$steps=@()
function Invoke-VBox([string[]]$Arguments,[string]$Step) {
  $log=Join-Path $evidence ('artifacts/'+$Step+'.log')
  $saved=$ErrorActionPreference
  try{$ErrorActionPreference='Continue';& $vbox @Arguments *> $log;$code=$LASTEXITCODE}
  finally{$ErrorActionPreference=$saved}
  $script:steps += [pscustomobject]@{step=$Step;exit_code=$code;result=$(if($code -eq 0){'PASS'}else{'BLOCKED'})}
  if($code -ne 0){throw "VirtualBox $Step refused (exit $code); see $log"}
}
try{
  $env:VBOX_USER_HOME=$profile
  Invoke-VBox @('--version') 'version'
  # Never select or mutate any existing VM, even in this isolated profile.
  Invoke-VBox @('list','vms') 'initial-vms'
  if((Get-Item -LiteralPath (Join-Path $evidence 'artifacts/initial-vms.log')).Length){throw 'Isolated VirtualBox inventory is not empty; no existing VM will be used'}
  # Unregistered until an owned guest/agent is available; the explicit UUID is
  # captured outside its disks and can never be confused with another machine.
  Invoke-VBox @('createvm',('--name='+$name),'--platform-architecture=x86','--ostype=Windows11_64',('--basefolder='+$root),('--uuid='+$uuid)) 'create-unregistered-vm'
  $machine=Join-Path $root ($name+'/'+$name+'.vbox')
  if(!(Test-Path -LiteralPath $machine)){throw 'Expected unregistered VM configuration was not created'}
  $created += $machine
  # Dynamic media are bounded in virtual capacity; no physical/raw disks.
  foreach($disk in @(@{name='guest-os.vdi';mib=65536},@{name='test-only.vdi';mib=2048})){
    $path=Join-Path $root $disk.name
    Invoke-VBox @('createmedium','disk',('--filename='+$path),('--size='+$disk.mib),'--format=VDI','--variant=Standard') ('create-'+$disk.name)
    $created += $path
  }
}catch{$failure=$_.Exception.Message}
finally{$env:VBOX_USER_HOME=$oldProfile}
$files=@($created | ForEach-Object {[pscustomobject]@{path=$_;bytes=(Get-Item -LiteralPath $_).Length;sha256=(Get-WfFileSha256 $_)}})
[ordered]@{test_id='P0-S14';run_id=$run;timestamp_utc=[DateTime]::UtcNow.ToString('o');result='BLOCKED';
  measured_power_loss_iterations=0;process_crashes_substituted=0;vm_started=$false;vm_registered=$false;
  isolated_profile=$profile;vm_uuid=$uuid;vm_name=$name;root=$root;created_files=$files;steps=$steps;failure=$failure;
  missing=@('Installed Windows guest and licensed installation media','Owned guest measurement agent and host protocol','Attach and verify dedicated disk inside guest','At least 300 real hard-power-off/reboot/hash-inspection trials');
  notes='Only explicitly new workspace files may be provisioned. No existing VM, user profile ACL, physical disk, or running machine is modified.'} |
  ConvertTo-Json -Depth 8 | Set-Content -Encoding utf8 -LiteralPath (Join-Path $evidence 'run.json')
[ordered]@{virtualbox=$vbox;host_os=[Environment]::OSVersion.VersionString;isolated_profile=$profile;
  actual_guest_os='NOT_INSTALLED';actual_guest_filesystem='NOT_TESTED'} |
  ConvertTo-Json | Set-Content -Encoding utf8 -LiteralPath (Join-Path $evidence 'environment.json')
$steps | Export-Csv -NoTypeInformation -Encoding utf8 -LiteralPath (Join-Path $evidence 'cases.csv')
Write-Output "S14 VM preparation: BLOCKED; created=$($files.Count); actual power-loss=0; $failure"
