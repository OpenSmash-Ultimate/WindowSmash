# Third-party sources

The two codec libraries the game needs are built from these sources during the game build
(`tools/codec_libs.py`, called from `tools/cc.py` when the program is linked), so music and
movies work without anything to download or set up. Their license texts are here too
([notices](../NOTICE.md)). Every file is a verbatim copy from the upstream release named below,
except for the one line listed under [Local changes](#local-changes), and the selection can be
checked against that release's archive.

| Library | Version | Upstream archive | SHA-256 of the archive |
| :--- | :--- | :--- | :--- |
| libopus | 1.5.2 | https://github.com/xiph/opus/releases/download/v1.5.2/opus-1.5.2.tar.gz | `65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1` |
| libvpx | 1.15.2 | https://github.com/webmproject/libvpx/archive/refs/tags/v1.15.2.tar.gz | `26fcd3db88045dee380e581862a6ef106f49b74b6396ee95c2993a260b4636aa` |

## opus/

The sources that opus's own build lists for a floating-point x86 build with its SSE, SSE4.1 and
AVX2 intrinsics (`celt_sources.mk`: `CELT_SOURCES`, `_X86_RTCD`, `_SSE`, `_SSE2`, `_SSE4_1`,
`_AVX2`; `silk_sources.mk`: `SILK_SOURCES`, `_X86_RTCD`, `_SSE4_1`, `_AVX2`, `_FLOAT`,
`_FLOAT_AVX2`; `opus_sources.mk`: `OPUS_SOURCES`, `_FLOAT`), plus the headers they include (found
by compiling each file with `clang-cl /showIncludes`), `COPYING` and `AUTHORS`. Left out: the
fixed-point and ARM/MIPS code, the `dnn/` neural-network extensions (not needed to decode), the
tests, docs and build system.

`codec_libs.py` compiles them with the defines opus's build system would set for this
configuration: `OPUS_BUILD`, `USE_ALLOCA`, `HAVE_LRINT`, `HAVE_LRINTF`, `PACKAGE_VERSION`, and
`OPUS_X86_MAY_HAVE_*` with `OPUS_X86_PRESUME_*` for SSE, SSE2, SSE4.1 and AVX2 (the whole program
is built for AVX2, so there is nothing to detect at run time).

## libvpx/

The VP9 decoder only, in plain C: the `.c` files that libvpx's makefiles select for this
configuration (`vp9/vp9_common.mk`, `vp9/vp9dx.mk`, `vpx_dsp/vpx_dsp.mk`, `vpx_scale/vpx_scale.mk`,
`vpx_mem/vpx_mem.mk`, `vpx_ports/vpx_ports.mk`, `vpx_util/vpx_util.mk`, `vpx/vpx_codec.mk`), plus
the headers they include, `LICENSE`, `PATENTS` and `AUTHORS`. No assembly: the `.asm` files need
nasm, and the intrinsics files depend on symbols from them. Measured on the game's 1280x720 title
movie, the plain C decoder manages about 570 frames a second (four threads), the assembly about
1,100; the game shows 30.

`config/` holds what libvpx's `configure` and `make` generate for this configuration, so that no
configure run is needed:

```sh
./configure --target=x86_64-win64-vs17 --disable-vp8 --disable-vp9-encoder --enable-vp9-decoder \
    --disable-examples --disable-tools --disable-docs --disable-unit-tests --disable-install-docs \
    --disable-install-bins --disable-install-srcs --disable-webm-io --disable-libyuv \
    --disable-postproc --disable-vp9-postproc --disable-runtime-cpu-detect --disable-mmx \
    --disable-sse --disable-sse2 --disable-sse3 --disable-ssse3 --disable-sse4_1 --disable-avx \
    --disable-avx2 --disable-avx512
```

- `vpx_config.h`: as written by `configure` (which needs nasm or yasm on the PATH even for this
  assembly-free configuration).
- `vp9_rtcd.h`, `vpx_dsp_rtcd.h`, `vpx_scale_rtcd.h`: `build/make/rtcd.pl --arch=x86_64
  --sym=<name> --config=libs-x86_64-win64-vs17.mk --disable-avx512 <defs .pl>`, as the makefiles
  run it (`vpx_dsp/vpx_dsp_rtcd_defs.pl`, `vp9/common/vp9_rtcd_defs.pl`,
  `vpx_scale/vpx_scale_rtcd.pl`).
- `vpx_version.h`: `build/make/version.sh <source dir> vpx_version.h`.

`config-arm64/` is the same for the macOS build (arm64, plain C): see its `README.md`. On arm64,
`tools/codec_libs.py` (`build_static`) compiles opus without the `x86` folders and the
`OPUS_X86_*` defines, and libvpx with `config-arm64/` in place of `config/`.

## Local changes

- `opus/silk/x86/NSQ_del_dec_avx2.c`: `#ifdef _MSC_VER` became
  `#if defined(_MSC_VER) && !defined(__clang__)`. Under that test the file defines its own
  `__builtin_sadd_overflow` and `__builtin_ctz` for Microsoft's compiler, which lacks them.
  clang-cl also defines `_MSC_VER` but has both built in, and older clang-cl versions refuse a
  definition of a built-in function ("definition of builtin function '__builtin_ctz'"), so the
  build stopped there for players whose Visual Studio came with one. Upstream opus still has
  `#ifdef _MSC_VER` (checked against its main branch, October 2026). When you update opus,
  apply this change again unless upstream has made it.

## Updating a library

Take the new release's archive, repeat the selection above (the file lists come from the
makefiles; the header set from `clang-cl /showIncludes` over the selected sources), redo the
[local changes](#local-changes) the new release still needs, regenerate
`libvpx/config/` and `libvpx/config-arm64/` (its `README.md` says how), replace the folder, update
the table above and the versions in `tools/codec_libs.py` (`PACKAGE_VERSION`) and `NOTICE.md`, then
build: `python tools/codec_libs.py --force` compiles both in a few seconds, and
`tests/test_codec_libs.py` checks the layout.
