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
# zimg's pkg-config file names libstdc++ as a private library by default, and a
# static link reads it: `link` then stops at LNK1181 on stdc++.lib. MSVC has no
# such library and its own C++ runtime links on its own, so the name is removed.
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

foreach ($file in "$prefix/lib/pkgconfig/zimg.pc", "$prefix/debug/lib/pkgconfig/zimg.pc") {
    if (Test-Path $file) {
        $text = (Get-Content -Raw $file) -replace '-lstdc\+\+', ''
        Set-Content -NoNewline -Path $file -Value $text
    }
}

$env:PKG_CONFIG_PATH = $searchPath
& $pkgconf --exists 'zimg >= 2.7'
if ($LASTEXITCODE -ne 0) {
    throw "$pkgconf cannot resolve zimg >= 2.7 from $pcDir (exit $LASTEXITCODE)"
}
$staticLibs = & $pkgconf --static --libs zimg
if ($LASTEXITCODE -ne 0 -or "$staticLibs" -match 'stdc\+\+') {
    throw "zimg.pc still asks a static link for libstdc++: $staticLibs"
}
Write-Host "zimg $(& $pkgconf --modversion zimg) through $pkgconf"

Add-Content -Path $env:GITHUB_ENV -Value "PKG_CONFIG=$pkgconf"
Add-Content -Path $env:GITHUB_ENV -Value "PKG_CONFIG_PATH=$searchPath"
