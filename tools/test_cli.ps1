param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference='Stop'
function Check([string[]]$Arguments,[int]$Expected,[string]$Text){
 $saved=$ErrorActionPreference
 try{$ErrorActionPreference='Continue';$output=(& $Exe @Arguments 2>&1|Out-String);$code=$LASTEXITCODE}finally{$ErrorActionPreference=$saved}
 if($code -ne $Expected -or ($Text -and $output -notmatch $Text)){throw "CLI failure args=$Arguments expected=$Expected actual=$code output=$output"}
 Write-Output "PASS args=$Arguments exit=$code"
}
Check @('--help') 0 'usage:'
Check @('--version') 0 'piero '
Check @('--unknown') 2 ''
Check @('--version','extra') 2 ''
Check @('--config') 2 ''
Check @('--image-worker') 3 'is not a command to run'
Check @('--purge-state') 3 'blocked'
Check @('--continue','C:') 2 'invalid arguments'
Check @('--continue','--setup') 2 'invalid arguments'
Check @('--continue','--version') 2 'invalid arguments'
Check @('--forget-session','--config','x.ini') 2 'invalid arguments'
