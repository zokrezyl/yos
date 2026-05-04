# yos — Makefile wrapper around meson/ninja.
#
# All real build logic lives in meson; this file just exposes the
# meaningful targets behind short, memorable names. Every recipe runs
# inside `nix develop` so the wasm cross-compile shim, apple-sdk_13,
# and matching wasm-ld are on PATH.
#
# Run `make help` for the menu.

# ─── Configuration ─────────────────────────────────────────────────────

# Build directory. Pick per-host so darwin and linux can co-exist on the
# same checkout without stomping each other's compile_commands.json.
UNAME_S  := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
BUILD    ?= build-darwin
else
BUILD    ?= build-linux
endif

# Optional ENV. Override on the command line as `make BUILD=foo …`.
JOBS     ?= $(shell command -v sysctl >/dev/null 2>&1 && sysctl -n hw.ncpu || nproc)

# All recipes go through `nix develop --command` so the toolchain shims
# (wasm-clang, lld, apple-sdk_13, …) are on PATH. Override with
# `make NIX_DEV=` to run with whatever is currently on the user's PATH.
NIX_DEV  ?= nix develop $(CURDIR) --command

MESON    := $(NIX_DEV) meson
NINJA    := $(NIX_DEV) ninja -C $(BUILD)

# ─── Phony bookkeeping ─────────────────────────────────────────────────

.PHONY: help all setup reconfigure clean distclean wipe \
        bootstrap toolchain mimalloc sysroot \
        codegen api api-compare api-analyse api-generate api-bridge api-struct-convert \
        libs wasm3 yos-api-bridge \
        libc-pure yos run \
        test test-libc test-freebsd test-list testlog \
        nvim-build nvim-deps \
        format check

.DEFAULT_GOAL := help

# ─── Help ──────────────────────────────────────────────────────────────

help:
	@printf '\nyos build targets (BUILD=$(BUILD), JOBS=$(JOBS))\n\n'
	@printf '  Top-level\n'
	@printf '    make all            — host yos + libc-pure.wasm (default deliverable)\n'
	@printf '    make yos            — just the host executable\n'
	@printf '    make libc-pure      — just build-tools/freebsd-libc/libc-pure.wasm\n'
	@printf '    make run ARGS=…     — run the host yos binary (forwards ARGS)\n'
	@printf '\n  Configuration / lifecycle\n'
	@printf '    make setup          — initial meson setup (no-op if BUILD/ exists)\n'
	@printf '    make reconfigure    — meson setup --reconfigure (re-evaluates flake)\n'
	@printf '    make clean          — delete build artifacts but keep config\n'
	@printf '    make distclean      — wipe BUILD/ entirely\n'
	@printf '    make wipe           — alias for distclean + clean fetched deps\n'
	@printf '\n  Bootstrap (one-time fetches; idempotent)\n'
	@printf '    make bootstrap      — freebsd source + headers + mimalloc + sysroot\n'
	@printf '    make toolchain      — alias for bootstrap (legacy name)\n'
	@printf '    make mimalloc       — fetch + build mimalloc only\n'
	@printf '    make sysroot        — assemble guest sysroot from freebsd headers\n'
	@printf '\n  Codegen (api-extract → compare → analyse → generate → bridge)\n'
	@printf '    make codegen        — run the whole pipeline (= make api)\n'
	@printf '    make api            — alias for codegen\n'
	@printf '    make api-compare    — guest vs host signature compare\n'
	@printf '    make api-analyse    — bucket each call into mechanical/policy/etc.\n'
	@printf '    make api-generate   — emit yos_remap.{c,h}\n'
	@printf '    make api-bridge     — emit yos_bridge.{c,h}\n'
	@printf '    make api-struct-convert — emit yos_struct_convert.{c,h}\n'
	@printf '\n  Static libs\n'
	@printf '    make wasm3          — libwasm3.a\n'
	@printf '    make yos-api-bridge — libyos_api_bridge.a (depends on codegen)\n'
	@printf '    make libs           — all of the above\n'
	@printf '\n  Tests (depend on yos + libc-pure.wasm)\n'
	@printf '    make test           — full test suite\n'
	@printf '    make test-libc      — yos:libc tests only (ut/libc/test_*.c)\n'
	@printf '    make test-freebsd   — yos:freebsd-libc tests only (FreeBSD libc/tests)\n'
	@printf '    make test-list      — list available test names\n'
	@printf '    make testlog        — tail meson-logs/testlog.txt\n'
	@printf '\n  External wasm packages\n'
	@printf '    make nvim-build     — neovim 0.10.4 → wasm32 (needs nvim-deps first)\n'
	@printf '    make nvim-deps      — fetch + build all 9 nvim deps\n'
	@printf '\n  Misc\n'
	@printf '    make format         — clang-format on src/yos\n'
	@printf '    make check          — quick compile-only sanity (no tests)\n'
	@printf '\n'

