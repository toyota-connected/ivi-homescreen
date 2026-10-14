# Cross-building Mesa for a board

For when a board's distro Mesa is too old for what you need to test — a Vulkan
driver that only recently gained support for its GPU, a bisect against a driver
bug, or a capability that a newer Mesa advertises and the shipped one does not.

Everything here installs under one prefix and is selected per-process by
environment variable, so the board's own graphics stack stays the default and
nothing is replaced. `rm -rf` the prefix to revert.

## Do not build it on the board

Mesa's Vulkan drivers pull in its OpenCL-C compiler, which pulls in LLVM:
`with_driver_using_cl` is true for several drivers (panfrost, intel, asahi,
nouveau, imagination, rusticl), and `with_clc` then forces the `llvm` feature on
with `CLC requires LLVM`. There is no opt-out — `-Dmesa-clc=auto` still enables
it, and `-Dllvm=disabled` is refused.

On a 4-core board with 4 GB of RAM that means ~1 GB of LLVM dev packages and an
hours-long build that can run out of memory while linking.

**Cross-building removes the LLVM dependency from the target build entirely.**
With `-Dmesa-clc=system`, Mesa's own logic becomes:

```meson
if get_option('mesa-clc') == 'system'
  prog_mesa_clc = find_program('mesa_clc', native : true)
  prog_vtn_bindgen2 = find_program('vtn_bindgen2', native : true)
  with_clc = with_gallium_rusticl      # false unless you asked for rusticl
```

so `with_clc` is false, `llvm` is no longer forced, and LLVM is needed only on
the build host for a short native stage. That single option is the difference
between minutes and hours.

## Host prerequisites

Mesa pins `LLVMSPIRVLib` to the **exact LLVM minor**: `>= 15.0.0.0`, plus
`>= <major>.<minor>` and `< <major>.<minor+1>` of whichever LLVM is found. A host
whose SPIRV-LLVM-Translator lags its LLVM cannot build the native stage, so check
this first.

On Fedora, with LLVM 22.1:

```sh
sudo dnf install -y spirv-llvm-translator-devel spirv-tools-devel
pkg-config --modversion LLVMSPIRVLib   # must land inside Mesa's window
llvm-config --version
```

`meson >= 1.4` is also required. Where the distro is older, a venv is enough:
`python3 -m venv ~/mesa-venv && ~/mesa-venv/bin/pip install meson`.

## Pick a sysroot whose glibc matches the board

A binary built against a **newer** glibc than the board has will not run, so the
sysroot's glibc must be equal to or older than the board's. `emb` resolves both
the toolchain and the sysroot:

```sh
emb cross . --list-targets
emb cross . --target radxa-zero3      # prints cc, cxx and target sysroot
```

Mind the toolchain pairing as well as the glibc: the bookworm targets use
arm-gnu 12.3 while the trixie ones use 15.2, and the newer toolchain's
`libstdc++` references glibc symbols an older target does not have — which
breaks C++ linking rather than failing at configure.

Check what the sysroot actually provides before configuring. Anything missing has
to be disabled or built: `libdrm` must clear the driver's floor (panfrost wants
2.4.109), and `expat`/`zstd` are commonly absent.

## Stage 1: the native tools

**The native stage must enable the same driver as the cross stage.** `mesa_clc`
and `vtn_bindgen2` build with no drivers selected, but the precompiler is
per-driver — `panfrost_compile` only appears with `-Dvulkan-drivers=panfrost`,
and the cross configure fails on `Program 'panfrost_compile' not found`.

```sh
meson setup native-build --prefix=$PWD/native -Dbuildtype=release \
  -Dmesa-clc=enabled -Dinstall-mesa-clc=true \
  -Dprecomp-compiler=enabled -Dinstall-precomp-compiler=true \
  -Dvulkan-drivers=panfrost \
  -Dgallium-drivers= -Dplatforms= -Dglx=disabled -Degl=disabled \
  -Dgles1=disabled -Dgles2=disabled -Dopengl=false
ninja -C native-build install
ls native/bin        # mesa_clc  panfrost_compile  vtn_bindgen2
```

This is the stage that needs host LLVM, and it is native x86, so it is quick.

## The cross file

```ini
[binaries]
c = '<toolchain>/bin/aarch64-none-linux-gnu-gcc'
cpp = '<toolchain>/bin/aarch64-none-linux-gnu-g++'
ar = '<toolchain>/bin/aarch64-none-linux-gnu-ar'
strip = '<toolchain>/bin/aarch64-none-linux-gnu-strip'
pkg-config = 'pkg-config'

[built_in options]
c_args = ['--sysroot=<sysroot>']
cpp_args = ['--sysroot=<sysroot>']
c_link_args = ['--sysroot=<sysroot>']
cpp_link_args = ['--sysroot=<sysroot>']

[properties]
sys_root = '<sysroot>'
pkg_config_libdir = '<sysroot>/usr/lib/aarch64-linux-gnu/pkgconfig:<sysroot>/usr/share/pkgconfig:<sysroot>/usr/lib/pkgconfig'

[host_machine]
system = 'linux'
kernel = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
```

