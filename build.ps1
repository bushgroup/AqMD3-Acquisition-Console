param(
    [string]$Config = "Release",
    [string]$VcpkgToolchain,
    [string]$Triplet = "x64-windows-clockwork"
)

if (-not $VcpkgToolchain) {
    $VcpkgToolchain = if ($env:VCPKG_TOOLCHAIN_FILE) { $env:VCPKG_TOOLCHAIN_FILE } else { "C:/vcpkg/scripts/buildsystems/vcpkg.cmake" }
}

$OverlayTriplets = Join-Path $PSScriptRoot "vcpkg-triplets"

Write-Host "Building $Config configuration for triplet $Triplet..." -ForegroundColor Cyan

cmake -B "build/$Config" -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=$Config -DCMAKE_TOOLCHAIN_FILE="$VcpkgToolchain" -DVCPKG_TARGET_TRIPLET="$Triplet" -DVCPKG_OVERLAY_TRIPLETS="$OverlayTriplets"
cmake --build "build/$Config" --config $Config

$OutputPath = "$PWD\build\$Config\AqMD3_console\$Config\"
Write-Host "Application build output folder $OutputPath" -ForegroundColor Cyan