# ─── Setup / reconfigure ───────────────────────────────────────────────

$(BUILD)/build.ninja:
	@$(MESON) setup $(BUILD)

setup: $(BUILD)/build.ninja

reconfigure:
	@$(MESON) setup --reconfigure $(BUILD)

clean: setup
	@$(NINJA) clean

distclean:
	rm -rf $(BUILD)

wipe: distclean
	rm -rf $(BUILD)-darwin $(BUILD)-linux build build-darwin build-linux

# ─── Top-level deliverable ─────────────────────────────────────────────

all: yos libc-pure

# ─── Bootstrap ─────────────────────────────────────────────────────────

bootstrap toolchain: setup
	@$(NINJA) freebsd-src-all freebsd-headers host-libc-headers \
	          build-tools/mimalloc/mimalloc.stamp build-tools/sysroot/sysroot.stamp

mimalloc: setup
	@$(NINJA) build-tools/mimalloc/mimalloc.stamp build-tools/mimalloc/libmimalloc.a

sysroot: setup
	@$(NINJA) build-tools/sysroot/sysroot.stamp

# ─── Codegen ───────────────────────────────────────────────────────────

codegen api: setup
	@$(NINJA) api

api-compare: setup
	@$(NINJA) api-compare

api-analyse: setup
	@$(NINJA) api-analyse

api-generate: setup
	@$(NINJA) api-generate

api-bridge: setup
	@$(NINJA) api-bridge

api-struct-convert: setup
	@$(NINJA) api-struct-convert

# ─── Static libs ───────────────────────────────────────────────────────

libs: wasm3 yos-api-bridge mimalloc

wasm3: setup
	@$(NINJA) src/wasm3/libwasm3.a

yos-api-bridge: setup
	@$(NINJA) src/yos/codegen/libyos_api_bridge.a

# ─── Top-level binaries ────────────────────────────────────────────────

yos: setup
	@$(NINJA) src/yos/yos

libc-pure: setup
	@$(NINJA) build-tools/freebsd-libc/libc-pure.wasm

run: yos
	@$(BUILD)/src/yos/yos $(ARGS)

# ─── Tests ─────────────────────────────────────────────────────────────

test: yos libc-pure
	@$(NIX_DEV) meson test -C $(BUILD)

test-libc: yos libc-pure
	@$(NIX_DEV) meson test -C $(BUILD) --suite yos:libc

test-freebsd: yos libc-pure
	@$(NIX_DEV) meson test -C $(BUILD) --suite yos:freebsd-libc

test-list:
	@$(NIX_DEV) meson test -C $(BUILD) --list

testlog:
	@cat $(BUILD)/meson-logs/testlog.txt

# ─── External wasm packages ────────────────────────────────────────────

# Each entry below shells out to tools/wasm-pkg.sh, which sources the
# matching build-tools/wasm-pkg/configs/<dep>/build.sh with the env
# it expects (ROOT/PREFIX/WASM_CC/WASM_SYSROOT, plus DEP_PREFIXES for
# deps that have transitive deps). The PREFIX path is derived from
# the build.sh's VERSION string and lives at
# build-linux/wasm-pkgs/<name>-<version>/out (kept on `build-linux/`
# even on darwin to match nvim's hardcoded WORK path).

