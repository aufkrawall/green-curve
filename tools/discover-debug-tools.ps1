<#
.SYNOPSIS
  Discovers local debugging, binary-analysis, symbol, capture, and related developer tools without installing or mutating them.

.DESCRIPTION
  Generic project tooling helper used by llm-wiki/debug-tools.md and by security-audit tooling.
  Resolution uses explicit tool-path overrides, project/local managed roots, standard Windows locations, and PATH with tool-specific precedence.
  The script writes a machine-local debug-tool manifest and Markdown availability report unless -NoWrite is used.
  It never installs packages, downloads tools, edits PATH, or changes debugger/system state.

.PARAMETER ProjectRoot
  Repository root used to resolve relative entries from tool-paths.env.

.PARAMETER OutputRoot
  Directory for debug-tool-manifest.json and the availability report.
  Defaults to %LOCALAPPDATA%\LLMDebugTools when available, otherwise a temp-directory fallback.

.PARAMETER ToolPathsEnv
  Optional local key=value override file. Missing files are allowed.

.PARAMETER AdditionalToolRoots
  Additional local roots to search recursively, for example a security-audit install root.
  Existing managed Sysinternals, FFmpeg, and vswhere directories under
  %LOCALAPPDATA%\SecurityAuditTools\bin are also searched automatically.

.PARAMETER NoWrite
  Perform discovery without writing manifest/report files.
#>

[CmdletBinding()]
param(
  [string]$ProjectRoot = ".",
  [string]$OutputRoot = "",
  [string]$ToolPathsEnv = ".\tool-paths.env",
  [string[]]$AdditionalToolRoots = @(),
  [switch]$NoWrite
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ProjectRoot = [IO.Path]::GetFullPath($ProjectRoot)
if (-not $OutputRoot) {
  if ($env:LOCALAPPDATA) {
    $OutputRoot = Join-Path $env:LOCALAPPDATA "LLMDebugTools"
  } else {
    $OutputRoot = Join-Path ([IO.Path]::GetTempPath()) "LLMDebugTools"
  }
}
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)

$ManifestPath = Join-Path $OutputRoot "debug-tool-manifest.json"
$WarningsPath = Join-Path $OutputRoot "debug-tool-warnings.txt"
$MarkdownPath = Join-Path $OutputRoot "debug-tool-availability.md"

$script:Results = [System.Collections.Generic.List[object]]::new()
$script:Warnings = [System.Collections.Generic.List[string]]::new()
$script:Overrides = @{}

function Add-WarningMessage {
  param([string]$Message)
  $script:Warnings.Add($Message) | Out-Null
  Write-Warning $Message
}

function Add-Result {
  param(
    [string]$Name,
    [string]$Category,
    [string]$Status,
    [string]$Path = "",
    [string]$Source = "",
    [string]$Notes = ""
  )

  $script:Results.Add([pscustomobject]@{
    name = $Name
    category = $Category
    status = $Status
    path = $Path
    source = $Source
    notes = $Notes
  }) | Out-Null
}

function Resolve-ConfiguredPath {
  param([string]$Value)

  if ([string]::IsNullOrWhiteSpace($Value)) { return $null }

  $trimmed = $Value.Trim().Trim('"')
  $expanded = [Environment]::ExpandEnvironmentVariables($trimmed)
  if (-not [IO.Path]::IsPathRooted($expanded)) {
    $expanded = Join-Path $ProjectRoot $expanded
  }

  return [IO.Path]::GetFullPath($expanded)
}

function Read-ToolPathOverrides {
  param([string]$Path)

  $values = @{}
  if (-not $Path) { return $values }

  $resolved = Resolve-ConfiguredPath -Value $Path
  if (-not $resolved -or -not (Test-Path -LiteralPath $resolved)) { return $values }

  foreach ($line in Get-Content -LiteralPath $resolved -ErrorAction Stop) {
    $trimmed = $line.Trim()
    if (-not $trimmed -or $trimmed.StartsWith("#")) { continue }

    $parts = $trimmed -split "=", 2
    if ($parts.Count -ne 2) {
      Add-WarningMessage ("Ignoring malformed tool-path override line in {0}: {1}" -f $resolved, $trimmed)
      continue
    }

    $key = $parts[0].Trim()
    $value = $parts[1].Trim()
    if ($key) { $values[$key] = $value }
  }

  return $values
}

