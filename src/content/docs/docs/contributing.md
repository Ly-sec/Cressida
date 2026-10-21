---
title: Contributing
description:
  Report issues, improve documentation and validate focused Cressida
  contributions.
---

## Find a useful change

Read the [open issues](https://github.com/Ly-sec/Cressida/issues), reproduce a
bug or improve a guide you found confusing. Discuss substantial new features
before starting a large implementation. Keep pull requests focused on one clear
problem.

Documentation should describe what works today. Changes to behavior, commands,
settings, dependencies or limitations should update the relevant website pages
and guides in the same change.

## Set up development

Follow [Building from source](../building/). Nix is optional. With the native
dependencies and `just` installed:

```sh
just build
just test
just format
just lint
```

Formatting uses clang-format 22 and nixfmt. Lint also needs actionlint,
ShellCheck and Python 3.11 or newer. For memory-sensitive C++ changes, use
`just test asan` with `llvm-symbolizer` available. Explain any checks you could
not run.

With the supplied Nix environment, `just check` builds and tests the locked
package.

## Check UI changes

Use synthetic demo data rather than personal account content:

```sh
just demo
CRESSIDA_DEMO_VIEW=themes just demo
just smoke-headless build-debug/cressida
```

The headless checks need Weston and Mesa. Check both light and dark appearances,
narrow windows, keyboard interaction and pointer hit areas when they are
affected.

## Architecture expectations

Use C++23 and preserve the native Wayland renderer and independent network
layer. Network workers return events to the owning application loop. Keep
credentials out of code, logs and fixtures. External pages open in the user's
browser.

Client UI layout must use the shared style tokens and layout metrics so drawing,
measurement and hit regions agree. The project does not use GTK, Qt or an
embedded browser for its interface.

## Pull requests

Explain the problem, the resulting behavior and the checks you ran. For visual
changes, include representative screenshots with sample data. Keep known
limitations visible rather than making an unfinished control appear supported.

## Website changes

The website is an Astro and Starlight project. Its repository README covers
local development, validation and GitHub Pages deployment. Keep documentation
clear, practical and addressed to the person using Cressida. Use ordinary
punctuation without em dashes, and keep links and screenshots accurate.
