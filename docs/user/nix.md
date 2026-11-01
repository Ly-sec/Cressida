---
title: Install with a Nix flake
description:
  Add the Cressida flake input and install its package with NixOS or Home
  Manager.
---

Add the Cressida input, then install its default package through NixOS or Home
Manager. Packages are available for `x86_64-linux` and `aarch64-linux`.

## Add the input

Add Cressida to your existing `flake.nix`:

```nix
{
  inputs.cressida.url = "github:Ly-sec/Cressida";
}
```

The examples below assume your modules receive `inputs` through `specialArgs`
for NixOS or `extraSpecialArgs` for standalone Home Manager. Keep Cressida's own
Nixpkgs input pinned as supplied for its build dependencies.

Normal Nix package builds omit test targets and do not run the test suite. CI
and `nix flake check` use a separate build with tests enabled.

## Install the package

### NixOS

Add the package to your system configuration:

```nix
# configuration.nix
{ inputs, pkgs, ... }:
{
  environment.systemPackages = [
    inputs.cressida.packages.${pkgs.stdenv.hostPlatform.system}.default
  ];
}
```

Rebuild from your configuration's flake directory, replacing `my-host` with its
configuration name:

```sh
sudo nixos-rebuild switch --flake .#my-host
```

### Home Manager

Add the package to your home configuration:

```nix
# home.nix
{ inputs, pkgs, ... }:
{
  home.packages = [
    inputs.cressida.packages.${pkgs.stdenv.hostPlatform.system}.default
  ];
}
```

For standalone Home Manager, activate it with your configuration name:

```sh
home-manager switch --flake .#your-user
```

For Home Manager integrated into NixOS, use your usual `nixos-rebuild switch`
command instead.

## Launch

Open **Cressida** from your application launcher or run `cressida`. Use
`cressida --demo` for an offline preview, or follow [Sign in](sign-in.md).

Cressida needs a Wayland session and working graphics drivers. See
[desktop runtime requirements](packaging.md#desktop-runtime) for desktop
services.

## Update or remove

Update the Cressida input from your configuration's flake directory:

```sh
nix flake update cressida
```

Then rebuild or activate the same configuration and restart Cressida.

To uninstall, remove the package from your package list and rebuild. Remove the
input if it is no longer used. Account data stays in the locations listed in
[Files and data](files-and-data.md).
