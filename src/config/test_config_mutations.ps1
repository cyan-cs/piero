param(
  [string]$Compiler = 'clang-cl',
  [string]$Build = 'out/config-agent',
  [string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../../tools/wf_toolchain.ps1')
. (Join-Path $PSScriptRoot '../../tools/p0/wf_mutation_text.ps1')
Initialize-WfToolchain
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$buildRoot = [IO.Path]::GetFullPath($Build)
$root = Join-Path $buildRoot ('config-mutants-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root -Force | Out-Null
$config = [IO.File]::ReadAllText((Join-Path $repo 'src/config/wf_config.c'))
$mutants = @(
  @{Name='file_bound';Old='size > WF_CONFIG_MAX_BYTES';New='size >= WF_CONFIG_MAX_BYTES'},
  @{Name='line_bound';Old='length > WF_CONFIG_MAX_LINE';New='length >= WF_CONFIG_MAX_LINE'},
  @{Name='utf8_surrogate';Old='(cp >= 0xD800 && cp <= 0xDFFF)';New='(cp > 0x10FFFF)'},
  @{Name='boolean_strictness';Old='choice(value, "false|true", &number)';New='choice(value, value[0] == ''T'' ? "false|TRUE" : "false|true", &number)'},
  @{Name='invalid_duplicate_default';Old='memcpy(&number, initial, sizeof number);';New='memcpy(&number, target, sizeof number);';Unused=$true},
  @{Name='ratio_total';Old='c->parent_ratio + c->current_ratio + c->preview_ratio != 100';New='c->parent_ratio + c->current_ratio + c->preview_ratio == 101'},
  @{Name='ratio_warning_field';Old='settings[id].offset == offsetof(WfConfig, parent_ratio)';New='(id >= 1 && id <= 3)'},
  @{Name='malformed_section';Old='section[0] = 0;';New='(void)section;'},
  @{Name='theme_id_checked';Old='if (!wf_custom_id_valid(id))';New='if (0)'},
  @{Name='unset_colour_removed';Old='if (b < 0) {';New='if (0) {'},
  @{Name='duplicate_key_override';Old='return DYN_DUPLICATE;';New='return DYN_OK;'}
)
$results = @()
$generated = Join-Path $root 'generated'
& python (Join-Path $repo 'tools/generate_i18n.py') (Join-Path $repo 'languages') $generated
if ($LASTEXITCODE -ne 0) { throw 'Language INI generation failed' }
foreach ($mutant in $mutants) {
  $dir = Join-Path $root $mutant.Name
  New-Item -ItemType Directory -Path $dir | Out-Null
  $source = $config
  $count = Measure-WfAnchor $source $mutant.Old
  if ($count -ne 1) { throw "Mutation anchor changed: $($mutant.Name): $count" }
  $source = Edit-WfAnchor $source $mutant.Old $mutant.New
  if ($mutant.ContainsKey('Unused')) { $source = $source.Replace('const char *initial = (const char *)defaults + s->offset;', 'const char *initial = (const char *)defaults + s->offset; (void)initial;') }
  $file = Join-Path $dir 'wf_config.c'
  [IO.File]::WriteAllText($file,$source)
  $exe = Join-Path $dir 'test_config.exe'
  $runtime = if ($Configuration -eq 'Debug') { '/MDd' } else { '/MD' }
  $args = @('/nologo','/std:c17','/W4','/WX','/utf-8',$runtime,
    "/I$repo/src","/I$repo/src/config","/I$repo/tests","/I$generated","/Fo$dir/","/Fe$exe",
    (Join-Path $repo 'tests/test_config.c'),$file,(Join-Path $repo 'src/config/wf_ini_edit.c'),(Join-Path $repo 'src/config/wf_cli.c'),(Join-Path $repo 'src/render/wf_theme.c'),(Join-Path $repo 'src/i18n/wf_i18n.c'),
    (Join-Path $repo 'src/i18n/wf_language.c'),(Join-Path $repo 'src/edit/wf_eenc.c'),
    (Join-Path $repo 'src/input/wf_stroke.c'),(Join-Path $repo 'src/input/wf_commands.c'),(Join-Path $repo 'src/input/wf_keymap.c'),(Join-Path $repo 'src/edit/wf_ekeymap.c'),(Join-Path $repo 'src/text/wf_utf.c'))
  & $Compiler @args *> (Join-Path $dir 'build.log')
  if ($LASTEXITCODE -ne 0) { throw "Mutation did not build (not counted as killed): $($mutant.Name) $dir" }
  & $exe *> (Join-Path $dir 'test.log')
  $results += Test-WfMutant $mutant.Name $dir $LASTEXITCODE
}
exit (Complete-WfMutations $results $root $null)
