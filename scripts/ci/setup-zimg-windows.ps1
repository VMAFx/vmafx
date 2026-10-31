# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# Install zimg and a working pkg-config on a Windows CI runner, and export both
# to the steps that follow (ADR-3396).
#
# The default build needs zimg, and Meson finds it through pkg-config. The only
# pkg-config on a hosted runner is Strawberry Perl's, which Meson refuses as
# broken, so the job installs pkgconf from vcpkg and names it in PKG_CONFIG.
#
# The triplet must match the C runtime of the build, because a static zimg
# carries that runtime into the link: `x64-windows-static-md` for /MD, which is
# what a release buildtype selects, and `x64-windows-static` for -Db_vscrt=mt.
#
# Usage: pwsh -File scripts/ci/setup-zimg-windows.ps1 -Triplet <vcpkg triplet>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Triplet
)

$ErrorActionPreference = 'Stop'

& vcpkg install "zimg:$Triplet" "pkgconf:$Triplet"
if ($LASTEXITCODE -ne 0) {
    throw "vcpkg install failed for $Triplet (exit $LASTEXITCODE)"
}

$prefix = "$env:VCPKG_INSTALLATION_ROOT/installed/$Triplet" -replace '\\', '/'
$pkgconf = "$prefix/tools/pkgconf/pkgconf.exe"
$pcDir = "$prefix/lib/pkgconfig"
$searchPath = if ($env:PKG_CONFIG_PATH) { "$pcDir;$env:PKG_CONFIG_PATH" } else { $pcDir }

$env:PKG_CONFIG_PATH = $searchPath
& $pkgconf --exists 'zimg >= 2.7'
if ($LASTEXITCODE -ne 0) {
    throw "$pkgconf cannot resolve zimg >= 2.7 from $pcDir (exit $LASTEXITCODE)"
}
Write-Host "zimg $(& $pkgconf --modversion zimg) through $pkgconf"

Add-Content -Path $env:GITHUB_ENV -Value "PKG_CONFIG=$pkgconf"
Add-Content -Path $env:GITHUB_ENV -Value "PKG_CONFIG_PATH=$searchPath"
