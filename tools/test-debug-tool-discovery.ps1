[CmdletBinding()]
param(
  [string]$DiscoveryScriptPath = (Join-Path $PSScriptRoot "discover-debug-tools.ps1")
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Assert-SequenceEqual {
  param([string]$Label, [object[]]$Actual, [object[]]$Expected)
  $actualText = @($Actual) -join "|"
  $expectedText = @($Expected) -join "|"
  if ($actualText -ne $expectedText) {
    throw "$Label mismatch. Expected '$expectedText', got '$actualText'."
  }
}

$tokens = $null
$parseErrors = $null
$resolvedScriptPath = (Resolve-Path -LiteralPath $DiscoveryScriptPath).Path
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
  $resolvedScriptPath,
  [ref]$tokens,
  [ref]$parseErrors
)
if ($parseErrors -and $parseErrors.Count -gt 0) {
  $messages = $parseErrors | ForEach-Object { $_.Message }
  throw "Discovery script parse failed: $($messages -join '; ')"
}

foreach ($functionName in @(
  "Resolve-ConfiguredPath",
  "Read-ToolPathOverrides",
  "Get-OverrideValue",
  "Find-ToolInRoots",
  "Get-EffectiveAdditionalToolRoots",
  "Get-WindowsSdkDebuggerArchitectures",
  "Get-WindowsSdkDebuggerCandidatePathsForArchitecture",
  "Get-WindowsSdkDebuggerCandidatePaths",
  "Get-WindowsSdkDebuggerArchitectureMatrix",
  "Get-MsvcBinaryToolArchitecturePreferences",
  "Get-MsvcOverrideKeys",
  "Select-PreferredMsvcToolMatch",
  "Resolve-MsvcTool"
)) {
  $functionAst = $ast.Find(
    {
      param($node)
      $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq $functionName
    },
    $true
  )
  if (-not $functionAst) { throw "Required function '$functionName' was not found." }
  . ([scriptblock]::Create($functionAst.Extent.Text))
}

$originalProgramFiles = [Environment]::GetEnvironmentVariable("ProgramFiles", "Process")
$originalProgramFilesX86 = [Environment]::GetEnvironmentVariable("ProgramFiles(x86)", "Process")
$originalProcessorArchitecture = [Environment]::GetEnvironmentVariable("PROCESSOR_ARCHITECTURE", "Process")
$originalLocalAppData = [Environment]::GetEnvironmentVariable("LOCALAPPDATA", "Process")
$overrideFile = $null
$vsRoot = $null
$rootA = $null
$rootB = $null
$testLocalAppData = $null

