# build-tools — meson helpers that produce inputs other parts of the
# build consume. Subdirs by component:
#
#   freebsd/    fetches FreeBSD source + curates a UAPI tree; output
#               is the GUEST sysroot reference (wasm32 i386 view).
#               extract.py walks it to fill the guest_* sections of
#               abi.yaml.
#
#   host-libc/  snapshots the build host's libc include search path
#               (glibc on Linux, libSystem on Darwin, FreeBSD libc on
#               FreeBSD). extract.py walks this as the HOST ABI
#               reference — yos's host runtime calls host libc and
#               must see its struct shapes for the converter.

subdir('freebsd')
subdir('host-libc')
# wasm3 lives under src/wasm3/ (vendored source); its meson.build is
# included from src/meson.build BEFORE subdir('yos').
subdir('mimalloc')      # host-side allocator; backs guest malloc/free
subdir('freebsd-libc')  # Tier 2 sidecar wasm (libc-pure.wasm)
subdir('sysroot')       # wasm sysroot for guest .c compilation
