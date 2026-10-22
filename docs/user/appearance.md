---
title: Appearance
description:
  Choose light or dark mode, adjust corners and select a theme palette.
---

Open settings from the account area at the bottom of the sidebar.

## Color mode

In **Appearance**, choose **Dark**, **Light** or **System**. System follows your
desktop's color preference through the XDG Settings portal. If the desktop does
not provide a preference, Cressida uses dark mode.

The color mode and theme palette are separate choices. Choosing a new palette
does not require you to change from light to dark, or from System to a fixed
mode.

## Theme palettes

Open **Cressida > Themes** to browse palettes and preview their light and dark
variants. Bundled choices include:

- Cressida.
- Catppuccin Mocha / Latte.
- Rosé Pine / Dawn.
- Gruvbox dark / light.

You can also [create a custom theme](custom-themes.md) with a TOML file.

## Corners and text

**Corner radius** controls the roundness of panels and controls. The default is
100%; 0% gives square corners and 200% increases rounding. Circular avatars and
presence dots stay circular.

Use the available message text and spacing settings to adjust reading comfort.
Compact mode changes the conversation's presentation. Appearance settings are
saved per account.

Text aligns to the display's pixel grid at the current display scale and client
zoom, including while scrolling. This happens automatically and keeps text sharp
without changing the layout.

## Profile cosmetics

Open **Cressida > Profile cosmetics** to control each type separately:

- **Avatar decorations** show equipped frames around user avatars in chat,
  conversation lists, member lists and profiles.
- **Nameplates** show equipped backgrounds and their palette colors behind users
  in conversation and member lists and the account area. Artwork keeps its
  original colors in both light and dark mode.
- **Profile effects** show equipped visual effects inside a profile.
- **Profile frames** show equipped borders around full profiles and the DM
  profile panel.
- **Profile banners** show banner images at the top of profiles.
- **Profile colors** use the profile's chosen background colors.

Each switch defaults on and is saved separately for your account. These are
local display preferences; changing them does not change anyone's equipped
cosmetics. Missing or unavailable artwork leaves the ordinary profile visible.

Turning off a category releases its cached images and animations and cancels
pending artwork loads. Images still used elsewhere stay available. Turning the
category back on loads its artwork again as needed.

Avatar decorations and nameplates animate while you hover over that user's row,
message or profile. A profile popup's avatar decoration keeps animating while
the popup is open, even when the pointer moves away. Other copies stay still.
Reduced motion uses still artwork. Turning off **Animate avatars** also keeps
cosmetic artwork still. Profile effects stay inside the profile's rounded edges.

Profile frames follow the card's size, with side borders clipped to the profile
and decorative top and bottom pieces placed around it. Server profiles inherit
global cosmetics unless they supply a replacement for that category.

## Motion

Open **Settings > Accessibility > Motion** and enable **Reduce motion** to make
supported transitions settle immediately, including server folder opening and
closing. Scrolling becomes immediate and animated artwork stays still.

To keep selected media animated, enable **Allow GIFs with reduced motion**,
**Allow emoji with reduced motion**, or both. These switches start off and are
saved separately for your account. They apply to visible message media and
previews, including the GIF picker and image viewer.

The overrides retain your other playback choices. **Animate emoji** must be on
for custom emoji to animate. **Automatically play GIFs** controls GIF autoplay;
when it is off, hover over a GIF to play it. Reduced motion still keeps avatars,
profile cosmetics, role-color motion and interface transitions still.

## When System mode does not follow the desktop

Check that your desktop portal and its Settings backend are running. A fixed
Light or Dark choice always takes precedence over the desktop hint. See
[Troubleshooting](troubleshooting.md#system-colors-do-not-change).
