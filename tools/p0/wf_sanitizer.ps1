# Shared by the mutation harnesses. A sanitized build (-DWF_SANITIZE=ON) links its libraries against the
# ASan/UBSan runtime, so a mutant linked to them has to be instrumented and linked the same way.
# This changes how a mutant is BUILT, never how it is judged: the kill verdict logic is untouched.
function Get-WfSanitizer([string]$Compiler,[string]$BuildRoot){
 $cache=Join-Path $BuildRoot 'CMakeCache.txt'
 $on=(Test-Path -LiteralPath $cache) -and
  [bool](Select-String -LiteralPath $cache -Pattern '^WF_SANITIZE:BOOL=(ON|TRUE|1)$' -Quiet)
 $san=[pscustomobject]@{Enabled=$on;Compile=@();Link=@();HangFactor=1}
 if($on){
  $resource=(& $Compiler '/clang:-print-resource-dir' 2>$null | Select-Object -First 1).Trim()
  $dir=Join-Path $resource 'lib/windows'
  # VS initialization prepends its own compiler-rt DLL directory. Controls
  # and mutants must load the runtime selected by this build, not VS's copy.
  $runtime=Select-String -LiteralPath $cache -Pattern '^WF_ASAN_DLL:FILEPATH=(.+)$' |
   Select-Object -First 1
  $runtimeDir=if($runtime){Split-Path -Parent $runtime.Matches[0].Groups[1].Value}else{$dir}
  $env:PATH=$runtimeDir+';'+$env:PATH
  $san.Compile=@('-fsanitize=address,undefined','-fno-sanitize-recover=undefined')
  $san.Link=@("/LIBPATH:$dir",'/OPT:NOICF','/INCREMENTAL:NO','/INCLUDE:__asan_seh_interceptor',
   "/WHOLEARCHIVE:$dir/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib",'clang_rt.asan_dynamic-x86_64.lib',
   'clang_rt.asan_dynamic_runtime_thunk-x86_64.lib','clang_rt.ubsan_standalone-x86_64.lib')
  # Instrumented code runs several times slower; a mutant that is a deliberate slowdown needs the same
  # factor before it can be called hung. The hang limit of a normal build is not touched.
  $san.HangFactor=3
 }
 return $san
}

# The CRT the test build uses: ASan builds use the release DLL runtime even in Debug.
function Get-WfRuntime([string]$Configuration,$San){
 if($Configuration -eq 'Debug' -and -not $San.Enabled){return '/MDd'}
 return '/MD'
}

# Compile options go before /link, linker options at the end.
function Add-WfSanitizerArgs([string[]]$Arguments,$San){
 if(-not $San.Enabled){return $Arguments}
 $at=[array]::IndexOf($Arguments,'/link')
 if($at -lt 0){return $Arguments+$San.Compile}
 return @($Arguments[0..($at-1)])+$San.Compile+@($Arguments[$at..($Arguments.Count-1)])+$San.Link
}
