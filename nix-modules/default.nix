{
  rev,
  lib,
  pkgs,
}:
pkgs.pkgsi686Linux.stdenv.mkDerivation {
  pname = "cloud_redirect";
  version = "${rev}";
  src = ../.;

  nativeBuildInputs = [ pkgs.cmake pkgs.patchelf ];

  # steamclient.so is 32-bit; Steam's own libstdc++ ABI expectation is the
  # pre-C++11 std::string/std::list layout CMakeLists.txt already selects via
  # _GLIBCXX_USE_CXX11_ABI=0. Building the whole tree under pkgsi686Linux
  # (a genuine i686 toolchain, not -m32 cross-compilation on a 64-bit
  # compiler) avoids CMakeLists.txt's Fedora/Debian multilib-path guessing
  # for LINUX_32BIT entirely -- CMAKE_SIZEOF_VOID_P is already 4 here.
  configurePhase = ''
    runHook preConfigure
    cmake -S . -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCR_GIT_SHA=${rev}
    runHook postConfigure
  '';

  buildPhase = ''
    runHook preBuild
    cmake --build build --target cloud_redirect -j''${NIX_BUILD_CORES:-1}
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p $out
    cp build/cloud_redirect.so $out/
    runHook postInstall
  '';

  # module/payload is copied out of the Nix store and must load from Steam's
  # runtime on another machine. Retaining build-host store paths would make
  # the artifact depend on an unregistered, garbage-collectable closure.
  postFixup = ''
    patchelf --remove-rpath $out/cloud_redirect.so
  '';

  meta = {
    description = "Steam Cloud redirection extension (Ronin)";
    homepage = "https://github.com/Selectively11/CloudRedirect";
    platforms = lib.platforms.linux;
  };
}
