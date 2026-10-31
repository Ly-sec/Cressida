{
  lib,
  stdenv,
  callPackage,
  meson,
  ninja,
  pkg-config,
  wayland-scanner,
  wayland,
  wayland-protocols,
  libglvnd,
  libGL,
  cairo,
  pango,
  freetype,
  fontconfig,
  harfbuzz,
  librsvg,
  libwebp,
  zstd,
  libjxl,
  libxkbcommon,
  nlohmann_json,
  tomlplusplus,
  curl,
  openssl,
  qrencode,
  autoAddDriverRunpath,
  makeWrapper,
  xdg-utils,
  noto-fonts-color-emoji,
  python3,
  dbus,
  md4c,
  libsecret,
  ffmpeg-headless,
  mpv-unwrapped,
  rlottie,
  shared-mime-info,
  opus,
  libsodium,
  libpulseaudio,
  pipewire,
}:
stdenv.mkDerivation {
  pname = "cressida";
  version = "0.1.0";

  src = lib.cleanSourceWith {
    src = ../.;
    filter =
      path: type:
      lib.cleanSourceFilter path type
      && !(type == "directory" && lib.hasPrefix "build" (builtins.baseNameOf path))
      && !(builtins.elem (builtins.baseNameOf path) [
        ".direnv"
        ".agents"
        ".aws"
        ".codex"
        "ci-logs"
        "dist"
      ]);
  };

  outputs = [
    "out"
    "dev"
  ];
  nativeBuildInputs = [
    meson
    ninja
    pkg-config
    wayland-scanner
    autoAddDriverRunpath
    makeWrapper
    xdg-utils
    python3
    dbus
  ];
  buildInputs = [
    wayland
    wayland-protocols
    libglvnd
    libGL
    cairo
    pango
    freetype
    fontconfig
    harfbuzz
    librsvg
    libwebp
    zstd
    libjxl
    libxkbcommon
    nlohmann_json
    tomlplusplus
    md4c
    libsecret
    openssl
    qrencode
    ffmpeg-headless
    mpv-unwrapped
    rlottie
    opus
    libsodium
    libpulseaudio
    pipewire
    (curl.override { websocketSupport = true; })
    (callPackage ./stb.nix { })
  ];

  doCheck = true;
  # GIO's byte-based MIME detection needs a database even in a clean sandbox.
  preCheck = ''
    export XDG_DATA_DIRS="${shared-mime-info}/share''${XDG_DATA_DIRS:+:$XDG_DATA_DIRS}"
  '';
  mesonFlags = [ "-Demoji_font=${noto-fonts-color-emoji}/share/fonts/noto/NotoColorEmoji.ttf" ];
  postFixup = ''
    wrapProgram "$out/bin/cressida" \
      --prefix XDG_DATA_DIRS : "${shared-mime-info}/share"
  '';

  meta = {
    description = "Cressida — native C++ Discord desktop client";
    mainProgram = "cressida";
    license = with lib.licenses; [
      mit
      asl20
      bsd2
    ];
    platforms = lib.platforms.linux;
  };
}
