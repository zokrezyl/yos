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
      in {
        # `nix develop` — interactive shell with the wasm toolchain wired up.
        # NOTE: ./nixpkgs/ tree (yos.devShell, package builders) is gone for
        # now; this is a minimal shell with just what meson needs to build.
        devShells.default = pkgs.mkShell {
          packages = with pkgs; [ meson ninja pkg-config ];
        };
      });
}
