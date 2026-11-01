{
  mkShell,
  meson,
  ninja,
  pkg-config,
  wayland-scanner,
  python3,
  dbus,
  gdb,
  just,
  nixfmt,
  llvmPackages_22,
  cressida,
  noto-fonts-color-emoji,
  actionlint,
  shellcheck,
  weston,
  mesa,
  shared-mime-info,
}:
mkShell {
  inputsFrom = [ cressida ];
  # Debug recipes use -O0, where Nix's Fortify define cannot take effect.
  # The optimized package build keeps its normal hardening.
  hardeningDisable = [ "fortify" ];
  nativeBuildInputs = [
    meson
    ninja
    pkg-config
    wayland-scanner
    python3
    dbus
    gdb
    just
    nixfmt
    llvmPackages_22.clang-tools
    llvmPackages_22.llvm
    actionlint
    shellcheck
    weston
  ];
  shellHook = ''
    export CRESSIDA_ASSETS_DIR="$PWD/assets"
    export XDG_DATA_DIRS="${shared-mime-info}/share''${XDG_DATA_DIRS:+:$XDG_DATA_DIRS}"
    export CRESSIDA_EMOJI_FONT="${noto-fonts-color-emoji}/share/fonts/noto/NotoColorEmoji.ttf"
    # Used only by the private headless CI helper, never by normal GUI launches.
    export CRESSIDA_CI_EGL_VENDOR="${mesa}/share/glvnd/egl_vendor.d/50_mesa.json"
    export CRESSIDA_CI_MESA_DRIVERS="${mesa}/lib/dri"
  '';
}
