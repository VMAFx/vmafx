# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# Install the CUDA toolkit on a Windows CI runner from NVIDIA's redistributable
# archives, reading the release from build-config.env so the Windows legs share
# the single coordinated pin (ADR-1285) with every other site.
#
# Why not the network installer. Until CUDA 13.4 the Windows legs ran
# `cuda_<version>_windows_network.exe -s nvcc_13.3 cudart_13.3 ...`. NVIDIA
# published no such installer for 13.4: verified 2026-09-23, every shape of that
# path 404s --
#   compute/cuda/13.4.1/network_installers/cuda_13.4.1_windows_network.exe
#   compute/cuda/13.4.0/network_installers/cuda_13.4.0_windows_network.exe
#   compute/cuda/13.4.1/local_installers/cuda_13.4.1_windows.exe
# while the 13.3.1 network installer still returns 200. The release itself is
# not missing on Windows: redistrib_13.4.1.json lists windows-x86_64 (and
# windows-arm64) artifacts for every component this build needs. So the
# componentised distribution is the one NVIDIA actually ships for 13.4, and it
# is also the same distribution the Linux legs install from.
#
# The manifest is the source of the component versions, which are NOT the
# release version -- 13.4.1 ships cuda_nvcc 13.4.59 and cuda_cudart 13.4.49.
# Resolving them at run time keeps those numbers out of the workflows, where
# check-cuda-pin-lockstep.py would have to learn a seventh spelling that is not
# a pin at all.
#
# Usage: pwsh -File scripts/ci/install-cuda-toolkit.ps1 [-RepoRoot <path>]

[CmdletBinding()]
param(
  [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..' '..')).Path
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$config = Join-Path $RepoRoot 'build-config.env'
if (-not (Test-Path $config)) {
  throw "install-cuda-toolkit: $config not found"
}

# build-config.env is shell syntax, but the CUDA pin is a plain NAME="value"
# line. Parse just that rather than shelling out to bash, which the Windows
# runners do not have on PATH by default.
$cudaVersion = $null
foreach ($line in Get-Content -LiteralPath $config) {
  if ($line -match '^\s*CUDA_VERSION\s*=\s*"?([0-9]+\.[0-9]+\.[0-9]+)"?') {
    $cudaVersion = $Matches[1]
    break
  }
}
if (-not $cudaVersion) {
  throw "install-cuda-toolkit: CUDA_VERSION is not set in $config"
}
$cudaMajorMinor = ($cudaVersion -split '\.')[0, 1] -join '.'
$cudaPathVar = 'CUDA_PATH_V' + (($cudaVersion -split '\.')[0, 1] -join '_')

$base = 'https://developer.download.nvidia.com/compute/cuda/redist'
$manifestUrl = "$base/redistrib_$cudaVersion.json"
$manifestFile = Join-Path $env:RUNNER_TEMP "redistrib_$cudaVersion.json"

Write-Host "install-cuda-toolkit: CUDA $cudaVersion from $manifestUrl"
curl.exe --fail --location --silent --show-error --retry 5 --retry-delay 10 `
  --output $manifestFile $manifestUrl
if ($LASTEXITCODE -ne 0) {
  throw "install-cuda-toolkit: no redistrib manifest for CUDA $cudaVersion at $manifestUrl"
}
$manifest = Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json

# The same subset the network installer was asked for: nvcc, the runtime
# headers and import library, crt/host_config.h, cicc.exe + libdevice (which
# live in libnvvm, not cuda_nvcc), and the MSBuild integration.
$components = @(
  'cuda_nvcc',
  'cuda_cudart',
  'cuda_crt',
  'libnvvm',
  'visual_studio_integration'
)

$cudaPath = "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v$cudaMajorMinor"
New-Item -ItemType Directory -Force -Path $cudaPath | Out-Null

foreach ($component in $components) {
  $entry = $manifest.$component
  if (-not $entry) {
    throw "install-cuda-toolkit: CUDA $cudaVersion manifest has no component '$component'"
  }
  $platform = $entry.'windows-x86_64'
  if (-not $platform) {
    throw "install-cuda-toolkit: '$component' ships no windows-x86_64 artifact in CUDA $cudaVersion"
  }

  $relative = $platform.relative_path
  $archive = Join-Path $env:RUNNER_TEMP (Split-Path -Leaf $relative)
  Write-Host "  $component $($entry.version) -> $(Split-Path -Leaf $relative)"
  curl.exe --fail --location --silent --show-error --retry 5 --retry-delay 10 `
    --output $archive "$base/$relative"
  if ($LASTEXITCODE -ne 0) {
    throw "install-cuda-toolkit: download failed for $base/$relative"
  }

  # Every archive wraps its payload in a single
  # <component>-windows-x86_64-<version>-archive/ directory whose children are
  # the toolkit layout (bin/, include/, lib/, nvvm/). Merging the children --
  # not the wrapper -- is what reconstructs the layout nvcc expects to find
  # beside itself.
  $staging = Join-Path $env:RUNNER_TEMP "extract-$component"
  if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
  Expand-Archive -LiteralPath $archive -DestinationPath $staging -Force

  $roots = @(Get-ChildItem -LiteralPath $staging -Directory)
  if ($roots.Count -ne 1) {
    throw "install-cuda-toolkit: expected one root directory in $archive, found $($roots.Count)"
  }
  Copy-Item -Path (Join-Path $roots[0].FullName '*') -Destination $cudaPath -Recurse -Force
}

# Fail by name rather than leaving a later compile to fail on a missing tool.
foreach ($required in @('bin\nvcc.exe', 'nvvm\bin\cicc.exe', 'nvvm\libdevice\libdevice.10.bc')) {
  if (-not (Test-Path (Join-Path $cudaPath $required))) {
    throw "install-cuda-toolkit: $required is missing under $cudaPath"
  }
}

"CUDA_PATH=$cudaPath" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
"$cudaPathVar=$cudaPath" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
(Join-Path $cudaPath 'bin') | Out-File -FilePath $env:GITHUB_PATH -Encoding utf8 -Append

& (Join-Path $cudaPath 'bin\nvcc.exe') --version
