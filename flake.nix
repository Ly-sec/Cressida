{
  description = "Cressida — native C++ Discord desktop client";

  inputs.nixpkgs.url = "https://channels.nixos.org/nixos-unstable/nixexprs.tar.zst";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      rev = self.shortRev or self.dirtyShortRev;
      forAllSystems = nixpkgs.lib.genAttrs systems;
      pkgsFor = system: nixpkgs.legacyPackages.${system};
    in
    {
      packages = forAllSystems (
        system:
        let
          pkgs = pkgsFor system;
        in
        {
          default = pkgs.callPackage ./nix/package.nix { inherit rev; };
        }
      );

      devShells = forAllSystems (system: {
        default = (pkgsFor system).callPackage ./nix/devshell.nix {
          cressida = self.packages.${system}.default;
        };
      });

      checks = forAllSystems (system: {
        client = self.packages.${system}.default.override { doCheck = true; };
      });

      apps = forAllSystems (system: {
        default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/cressida";
        };
      });

      formatter = forAllSystems (system: (pkgsFor system).nixfmt);
    };
}
