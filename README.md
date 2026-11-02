> [!WARNING]
>
> **Account risk: new DMs and server joins**
>
> Take particular care when starting new DMs or joining new servers. Other
> native-client projects mention these actions as possible risk factors.
>
> Users have reported account flags and messaging restrictions while using
> Cressida. **The trigger remains unconfirmed.** Existing conversations and our
> request safeguards do not guarantee protection from restrictions or
> suspension.
>
> Read the [account-risk FAQ](docs/user/faq.md#can-my-account-be-restricted) for
> details. Complete any verification or security checks in the official app.

<p align="center">
  <a href="https://cressida.lysec.dev">
    <img src="assets/cressida-logo.svg" alt="Cressida logo" width="180">
  </a>
</p>

Cressida is an unofficial, unaffiliated third-party client.

# Cressida

A native desktop chat client for Linux and Wayland.

**Early development:** text chat and voice are available, with experimental
screen sharing. Video calls and some other features remain incomplete. Expect
occasional rough edges.

A few highlights:

- Rich profiles and animated server role colors.
- Threads and forum posts alongside server chat and DMs.
- Built-in image/video viewing, GIFs, custom emoji and stickers.
- Custom themes with automatic dark and light switching.

## Can my account be restricted?

Yes. Multiple users have reported violations and messaging restrictions while
using Cressida, including reports immediately after sending a message.
Suspension is also a possible account risk. We have not established the trigger,
and our request safeguards cannot guarantee protection from restrictions or
suspension.

Take particular care when **starting new DMs or joining new servers**, which
other native-client projects mention as possible risk factors. These are not
confirmed Cressida triggers, and existing conversations are not guaranteed to
avoid account flags.

If the service requests verification or a CAPTCHA, complete it in the official
app. Read the [account-risk FAQ](docs/user/faq.md#can-my-account-be-restricted)
before signing in.

## Documentation

Visit [cressida.lysec.dev](https://cressida.lysec.dev) for the website and
[user documentation](https://cressida.lysec.dev/docs/) covering installation,
everyday use, themes and troubleshooting.

## Run

A Wayland desktop and working graphics drivers are required. See the
[installation guide](https://cressida.lysec.dev/docs/installation/) and
[packaging notes](PACKAGING.md) for build requirements and installation options.
With the build dependencies installed, `just install` builds and installs a
release under `/usr/local`. Use `just install release "$HOME/.local"` for a
user-local install.

With a desktop tray available, closing the window keeps Cressida running. Use
the tray's **Open**, **Restart** or **Close** menu; **Close** exits the
application. Run `cressida --start-minimized` for a hidden tray startup. If no
tray host appears within five seconds, the window opens normally. Launching
`cressida` or its application-menu entry again opens or focuses the running
client. Use `cressida --new-instance` (or `cressida -n`) for a separate client.

## Sign in

Choose the browser sign-in option and finish signing in through your browser. A
desktop keyring, such as GNOME Keyring or KeePassXC with Secret Service enabled,
can remember your sign-in. Signing out removes the saved login.

## Current limitations

Video calls, arbitrary chat file uploads and file drag-and-drop are not
available yet. Voice supports server channels and joining existing direct or
group calls; see the [voice guide](docs/user/voice.md) for settings and current
limits. Screen sharing needs PipeWire and your desktop's ScreenCast portal; see
the [screen sharing guide](docs/user/screen-sharing.md) for setup and current
limits. Image uploads are supported. Some service controls and settings remain
incomplete.

## Contributing

Documentation lives under `docs/` on `main`; the website imports it during its
build. See [the authoring guide](docs/README.md) to update or preview a page.

See [CONTRIBUTING.md](CONTRIBUTING.md) for development and pull request
guidance.

## License

[MIT](LICENSE). Bundled [Tabler icons](assets/fonts/tabler-icons-license.txt),
[emoji alias data](assets/emoji-data-license.txt),
[theme palettes](assets/themes/LICENSES.txt), [Wuffs](third_party/wuffs) and
[MLS++](third_party/mlspp/LICENSE) retain their own license notices.