function Get-OverrideValue {
  param([string]$Name)

  if ($script:Overrides.ContainsKey($Name) -and $script:Overrides[$Name]) {
    return [string]$script:Overrides[$Name]
  }

  $environmentValue = [Environment]::GetEnvironmentVariable($Name, "Process")
  if ($environmentValue) { return $environmentValue }
  return $null
}

function Get-CommandPath {
  param([string]$Name)

  $command = Get-Command $Name -ErrorAction SilentlyContinue
  if ($command) { return $command.Source }
  return $null
}

function Find-ToolInRoots {
  param(
    [string]$ToolName,
    [string[]]$Roots,
    [switch]$Recurse
  )

  foreach ($rawRoot in @($Roots | Where-Object { $_ } | Select-Object -Unique)) {
    $root = Resolve-ConfiguredPath -Value $rawRoot
    if (-not $root -or -not (Test-Path -LiteralPath $root)) { continue }

    $direct = Join-Path $root $ToolName
    if (Test-Path -LiteralPath $direct) { return $direct }

    if ($Recurse) {
      $match = Get-ChildItem -LiteralPath $root -Recurse -File -Filter $ToolName -ErrorAction SilentlyContinue |
        Sort-Object FullName |
        Select-Object -First 1
      if ($match) { return $match.FullName }
    }
  }

  return $null
}

function Get-EffectiveAdditionalToolRoots {
  param([string[]]$ConfiguredRoots = @())

  $roots = [System.Collections.Generic.List[string]]::new()
  foreach ($configuredRoot in @($ConfiguredRoots)) {
    if (-not $configuredRoot) { continue }
    $resolved = Resolve-ConfiguredPath -Value $configuredRoot
    if ($resolved -and (Test-Path -LiteralPath $resolved)) {
      $roots.Add($resolved) | Out-Null
    }
  }

  if ($env:LOCALAPPDATA) {
    $securityAuditBin = Join-Path $env:LOCALAPPDATA "SecurityAuditTools\bin"
    foreach ($managedSubdirectory in @("sysinternals", "ffmpeg", "vswhere")) {
      $managedRoot = Join-Path $securityAuditBin $managedSubdirectory
      if (Test-Path -LiteralPath $managedRoot) {
        $roots.Add([IO.Path]::GetFullPath($managedRoot)) | Out-Null
      }
    }
  }

  return @($roots | Select-Object -Unique)
}


function Get-WindowsSdkDebuggerArchitectures {
  $hostArchitecture = [string]$env:PROCESSOR_ARCHITECTURE
  switch ($hostArchitecture.ToUpperInvariant()) {
    "ARM64" { return @("arm64", "x64", "x86", "arm") }
    "AMD64" { return @("x64", "x86", "arm64", "arm") }
    default { return @("x86", "x64", "arm", "arm64") }
  }
}

function Get-WindowsSdkDebuggerCandidatePathsForArchitecture {
  param(
    [string]$ToolName,
    [ValidateSet("x86", "x64", "arm", "arm64")]
    [string]$Architecture
  )

  $paths = [System.Collections.Generic.List[string]]::new()
  $overrideKeys = @{
    x86 = "WINDOWS_SDK_DEBUGGERS_X86"
    x64 = "WINDOWS_SDK_DEBUGGERS_X64"
    arm = "WINDOWS_SDK_DEBUGGERS_ARM"
    arm64 = "WINDOWS_SDK_DEBUGGERS_ARM64"
  }

  $override = Get-OverrideValue -Name $overrideKeys[$Architecture]
  if ($override) {
    $root = Resolve-ConfiguredPath -Value $override
    if ($root) { $paths.Add((Join-Path $root $ToolName)) | Out-Null }
  }

  foreach ($baseRoot in @(
    [Environment]::GetEnvironmentVariable("ProgramFiles(x86)", "Process"),
    [Environment]::GetEnvironmentVariable("ProgramFiles", "Process")
  ) | Where-Object { $_ } | Select-Object -Unique) {
    $paths.Add((Join-Path (Join-Path (Join-Path (Join-Path $baseRoot "Windows Kits") "10") "Debuggers\$Architecture") $ToolName)) | Out-Null
  }

  return @($paths | Select-Object -Unique)
}

