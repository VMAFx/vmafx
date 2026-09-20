# Copyright 2026 Lusoris
# Copyright 2026 Claude (Anthropic)
# SPDX-License-Identifier: EUPL-1.2
# Install the repository's canonical CUDA compiler/runtime subset on Windows CI.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("x86_64", "arm64")]
    [string]$Architecture
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$configPath = Join-Path $repoRoot "build-config.env"
$versionLines = Select-String -Path $configPath -Pattern '^CUDA_VERSION="([0-9]+\.[0-9]+\.[0-9]+)"$'
if ($versionLines.Count -ne 1) {
    throw "build-config.env must define exactly one three-part CUDA_VERSION"
}
$cudaVersion = $versionLines.Matches[0].Groups[1].Value
$parts = $cudaVersion.Split(".")
$cudaMajorMinor = "$($parts[0]).$($parts[1])"
$installerName = "cuda_${cudaVersion}_windows_${Architecture}_network.exe"
$baseUrl = "https://developer.download.nvidia.com/compute/cuda/$cudaVersion"
$installerUrl = "$baseUrl/network_installers/$installerName"
$checksumUrl = "$baseUrl/docs/sidebar/md5sum.txt"
$installer = Join-Path $env:RUNNER_TEMP $installerName
$checksumFile = Join-Path $env:RUNNER_TEMP "cuda-$cudaVersion-md5sum.txt"

try {
    curl.exe --fail --location --retry 5 --retry-delay 10 --output $installer $installerUrl
    curl.exe --fail --location --retry 5 --retry-delay 10 --output $checksumFile $checksumUrl
    $checksumLine = Get-Content $checksumFile | Where-Object { $_ -match " $([regex]::Escape($installerName))$" }
    if ($checksumLine.Count -ne 1) {
        throw "NVIDIA checksum manifest does not contain $installerName"
    }
    $expectedHash = ($checksumLine -split '\s+')[0].ToLowerInvariant()
    $actualHash = (Get-FileHash -Path $installer -Algorithm MD5).Hash.ToLowerInvariant()
    if ($actualHash -ne $expectedHash) {
        throw "CUDA installer checksum mismatch: expected $expectedHash, got $actualHash"
    }

    $signature = Get-AuthenticodeSignature -FilePath $installer
    if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
        throw "CUDA installer Authenticode signature is not valid: $($signature.Status)"
    }
    if ($null -eq $signature.SignerCertificate) {
        throw "CUDA installer Authenticode signature has no signer certificate"
    }
    $signerSubject = $signature.SignerCertificate.Subject
    if ($signerSubject -notmatch "(?i)NVIDIA") {
        throw "CUDA installer Authenticode signer is not NVIDIA: $signerSubject"
    }

    $packages = @(
        "nvcc_$cudaMajorMinor",
        "cudart_$cudaMajorMinor",
        "crt_$cudaMajorMinor",
        "nvvm_$cudaMajorMinor",
        "visual_studio_integration_$cudaMajorMinor"
    )
    $process = Start-Process -FilePath $installer -ArgumentList (@("-s") + $packages) -Wait -PassThru
    if ($process.ExitCode -ne 0) {
        throw "CUDA installer failed with exit code $($process.ExitCode)"
    }
} finally {
    Remove-Item -Path $installer, $checksumFile -Force -ErrorAction SilentlyContinue
}

$cudaPath = "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v$cudaMajorMinor"
$nvcc = Join-Path $cudaPath "bin\nvcc.exe"
if (-not (Test-Path $nvcc)) {
    throw "nvcc.exe was not installed under $cudaPath"
}
$versionVariable = "CUDA_PATH_V$($cudaMajorMinor.Replace('.', '_'))"
if ($env:GITHUB_ENV) {
    "CUDA_PATH=$cudaPath" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
    "$versionVariable=$cudaPath" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
}
if ($env:GITHUB_PATH) {
    (Join-Path $cudaPath "bin") | Out-File -FilePath $env:GITHUB_PATH -Encoding utf8 -Append
}
& $nvcc --version