try {
  $script:Warnings = [System.Collections.Generic.List[string]]::new()
  function Add-WarningMessage { param([string]$Message) $script:Warnings.Add($Message) | Out-Null }

  $ProjectRoot = [IO.Path]::GetTempPath()
  $rootA = Join-Path $ProjectRoot "llm-template-test-pf"
  $rootB = Join-Path $ProjectRoot "llm-template-test-pfx86"
  [Environment]::SetEnvironmentVariable("ProgramFiles", $rootA, "Process")
  [Environment]::SetEnvironmentVariable("ProgramFiles(x86)", $rootB, "Process")
  $env:PROCESSOR_ARCHITECTURE = "AMD64"

  $testLocalAppData = Join-Path $ProjectRoot "llm-template-test-localappdata"
  [Environment]::SetEnvironmentVariable("LOCALAPPDATA", $testLocalAppData, "Process")
  $managedBin = Join-Path $testLocalAppData "SecurityAuditTools\bin"
  $managedFfmpegDir = Join-Path $managedBin "ffmpeg\extract\test\bin"
  New-Item -ItemType Directory -Path $managedFfmpegDir -Force | Out-Null
  $managedFfmpeg = Join-Path $managedFfmpegDir "ffmpeg.exe"
  Set-Content -LiteralPath $managedFfmpeg -Value "" -Encoding ASCII

  $effectiveRoots = @(Get-EffectiveAdditionalToolRoots -ConfiguredRoots @())
  $managedFfmpegRoot = Join-Path $managedBin "ffmpeg"
  if ($effectiveRoots -notcontains ([IO.Path]::GetFullPath($managedFfmpegRoot))) {
    throw "SecurityAuditTools managed FFmpeg root was not auto-discovered."
  }
  $resolvedManagedFfmpeg = Find-ToolInRoots -ToolName "ffmpeg.exe" -Roots $effectiveRoots -Recurse
  if ($resolvedManagedFfmpeg -ne $managedFfmpeg) {
    throw "Managed FFmpeg was not discoverable through automatic additional roots."
  }

  $overrideRoot = Join-Path $ProjectRoot "sdk-x86-override"
  $overrideFile = Join-Path $ProjectRoot "llm-debug-tool-paths-test.env"
  @(
    "WINDOWS_SDK_DEBUGGERS_X86=$overrideRoot",
    "LLVM_ROOT=.\llvm-test",
    "MALFORMED_LINE"
  ) | Set-Content -LiteralPath $overrideFile -Encoding UTF8

  $script:Overrides = Read-ToolPathOverrides -Path $overrideFile
  if ((Get-OverrideValue -Name "WINDOWS_SDK_DEBUGGERS_X86") -ne $overrideRoot) {
    throw "tool-paths.env override was not loaded."
  }

  $expectedMalformedWarning = "Ignoring malformed tool-path override line in {0}: MALFORMED_LINE" -f ([IO.Path]::GetFullPath($overrideFile))
  if ($script:Warnings -notcontains $expectedMalformedWarning) {
    throw "Malformed override warning was not emitted with the resolved path."
  }

  $paths = @(Get-WindowsSdkDebuggerCandidatePaths -ToolName "cdb.exe")
  $expectedOverride = Join-Path $overrideRoot "cdb.exe"
  if ($paths -notcontains $expectedOverride) {
    throw "Configured x86 debugger root was not included in cdb.exe candidates."
  }

  $architectures = @(
    $paths |
      Where-Object { $_ -notlike "$overrideRoot*" } |
      ForEach-Object { Split-Path -Leaf (Split-Path -Parent $_) }
  )
  Assert-SequenceEqual -Label "AMD64 standard debugger path preference" -Actual $architectures -Expected @(
    "x64", "x64", "x86", "x86", "arm64", "arm64", "arm", "arm"
  )

  $sdkX64Dir = Join-Path $rootB "Windows Kits\10\Debuggers\x64"
  $sdkX86Dir = Join-Path $rootB "Windows Kits\10\Debuggers\x86"
  New-Item -ItemType Directory -Path $sdkX64Dir -Force | Out-Null
  New-Item -ItemType Directory -Path $sdkX86Dir -Force | Out-Null
  $sdkX64Cdb = Join-Path $sdkX64Dir "cdb.exe"
  $sdkX86Cdb = Join-Path $sdkX86Dir "cdb.exe"
  Set-Content -LiteralPath $sdkX64Cdb -Value "" -Encoding ASCII
  Set-Content -LiteralPath $sdkX86Cdb -Value "" -Encoding ASCII

  $sdkMatrix = @(Get-WindowsSdkDebuggerArchitectureMatrix -ToolNames @("cdb.exe"))
  $x64Cdb = $sdkMatrix | Where-Object { $_.architecture -eq "x64" -and $_.tool -eq "cdb.exe" } | Select-Object -First 1
  $x86Cdb = $sdkMatrix | Where-Object { $_.architecture -eq "x86" -and $_.tool -eq "cdb.exe" } | Select-Object -First 1
  if (-not $x64Cdb -or $x64Cdb.status -ne "available" -or $x64Cdb.path -ne $sdkX64Cdb) {
    throw "Architecture matrix did not report x64 cdb.exe."
  }
  if (-not $x86Cdb -or $x86Cdb.status -ne "available" -or $x86Cdb.path -ne $sdkX86Cdb) {
    throw "Architecture matrix did not report x86 cdb.exe."
  }

  Assert-SequenceEqual -Label "AMD64 MSVC override preference" -Actual @(Get-MsvcOverrideKeys) -Expected @(
    "MSVC_TOOLS_X64", "MSVC_TOOLS_X86", "MSVC_TOOLS_ARM64"
  )

  $msvcMatches = @(
    [pscustomobject]@{ FullName = "C:\VS\VC\Tools\MSVC\14.0\bin\Hostx86\x86\dumpbin.exe" },
    [pscustomobject]@{ FullName = "C:\VS\VC\Tools\MSVC\14.0\bin\Hostx64\x86\dumpbin.exe" },
    [pscustomobject]@{ FullName = "C:\VS\VC\Tools\MSVC\14.0\bin\Hostx64\x64\dumpbin.exe" }
  )
  $preferred = Select-PreferredMsvcToolMatch -Candidates $msvcMatches
  if ($preferred.FullName -notlike "*\Hostx64\x64\dumpbin.exe") {
    throw "AMD64 MSVC preference did not select Hostx64\x64."
  }

  # Exercise Resolve-MsvcTool through its regex filter. PowerShell's automatic
  # $Matches variable is case-insensitive and must not collide with the accumulator.
  $vsRoot = Join-Path $ProjectRoot "llm-template-test-vs"
  $toolDir = Join-Path $vsRoot "VC\Tools\MSVC\14.0\bin\Hostx64\x64"
  New-Item -ItemType Directory -Path $toolDir -Force | Out-Null
  $expectedMsvcPath = Join-Path $toolDir "dumpbin.exe"
  Set-Content -LiteralPath $expectedMsvcPath -Value "" -Encoding ASCII

  function Get-OverrideValue { param([string]$Name) return $null }
  function Find-ToolInRoots { param([string]$ToolName, [string[]]$Roots, [switch]$Recurse) return $null }
  function Get-VisualStudioRoots { return @($vsRoot) }
  function Get-CommandPath { param([string]$Name) return $null }
  $script:CapturedResults = [System.Collections.Generic.List[object]]::new()
  function Add-Result {
    param(
      [string]$Name,
      [string]$Category,
      [string]$Status,
      [string]$Path = "",
      [string]$Source = "",
      [string]$Notes = ""
    )
    $script:CapturedResults.Add([pscustomobject]@{
      name = $Name
      category = $Category
      status = $Status
      path = $Path
      source = $Source
      notes = $Notes
    }) | Out-Null
  }
  $AdditionalToolRoots = @()

  Resolve-MsvcTool -ToolName "dumpbin.exe"
  $resolvedMsvc = $script:CapturedResults | Where-Object { $_.name -eq "dumpbin.exe" } | Select-Object -Last 1
  if (-not $resolvedMsvc -or $resolvedMsvc.path -ne $expectedMsvcPath -or $resolvedMsvc.source -ne "Visual Studio discovery") {
    throw "Resolve-MsvcTool did not survive regex matching and select the expected MSVC binary."
  }
} finally {
  [Environment]::SetEnvironmentVariable("ProgramFiles", $originalProgramFiles, "Process")
  [Environment]::SetEnvironmentVariable("ProgramFiles(x86)", $originalProgramFilesX86, "Process")
  [Environment]::SetEnvironmentVariable("PROCESSOR_ARCHITECTURE", $originalProcessorArchitecture, "Process")
  [Environment]::SetEnvironmentVariable("LOCALAPPDATA", $originalLocalAppData, "Process")
  if ($overrideFile -and (Test-Path -LiteralPath $overrideFile)) {
    Remove-Item -LiteralPath $overrideFile -Force -ErrorAction SilentlyContinue
  }
  if ($vsRoot -and (Test-Path -LiteralPath $vsRoot)) {
    Remove-Item -LiteralPath $vsRoot -Recurse -Force -ErrorAction SilentlyContinue
  }
  foreach ($testRoot in @($rootA, $rootB, $testLocalAppData)) {
    if ($testRoot -and (Test-Path -LiteralPath $testRoot)) {
      Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
  }
}

Write-Host "Generic debug-tool discovery regression checks passed."