# Common env for every wasm-pkg invocation. Set once, exported.
# WASM_CFLAGS / WASM_LDFLAGS are the base compile/link flags every dep
# starts from — each build.sh prepends its own target-specific bits
# (e.g. `--target=wasm32-unknown-unknown -nostdlib`) before appending
# package-specific ones. Mirror what tests/freebsd-libc uses so deps
# share an ABI with the test harness.
WASM_BASE_CFLAGS = -target wasm32-unknown-unknown -nostdlib -O2 \
                   -fno-builtin -ffreestanding \
                   --sysroot=$(CURDIR)/$(BUILD)/sysroot \
                   -isystem $(CURDIR)/$(BUILD)/sysroot/usr/include \
                   -D__i386__=1 -D__yos__=1 -Wno-unused-parameter
WASM_BASE_LDFLAGS = -Wl,--no-entry -Wl,--export-all -Wl,--allow-undefined

WASM_PKG_ENV = \
    ROOT=$(CURDIR) \
    WASM_CC=$(CURDIR)/build-tools/wasm-clang \
    WASM_SYSROOT=$(CURDIR)/$(BUILD)/sysroot \
    WASM_CFLAGS='$(WASM_BASE_CFLAGS)' \
    WASM_LDFLAGS='$(WASM_BASE_LDFLAGS)'

# Each dep target depends on `sysroot` (so wasm headers + crt1.o exist)
# and on its own transitive deps. Stamp files live next to the install
# prefix so re-runs are no-ops once a dep has been built.
.PHONY: pkg-lua pkg-libuv pkg-msgpack-c pkg-unibilium pkg-libvterm \
        pkg-tree-sitter pkg-lpeg pkg-lua-mpack pkg-luv nvim-deps nvim-build

pkg-lua: sysroot
	@$(NIX_DEV) env $(WASM_PKG_ENV) bash tools/wasm-pkg.sh lua

pkg-libuv: sysroot
	@$(NIX_DEV) env $(WASM_PKG_ENV) bash tools/wasm-pkg.sh libuv

pkg-msgpack-c: sysroot
	@$(NIX_DEV) env $(WASM_PKG_ENV) bash tools/wasm-pkg.sh msgpack-c

pkg-unibilium: sysroot
	@$(NIX_DEV) env $(WASM_PKG_ENV) bash tools/wasm-pkg.sh unibilium

pkg-libvterm: sysroot
	@$(NIX_DEV) env $(WASM_PKG_ENV) bash tools/wasm-pkg.sh libvterm

pkg-tree-sitter: sysroot
	@$(NIX_DEV) env $(WASM_PKG_ENV) bash tools/wasm-pkg.sh tree-sitter

pkg-lpeg: pkg-lua
	@$(NIX_DEV) env $(WASM_PKG_ENV) bash tools/wasm-pkg.sh lpeg lua

pkg-lua-mpack: pkg-lua
	@$(NIX_DEV) env $(WASM_PKG_ENV) bash tools/wasm-pkg.sh lua-mpack lua

pkg-luv: pkg-lua pkg-libuv
	@$(NIX_DEV) env $(WASM_PKG_ENV) bash tools/wasm-pkg.sh luv lua libuv

# All 9 nvim deps, in build-order.
nvim-deps: pkg-lua pkg-libuv pkg-msgpack-c pkg-unibilium pkg-libvterm \
           pkg-tree-sitter pkg-lpeg pkg-lua-mpack pkg-luv

nvim-build: yos libc-pure nvim-deps
	@$(NIX_DEV) env $(WASM_PKG_ENV) bash tools/wasm-pkg.sh nvim \
	    lua libuv msgpack-c unibilium libvterm tree-sitter lpeg lua-mpack luv

# ─── Misc ──────────────────────────────────────────────────────────────

check: setup
	@$(NIX_DEV) ninja -C $(BUILD) src/yos/yos

format:
	@find src/yos -name '*.c' -o -name '*.h' -o -name '*.hpp' -o -name '*.cpp' \
	    | xargs $(NIX_DEV) clang-format -i
