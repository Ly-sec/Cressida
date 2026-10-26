{
  lib,
  stdenv,
  fetchzip,
  cmake,
  ninja,
  openssl,
  nlohmann_json,
}:
let
  mlspp = stdenv.mkDerivation {
    pname = "cressida-mlspp";
    version = "unstable-2026-10-26";
    src = fetchzip {
      url = "https://codeload.github.com/cisco/mlspp/tar.gz/1cc50a124a3bc4e143a787ec934280dc70c1034d";
      extension = "tar.gz";
      hash = "sha256-IjS2yYnfScwJR3BqDJp37ANgNkCg9ECxON41tYEocvA=";
    };
    nativeBuildInputs = [
      cmake
      ninja
    ];
    buildInputs = [
      openssl
      nlohmann_json
    ];
    postPatch = ''
      substituteInPlace CMakeLists.txt --replace-fail "set(CMAKE_CXX_STANDARD 17)" "set(CMAKE_CXX_STANDARD 23)"
      substituteInPlace lib/CMakeLists.txt \
        --replace-fail "add_subdirectory(mls_ds)" "" \
        --replace-fail "add_subdirectory(mls_vectors)" ""
    '';
    cmakeFlags = [
      "-DMLS_CXX_NAMESPACE=mlspp"
      "-DDISABLE_GREASE=ON"
      "-DBUILD_SHARED_LIBS=ON"
      "-DTESTING=OFF"
    ];
    postInstall = ''
      install -Dm644 ../LICENSE "$out/share/licenses/cressida-mlspp/LICENSE"
      mkdir -p "$out/lib/pkgconfig"
      cat > "$out/lib/pkgconfig/mlspp.pc" <<EOF
      prefix=$out
      libdir=\''${prefix}/lib
      includedir=\''${prefix}/include/mlspp

      Name: mlspp
      Description: MLS group protocol
      Version: 0.1
      Libs: -L\''${libdir} -lmlspp -lhpke -ltls_syntax -lbytes
      Cflags: -I\''${includedir}
      EOF
    '';
    meta.license = lib.licenses.bsd2;
  };
in
stdenv.mkDerivation {
  pname = "libdave";
  version = "unstable-2026-10-26";
  src = fetchzip {
    url = "https://codeload.github.com/discord/libdave/tar.gz/6396c90aedbb9a6ee117ea9ce1ce9832af6a3375";
    extension = "tar.gz";
    hash = "sha256-kU+f17tUDlNq/IVwEKhsMj1PYOW5oyuApjjwQm4FymY=";
  };
  sourceRoot = "source/cpp";
  nativeBuildInputs = [
    cmake
    ninja
  ];
  buildInputs = [
    openssl
    nlohmann_json
    mlspp
  ];
  propagatedBuildInputs = [ mlspp ];
  postPatch = ''
    substituteInPlace CMakeLists.txt --replace-fail "set(CMAKE_CXX_STANDARD 17)" "set(CMAKE_CXX_STANDARD 23)"
  '';
  cmakeFlags = [
    "-DBUILD_SHARED_LIBS=ON"
    "-DPERSISTENT_KEYS=OFF"
    "-DTESTING=OFF"
  ];
  postInstall = ''
    mkdir -p "$out/lib/pkgconfig"
    cat > "$out/lib/pkgconfig/libdave.pc" <<EOF
    prefix=$out
    libdir=\''${prefix}/lib
    includedir=\''${prefix}/include

    Name: libdave
    Description: Audio frame encryption and MLS sessions
    Version: 1.0
    Libs: -L\''${libdir} -ldave
    Cflags: -I\''${includedir}
    EOF
  '';
  meta = {
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
  };
}
