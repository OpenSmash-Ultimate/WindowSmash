# Compile and run a known 13.0.5 main-module function against prepared local inputs (runtime/smoke.c).
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))   # the WindowSmash folder (this is tools\dev\)
$tools = Join-Path $here 'tools'
$exefs = Join-Path $here 'dump\exefs'
$build = Join-Path $here 'build'
$image = Join-Path $here 'dump\modules\main.reloc.bin'
if (-not (Test-Path -LiteralPath (Join-Path $exefs 'main'))) {
    throw 'Prepare dump/exefs/main first. See docs/BUILDING.md.'
}
$clang = python -c "import sys; sys.path.insert(0, sys.argv[1]); import cc; print(cc.CLANG)" $tools
if ($LASTEXITCODE) { throw 'Could not locate clang-cl through tools/cc.py.' }
New-Item -ItemType Directory -Force $build, (Split-Path -Parent $image) | Out-Null
if (-not (Test-Path -LiteralPath $image)) {
    python (Join-Path $tools 'recomp.py') image (Join-Path $exefs 'main') --out $image
    if ($LASTEXITCODE) { throw 'Module image generation failed.' }
}
python (Join-Path $tools 'recomp.py') emit (Join-Path $exefs 'main') --roots 25ee6c0 --out (Join-Path $build 'smoke_gen.c')
if ($LASTEXITCODE) { throw 'C emission failed.' }
& $clang /nologo /O2 /arch:AVX2 -mfma -fno-strict-aliasing /clang:-ffp-contract=off `
    -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-label -Wno-parentheses-equality `
    /I (Join-Path $here 'runtime') (Join-Path $build 'smoke_gen.c') (Join-Path $here 'runtime\smoke.c') `
    "/Fo$build/" "/Fe$build/smoke.exe"
if ($LASTEXITCODE) { throw "Compile failed ($LASTEXITCODE)." }
& (Join-Path $build 'smoke.exe') $image
exit $LASTEXITCODE
