{
  description = "YOS — Linux syscall runtime for wasm3, with package builders";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-24.05";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
        yos = import ./nixpkgs { inherit pkgs; src = self; };
      in {
        # `nix develop` — interactive shell with the wasm toolchain wired up.
        devShells.default = yos.devShell;

        # `nix build .#<name>` — derivations.
        # Layered:
        #   .#yos        host runtime (lowest)
        #   .#musl       wasm32 sysroot
        #   .#toolchain  wasm-cc + lld + clang-unwrapped + wasm-opt
        # Examples / ported packages:
        #   .#hello      mkYosPackage example
        #   .#busybox    busybox 1.36.1 wasm
        #   .#nvim       neovim 0.10.4 wasm (with all 9 deps)
        # nvim's libraries (each is its own derivation, also exposed):
        #   .#lua .#libuv .#msgpack-c .#unibilium .#libvterm
        #   .#tree-sitter .#lpeg .#lua-mpack .#luv
        packages = {
          default = yos.yos;
          inherit (yos)
            yos musl toolchain
            hello busybox nvim
            lua libuv msgpack-c unibilium libvterm
            tree-sitter lpeg lua-mpack luv;
        };

        # Re-export the package builders so downstream flakes can do
        # `yos.lib.${system}.mkYosPackage { … }` or `…buildRecipe`.
        lib = {
          inherit (yos) mkYosPackage buildRecipe;
        };
      });
}
