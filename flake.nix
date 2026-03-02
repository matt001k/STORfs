{
  description = "Development environment for StorFS";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.11";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      nixpkgs,
      flake-utils,
      ...
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        overlays = [ ];
        pkgs = import nixpkgs {
          inherit system overlays;
        };
      in
      {
        devShells.default = pkgs.mkShell {
          stdenv = pkgs.clangStdenv;
          packages = with pkgs; [
            gnumake

            # for testing
            (python313.withPackages (
              ps: with ps; [
              ]
            ))
            gcovr
            ruby
            ceedling

            # additional tools, nice to have
            bear # to generate compile_commands.json
            llvmPackages_18.clang-tools # we use ClangFormatter 18

          ];

          shellHook = ''
            export LD_LIBRARY_PATH="${
              pkgs.lib.makeLibraryPath [
              ]
            }:$LD_LIBRARY_PATH"
            chmod +x ./test/run.sh
          '';
        };

      }
    );
}
