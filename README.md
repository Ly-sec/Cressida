<p align="center">
  <img src="assets/cressida-logo.svg" alt="Cressida logo" width="180">
</p>

> [!WARNING]
> Cressida is an unofficial third-party Discord client. It is not affiliated with or
> endorsed by Discord.

# Cressida

A native Discord desktop client for Linux and Wayland.

**Early development:** text chat is available; voice and video calls are not yet
supported. Expect missing features and occasional rough edges.

A few highlights:

- Rich profiles and animated server role colors.
- Threads and forum posts alongside server chat and DMs.
- Built-in image/video viewing, GIFs, custom emoji and stickers.
- Custom themes with automatic dark and light switching.

## Run
A Wayland desktop and working graphics drivers are required.
For builds without Nix or Guix and distribution packages, see [PACKAGING.md](PACKAGING.md).

### Nix
With Nix installed, run from this checkout:

```sh
nix run path:.
```

### Guix
Add the following to your list of channels:

``` scheme
(channel
    (name 'cressida)
    (url "https://github.com/Ly-sec/Cressida")
    (branch "main"))
```

After you perform `guix pull` (or `guix pull --select=cressida`), the `cressida-git`
package will be available under the module `(cressida)`. Whenever you want to update,
run `guix pull` again to get latest commit.

## Sign in

Choose **Sign in with Discord** and finish signing in through your browser.
A desktop keyring, such as GNOME Keyring or KeePassXC with Secret Service enabled,
can remember your sign-in. Signing out removes the saved login.

## Current limitations

Voice/video calls, arbitrary file uploads and drag-and-drop are not available yet.
Image uploads are supported. Some Discord controls and settings remain incomplete.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for development and pull request guidance.

## License

[MIT](LICENSE). Bundled [Tabler icons](assets/fonts/tabler-icons-license.txt),
[emoji alias data](assets/emoji-data-license.txt), [theme palettes](assets/themes/LICENSES.txt)
and [Wuffs](third_party/wuffs)
retain their own license notices.
