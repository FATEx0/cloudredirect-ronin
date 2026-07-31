{
  description = "CloudRedirect (Ronin)";

  inputs.nixpkgs.url = "github:nixos/nixpkgs?ref=nixos-unstable";

  outputs =
    {
      self,
      nixpkgs,
    }:
    let
      forAllSystems =
        fn:
        nixpkgs.lib.genAttrs nixpkgs.lib.platforms.linux (
          system:
          let
            pkgs = import nixpkgs { inherit system; };
          in
          fn pkgs
        );
    in
    {
      formatter = forAllSystems (pkgs: pkgs.nixfmt-tree);

      packages = forAllSystems (pkgs: rec {
        cloud-redirect = pkgs.callPackage ./nix-modules/default.nix {
          rev = self.rev or self.dirtyRev or "unknown";
        };
        default = cloud-redirect;
      });
    };
}