function Get-WindowsSdkDebuggerCandidatePaths {
  param([string]$ToolName)

  $paths = [System.Collections.Generic.List[string]]::new()
  foreach ($architecture in @(Get-WindowsSdkDebuggerArchitectures)) {
    foreach ($candidate in @(Get-WindowsSdkDebuggerCandidatePathsForArchitecture -ToolName $ToolName -Architecture $architecture)) {
      $paths.Add($candidate) | Out-Null
    }
  }
  return @($paths | Select-Object -Unique)
}

function Get-WindowsSdkDebuggerArchitectureMatrix {
  param([string[]]$ToolNames)

  $matrix = [System.Collections.Generic.List[object]]::new()
  foreach ($architecture in @(Get-WindowsSdkDebuggerArchitectures)) {
    foreach ($toolName in @($ToolNames)) {
      $resolvedPath = $null
      foreach ($candidate in @(Get-WindowsSdkDebuggerCandidatePathsForArchitecture -ToolName $toolName -Architecture $architecture)) {
        if (Test-Path -LiteralPath $candidate) {
          $resolvedPath = $candidate
          break
        }
      }

      if ($resolvedPath) {
        $matrix.Add([pscustomobject]@{
          tool = $toolName
          architecture = $architecture
          status = "available"
          path = $resolvedPath
          source = "override-or-sdk-layout"
        }) | Out-Null
      } else {
        $matrix.Add([pscustomobject]@{
          tool = $toolName
          architecture = $architecture
          status = "missing"
          path = ""
          source = ""
        }) | Out-Null
      }
    }
  }

  return @($matrix)
}

function Resolve-WindowsSdkDebuggerTool {
  param([string]$ToolName)

  foreach ($candidate in @(Get-WindowsSdkDebuggerCandidatePaths -ToolName $ToolName)) {
    if (Test-Path -LiteralPath $candidate) {
      Add-Result -Name $ToolName -Category "Windows SDK Debugging Tools" -Status "available" -Path $candidate -Source "override-or-sdk-layout"
      return
    }
  }

  $path = Get-CommandPath -Name $ToolName
  if ($path) {
    Add-Result -Name $ToolName -Category "Windows SDK Debugging Tools" -Status "available-in-path" -Path $path -Source "PATH"
  } else {
    Add-Result -Name $ToolName -Category "Windows SDK Debugging Tools" -Status "missing" -Notes "Not found in configured x86/x64/ARM/ARM64 SDK roots, standard Windows SDK locations, or PATH"
  }
}

function Get-MsvcBinaryToolArchitecturePreferences {
  $hostArchitecture = [string]$env:PROCESSOR_ARCHITECTURE
  switch ($hostArchitecture.ToUpperInvariant()) {
    "ARM64" {
      return @(
        "Hostarm64\arm64",
        "Hostarm64\x64",
        "Hostarm64\x86",
        "Hostx64\x64",
        "Hostx64\x86",
        "Hostx86\x86"
      )
    }
    "AMD64" {
      return @(
        "Hostx64\x64",
        "Hostx64\x86",
        "Hostx86\x86",
        "Hostx86\x64",
        "Hostarm64\arm64"
      )
    }
    default {
      return @(
        "Hostx86\x86",
        "Hostx86\x64",
        "Hostx64\x64",
        "Hostx64\x86",
        "Hostarm64\arm64"
      )
    }
  }
}

