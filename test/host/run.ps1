$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe = Join-Path $PSScriptRoot 'run_all.exe'
# _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH: installed clang 18 vs MSVC STL wanting 19.
clang++ -std=c++17 -O1 -Wall -Wextra -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH `
  -I"$root/lib/core" -I"$PSScriptRoot" "$PSScriptRoot/test_all.cpp" -o $exe
if ($LASTEXITCODE -ne 0) { exit 1 }
& $exe
exit $LASTEXITCODE