`sys_root` alone is not enough: `pkg_config_libdir` is what makes the sysroot's
`.pc` files resolve instead of the host's, and `--sysroot` is needed in all four
of the args and link_args entries.

## Stage 2: the cross build

With the native tools on `PATH`:

```sh
PATH=$PWD/native/bin:$PATH meson setup build --cross-file cross.ini \
  --prefix=/opt/mesa-main -Dbuildtype=release \
  -Dmesa-clc=system -Dprecomp-compiler=system \
  -Dgallium-drivers= -Dvulkan-drivers=panfrost -Dplatforms= \
  -Dglx=disabled -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false \
  -Dllvm=disabled -Dexpat=disabled -Dxmlconfig=disabled -Dzstd=disabled \
  -Dpanfrost-rust=false -Dvideo-codecs=
ninja -C build
```

`-Dplatforms=` empty gives a surfaceless build, which is all a capability probe
needs and avoids the wayland/X11 dependency set. Add `-Dplatforms=wayland` and
re-enable EGL/GLES when you want the GL driver too.

`-Dpanfrost-rust=false` is the default and worth passing explicitly: the Rust
components need Rust 1.85, bindgen and meson 1.7, which few board distros carry.

## Deploy and select it

The generated ICD JSON hardcodes an absolute `library_path` from the prefix, and
it is plain `lib/` rather than `lib/<multiarch>/`. Put the library exactly where
the JSON says or edit the JSON:

```sh
scp build/src/panfrost/vulkan/libvulkan_panfrost.so        board:/tmp/
scp build/src/panfrost/vulkan/panfrost_icd.aarch64.json    board:/tmp/
# on the board
sudo mkdir -p /opt/mesa-main/lib /opt/mesa-main/share/vulkan/icd.d
sudo cp /tmp/libvulkan_panfrost.so     /opt/mesa-main/lib/
sudo cp /tmp/panfrost_icd.aarch64.json /opt/mesa-main/share/vulkan/icd.d/
```

Then select it for one process only:

```sh
VK_DRIVER_FILES=/opt/mesa-main/share/vulkan/icd.d/panfrost_icd.aarch64.json \
  vulkaninfo --summary
```

Confirm it will load before chasing runtime errors — the library must not need a
newer glibc or C++ ABI than the board has:

```sh
readelf -V libvulkan_panfrost.so | grep -oE "GLIBC_2\.[0-9]+" | sort -uV | tail -1
readelf -V libvulkan_panfrost.so | grep -oE "(GLIBCXX|CXXABI)_[0-9.]+" | sort -uV | tail -1
readelf -d libvulkan_panfrost.so | grep NEEDED
```

## Worked example: panvk on a Radxa Zero 3

RK3566, Mali-G52, Debian bookworm with glibc 2.36, stock Mesa 24.2 and no panvk
ICD at all — `vulkaninfo` reported only llvmpipe.

Cross-built Mesa 26.3.0-devel by the recipe above and deployed just
`libvulkan_panfrost.so`. The library needed at most GLIBC_2.34 and CXXABI_1.3,
against the board's 2.36 and GLIBCXX 3.4.30, so it loaded as built. With
`VK_DRIVER_FILES` set:

```
deviceName   = Mali-G52 r1 MC1          (panfrost arch v7)
driverName   = panvk
driverInfo   = Mesa 26.3.0-devel
apiVersion   = 1.3.363
```

162 device extensions, including `VK_KHR_external_semaphore_fd`,
`VK_EXT_external_memory_dma_buf` and `VK_EXT_image_drm_format_modifier`.
`drm_kms_vulkan_probe /dev/dri/card0` then reported `ZERO-COPY GATE: PASS`.

Two things to know about that result. panvk gates this GPU generation behind
`PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1` and reports `conformanceVersion 0.0.0.0`,
so it is a testing driver and not a shipping one. And a capability probe is a
precondition, not an end-to-end run.

## Gotchas, each of which cost a round trip

- The native stage needs the **same driver** enabled as the cross stage, for the
  per-driver precompiler.
- `-Dllvm=disabled` is refused unless `-Dmesa-clc=system`.
- `LLVMSPIRVLib` is pinned to the exact LLVM minor on the host.
- The ICD JSON's `library_path` is absolute and uses plain `lib/`.
- A Debian-style sysroot needs `-I<sysroot>/usr/include/<multiarch>` or the
  compile dies on `bits/wordsize.h`, and `-B<sysroot>/usr/lib/<multiarch>` or the
  link cannot find `crt1.o`. Both apply to anything hand-compiled against one,
  not just Mesa.
