{
  description = "CloudRedirect (Ronin)";

  # Build the copied module payload against a conservative userspace ABI.
  # Tracking unstable made the ELF require the build host's GLIBC 2.38 and
  # embedded absolute Nix-store RUNPATHs, which is not a portable module.
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-21.11";

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