function Get-MsvcOverrideKeys {
  $hostArchitecture = [string]$env:PROCESSOR_ARCHITECTURE
  switch ($hostArchitecture.ToUpperInvariant()) {
    "ARM64" { return @("MSVC_TOOLS_ARM64", "MSVC_TOOLS_X64", "MSVC_TOOLS_X86") }
    "AMD64" { return @("MSVC_TOOLS_X64", "MSVC_TOOLS_X86", "MSVC_TOOLS_ARM64") }
    default { return @("MSVC_TOOLS_X86", "MSVC_TOOLS_X64", "MSVC_TOOLS_ARM64") }
  }
}

function Select-PreferredMsvcToolMatch {
  param([object[]]$Candidates)

  if (-not $Candidates -or $Candidates.Count -eq 0) { return $null }

  foreach ($preference in @(Get-MsvcBinaryToolArchitecturePreferences)) {
    $candidate = $Candidates |
      Where-Object {
        $normalized = $_.FullName.Replace("/", "\")
        $normalized -like "*\bin\$preference\*"
      } |
      Sort-Object FullName -Descending |
      Select-Object -First 1
    if ($candidate) { return $candidate }
  }

  return $Candidates | Sort-Object FullName -Descending | Select-Object -First 1
}

function Get-VsWherePath {
  $path = Get-CommandPath -Name "vswhere.exe"
  if ($path) { return $path }

  $additional = Find-ToolInRoots -ToolName "vswhere.exe" -Roots $AdditionalToolRoots -Recurse
  if ($additional) { return $additional }

  $programFilesX86 = [Environment]::GetEnvironmentVariable("ProgramFiles(x86)", "Process")
  if ($programFilesX86) {
    $known = Join-Path $programFilesX86 "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $known) { return $known }
  }

  return $null
}

function Get-VisualStudioRoots {
  $roots = [System.Collections.Generic.List[string]]::new()
  $vswhere = Get-VsWherePath

  if ($vswhere) {
    try {
      $installations = @(& $vswhere -all -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
      foreach ($installation in $installations) {
        if ($installation) { $roots.Add([string]$installation) | Out-Null }
      }
    } catch {
      Add-WarningMessage "vswhere failed while locating MSVC tools: $($_.Exception.Message)"
    }
  }

  $programFiles = [Environment]::GetEnvironmentVariable("ProgramFiles", "Process")
  if ($programFiles) {
    foreach ($edition in @("Community", "Professional", "Enterprise", "BuildTools")) {
      $roots.Add((Join-Path $programFiles "Microsoft Visual Studio\2022\$edition")) | Out-Null
    }
  }

  return @($roots | Select-Object -Unique)
}

function Resolve-MsvcTool {
  param([string]$ToolName)

  $overrideRoots = foreach ($key in @(Get-MsvcOverrideKeys)) {
    $value = Get-OverrideValue -Name $key
    if ($value) { $value }
  }
  $overrideRoots = @($overrideRoots | Select-Object -Unique)

  $path = Find-ToolInRoots -ToolName $ToolName -Roots $overrideRoots -Recurse
  if ($path) {
    Add-Result -Name $ToolName -Category "MSVC binary tools" -Status "available" -Path $path -Source "tool-path override"
    return
  }

  $path = Find-ToolInRoots -ToolName $ToolName -Roots $AdditionalToolRoots -Recurse
  if ($path) {
    Add-Result -Name $ToolName -Category "MSVC binary tools" -Status "available" -Path $path -Source "additional local root"
    return
  }

  $msvcToolMatches = [System.Collections.Generic.List[object]]::new()
  foreach ($root in @(Get-VisualStudioRoots)) {
    if (-not $root -or -not (Test-Path -LiteralPath $root)) { continue }

    Get-ChildItem -LiteralPath $root -Recurse -File -Filter $ToolName -ErrorAction SilentlyContinue |
      Where-Object {
        $normalized = $_.FullName.Replace("/", "\")
        $normalized -match "\\VC\\Tools\\MSVC\\.*\\bin\\Host(?:x64|x86|arm64)\\(?:x64|x86|arm64)\\"
      } |
      ForEach-Object { $msvcToolMatches.Add($_) | Out-Null }
  }

  $preferred = Select-PreferredMsvcToolMatch -Candidates @($msvcToolMatches)
  if ($preferred) {
    Add-Result -Name $ToolName -Category "MSVC binary tools" -Status "available" -Path $preferred.FullName -Source "Visual Studio discovery"
    return
  }

  $path = Get-CommandPath -Name $ToolName
  if ($path) {
    Add-Result -Name $ToolName -Category "MSVC binary tools" -Status "available-in-path" -Path $path -Source "PATH"
  } else {
    Add-Result -Name $ToolName -Category "MSVC binary tools" -Status "missing"
  }
}

function Resolve-GenericTool {
  param(
    [string]$ToolName,
    [string]$Category,
    [string[]]$OverrideKeys = @(),
    [string[]]$KnownRoots = @()
  )

  $overrideRoots = foreach ($key in $OverrideKeys) {
    $value = Get-OverrideValue -Name $key
    if ($value) { $value }
  }

  $path = Find-ToolInRoots -ToolName $ToolName -Roots @($overrideRoots) -Recurse
  if ($path) {
    Add-Result -Name $ToolName -Category $Category -Status "available" -Path $path -Source "tool-path override"
    return
  }

  $path = Find-ToolInRoots -ToolName $ToolName -Roots $AdditionalToolRoots -Recurse
  if ($path) {
    Add-Result -Name $ToolName -Category $Category -Status "available" -Path $path -Source "additional local root"
    return
  }

  $path = Find-ToolInRoots -ToolName $ToolName -Roots $KnownRoots
  if ($path) {
    Add-Result -Name $ToolName -Category $Category -Status "available" -Path $path -Source "known system location"
    return
  }

  $path = Get-CommandPath -Name $ToolName
  if ($path) {
    Add-Result -Name $ToolName -Category $Category -Status "available-in-path" -Path $path -Source "PATH"
  } else {
    Add-Result -Name $ToolName -Category $Category -Status "missing"
  }
}

$script:Overrides = Read-ToolPathOverrides -Path $ToolPathsEnv
$AdditionalToolRoots = @(Get-EffectiveAdditionalToolRoots -ConfiguredRoots $AdditionalToolRoots)

$windowsSdkDebuggerTools = @(
  "cdb.exe",
  "windbg.exe",
  "dumpchk.exe",
  "symchk.exe",
  "dbh.exe",
  "pdbcopy.exe",
  "symstore.exe",
  "gflags.exe",
  "umdh.exe"
)

foreach ($tool in $windowsSdkDebuggerTools) {
  Resolve-WindowsSdkDebuggerTool -ToolName $tool
}

$windowsSdkDebuggerArchitectures = @(Get-WindowsSdkDebuggerArchitectureMatrix -ToolNames $windowsSdkDebuggerTools)

$windowsApps = @()
if ($env:LOCALAPPDATA) { $windowsApps += (Join-Path $env:LOCALAPPDATA "Microsoft\WindowsApps") }
Resolve-GenericTool -ToolName "WinDbgX.exe" -Category "WinDbg" -KnownRoots $windowsApps

foreach ($tool in @("dumpbin.exe", "link.exe", "lib.exe", "editbin.exe", "undname.exe")) {
  Resolve-MsvcTool -ToolName $tool
}

$llvmRoots = [System.Collections.Generic.List[string]]::new()
$llvmOverride = Get-OverrideValue -Name "LLVM_ROOT"
if ($llvmOverride) { $llvmRoots.Add($llvmOverride) | Out-Null }
$programFiles = [Environment]::GetEnvironmentVariable("ProgramFiles", "Process")
if ($programFiles) { $llvmRoots.Add((Join-Path $programFiles "LLVM\bin")) | Out-Null }
$programFilesX86 = [Environment]::GetEnvironmentVariable("ProgramFiles(x86)", "Process")
if ($programFilesX86) { $llvmRoots.Add((Join-Path $programFilesX86 "LLVM\bin")) | Out-Null }
if ($env:LOCALAPPDATA) { $llvmRoots.Add((Join-Path $env:LOCALAPPDATA "Programs\LLVM\bin")) | Out-Null }
foreach ($tool in @("llvm-objdump.exe", "llvm-strings.exe")) {
  Resolve-GenericTool -ToolName $tool -Category "LLVM tools" -OverrideKeys @("LLVM_ROOT") -KnownRoots $llvmRoots
}

foreach ($tool in @("procdump.exe", "sigcheck.exe", "strings.exe", "handle.exe", "listdlls.exe", "vmmap.exe", "Procmon.exe", "procexp.exe")) {
  Resolve-GenericTool -ToolName $tool -Category "Sysinternals" -OverrideKeys @("SYSINTERNALS_ROOT")
}

foreach ($tool in @("ffmpeg.exe", "ffprobe.exe")) {
  Resolve-GenericTool -ToolName $tool -Category "media/capture" -OverrideKeys @("FFMPEG_ROOT")
}

$vswhereKnownRoots = @()
$programFilesX86 = [Environment]::GetEnvironmentVariable("ProgramFiles(x86)", "Process")
if ($programFilesX86) { $vswhereKnownRoots += (Join-Path $programFilesX86 "Microsoft Visual Studio\Installer") }
Resolve-GenericTool -ToolName "vswhere.exe" -Category "Visual Studio discovery" -KnownRoots $vswhereKnownRoots

$resolvedToolPathsEnv = $null
if ($ToolPathsEnv) { $resolvedToolPathsEnv = Resolve-ConfiguredPath -Value $ToolPathsEnv }

$manifest = [pscustomobject]@{
  generated_at = (Get-Date).ToString("o")
  project_root = $ProjectRoot
  output_root = $OutputRoot
  tool_paths_env = $resolvedToolPathsEnv
  additional_tool_roots = @($AdditionalToolRoots)
  windows_sdk_debugger_architectures = $windowsSdkDebuggerArchitectures
  host = [pscustomobject]@{
    computer_name = $env:COMPUTERNAME
    user = $env:USERNAME
    processor_architecture = $env:PROCESSOR_ARCHITECTURE
    powershell_version = $PSVersionTable.PSVersion.ToString()
  }
  results = $script:Results
  warnings = $script:Warnings
}

if (-not $NoWrite) {
  if (-not (Test-Path -LiteralPath $OutputRoot)) {
    New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
  }

  $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $ManifestPath -Encoding UTF8
  $script:Warnings | Set-Content -LiteralPath $WarningsPath -Encoding UTF8

  $md = [System.Collections.Generic.List[string]]::new()
  $md.Add("# Debug Tool Availability")
  $md.Add("")
  $md.Add("- Generated: $($manifest.generated_at)")
  $md.Add('- Project root: `' + $ProjectRoot + '`')
  $md.Add('- Tool-path overrides: `' + $manifest.tool_paths_env + '`')
  $md.Add('- Additional tool roots: `' + (@($manifest.additional_tool_roots) -join '; ') + '`')
  $md.Add("")
  $md.Add("## Windows SDK debugger architecture matrix")
  $md.Add("")
  $md.Add("| Architecture | Tool | Status | Path | Source |")
  $md.Add("|---|---|---|---|---|")
  foreach ($variant in $windowsSdkDebuggerArchitectures) {
    $md.Add(('| {0} | {1} | {2} | `{3}` | {4} |' -f $variant.architecture, $variant.tool, $variant.status, $variant.path, $variant.source))
  }
  $md.Add("")
  $md.Add("## Preferred/general tool resolution")
  $md.Add("")
  $md.Add("| Tool | Category | Status | Path | Source | Notes |")
  $md.Add("|---|---|---|---|---|---|")
  foreach ($result in $script:Results) {
    $md.Add(('| {0} | {1} | {2} | `{3}` | {4} | {5} |' -f $result.name, $result.category, $result.status, $result.path, $result.source, $result.notes))
  }
  $md | Set-Content -LiteralPath $MarkdownPath -Encoding UTF8

  Write-Host "Wrote debug tool manifest: $ManifestPath"
  Write-Host "Wrote debug tool warnings: $WarningsPath"
  Write-Host "Wrote debug tool availability report: $MarkdownPath"
}

$manifest
