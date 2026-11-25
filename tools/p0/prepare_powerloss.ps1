# Inventory and deterministic experiment schedule only. This script never
# shuts down a machine and never counts a process crash as a power-loss trial.
param([int]$Iterations=300,[int]$Seed=1401,[Parameter(Mandatory)][string]$Output)
$ErrorActionPreference='Stop'
if($Iterations -lt 300){throw 'S14 requires at least 300 real VM power-loss iterations'}
if(Test-Path -LiteralPath $Output){throw 'Refusing to overwrite an experiment schedule'}
$vbox=Join-Path $env:ProgramFiles 'Oracle/VirtualBox/VBoxManage.exe'
$vmLines=@(); $probeError=''; $vboxVersion=''
if(Test-Path -LiteralPath $vbox){
  $vboxVersion=(& $vbox --version | Out-String).Trim()
  if($LASTEXITCODE -ne 0){throw 'VirtualBox version probe failed'}
  $vmLines=@(& $vbox list vms)
  if($LASTEXITCODE -ne 0){$probeError='VM enumeration failed';$vmLines=@()}
}else{$probeError='VirtualBox not installed at the supported path'}
$random=New-Object System.Random($Seed)
$points=@('AFTER_INTENT_FLUSH','AFTER_TEMP_ID_FLUSH','MID_COPY','AFTER_COPY_FINISH',
  'AFTER_STRUCTURAL_VERIFY','AFTER_DATA_FLUSH','BEFORE_RENAME','AFTER_RENAME','BEFORE_COMMITTED_FLUSH','AFTER_COMMITTED_FLUSH')
$cases=for($i=0;$i -lt $Iterations;$i++){
  $class=$i%3
  $size=switch($class){0{[uint64]$random.Next(0,4097)}1{[uint64]$random.Next(1048576,8388609)}2{[uint64]$random.Next(67108864,134217729)}}
  [pscustomobject]@{iteration=$i+1;size_class=$class;bytes=$size;generator_seed=$random.Next();
    power_cut_point=$points[$random.Next($points.Count)];jitter_ms=$random.Next(0,101);result='NOT_EXECUTED'}
}
$plan=[ordered]@{
  test_id='P0-S14';created_utc=[DateTime]::UtcNow.ToString('o');result='BLOCKED';
  reason='No wfile-owned disposable Windows guest and dedicated test virtual disk have been provisioned. Existing VMs must never be selected automatically.';
  hypervisor=@{path=$vbox;version=$vboxVersion;registered_vms=$vmLines;probe_error=$probeError};
  seed=$Seed;minimum_real_iterations=300;real_power_loss_iterations=0;process_crashes_counted_as_power_loss=0;
  prerequisites=@('Explicit disposable VM UUID and ownership marker','Windows guest installation and isolated test VHD',
    'Guest agent exchanging durable readiness and recovery reports with host',
    'Expected source SHA256 saved on host outside guest test target before each cut',
    'Hard power-off without guest shutdown, reboot and inspect every committed final hash',
    'Stop immediately on wrong committed hash or unproved deletion; preserve the disk');
  execution_protocol=@('Host generates deterministic source; records expected SHA256 outside test guest disk',
    'Guest uses shared journal/recovery implementation; reports reached injection point to host',
    'Host issues hypervisor hard power-off to the explicitly owned VM UUID (not guest shutdown)',
    'Host reboots guest and collects final-name hashes and shared recovery classification',
    'Increment real_power_loss_iterations only after post-reboot inspection; require at least 100 trials per size class');
  schedule=@($cases)
}
$plan|ConvertTo-Json -Depth 7|Set-Content -LiteralPath $Output -Encoding utf8
Write-Output "Prepared $Iterations NOT_EXECUTED trials across three size classes; measured power-loss count=0; BLOCKED: $Output"
