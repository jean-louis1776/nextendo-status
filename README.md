# NexStatus

NexStatus shows which [Nextendo](https://nextendo.network) game servers are up and how many people are playing. It is a Nintendo Switch homebrew app, plus an [Ultrahand](https://github.com/ppkantorski/Ultrahand-Overlay) overlay for checking the same public counts without leaving a game.

No account and no sign-in. The console reads Nextendo's public status API.

NexStatus is not affiliated with Nintendo or Nextendo.

## What you get

The release is one zip. Unzip it at the root of the SD card, not inside another folder. It already contains the right paths:

| Path on the SD card | What it is |
| --- | --- |
| `switch/nexstatus.nro` | The app in the Homebrew Menu |
| `switch/.overlays/nexstatus.ovl` | The Ultrahand overlay |

You need the Homebrew Menu for the app, and Ultrahand for the overlay. The dot in `.overlays` is part of the folder name. Ultrahand only loads overlays from `sdmc:/switch/.overlays/`.

## The app

Open **NexStatus** from the Homebrew Menu.

The screen is a gray list of game servers. Each row has the game icon, the name, whether that server is online, and how many people are playing. Names that do not fit are cut with `...`. The list is sorted with the busiest servers first. Servers with nobody on them stay in the list, so an empty server is not hidden.

A plaque at the top summarizes the network: how many game servers are offline, or that everything is up, then how many of the servers are active and how many players are online. The clock under that is the console clock. It changes only when a player count or an online/offline state actually changes, not on every refresh.

The list itself refreshes about every 15 seconds. Scroll it with the stick.

**(+)** on the bottom left leaves the app. The bottom right is the version and the credit, `by ILALEX & Nextendo`.

### Updates

On launch, the app checks GitHub for a newer release. If one exists, a gray window with a soft red border blocks the list.

- **A** starts the download. The window shows percent and speed above the bar.
- When it finishes, the window says the install is complete. **A** closes the app. Open it again from the Homebrew Menu.
- If the install fails, **A** tries again.
- **(+)** is ignored while the file is downloading, so leaving mid-download does not break the app.

The download is `nexstatus.zip`. The app installs both `switch/nexstatus.nro` and `switch/.overlays/nexstatus.ovl`, so the overlay updates together with the app. The first time you add the overlay, unzip a release once so the file exists. After that, updating inside the app replaces both files.

## The overlay

Open Ultrahand, then **NexStatus**.

It shows the same public counts without icons, so it stays small:

1. **Refresh** asks for new numbers immediately.
2. **Online** is the total number of players.
3. **Games** is one row per game: the name on the left, the player count on the right. The busiest games are first. Scroll for the rest.

While the overlay is open, it refreshes about every 15 seconds. While it is closed, it does not fetch anything. The network socket is opened for one request and closed again, so it does not sit on the game's network.

## Build

Install [devkitPro](https://devkitpro.org/) with `switch-dev`, `switch-sdl2`, `switch-sdl2_ttf`, `switch-libjpeg-turbo`, `switch-curl`, and `switch-mbedtls`. From the repository root:

```sh
make
```

That builds the app and the overlay, then writes `nexstatus.zip` in the repository root with the SD card layout above. `make clean` removes the build output and the zip.

The version baked into the app is `APP_VERSION` in the root Makefile. A release tag `v1.0.0` is built with that version so the console can tell it is newer than what is installed.

## License

NexStatus is released under the GNU General Public License v2.0. See [LICENSE](LICENSE).

The QR code generator in `app/source/qrcodegen.cpp` is copyright Project Nayuki and is used under the MIT license. The overlay UI uses [libtesla](https://github.com/WerWolv/libtesla), which includes Sean Barrett's stb_truetype (public domain).

## Acknowledgements

- [devkitPro](https://devkitpro.org/) and [libnx](https://github.com/switchbrew/libnx)
- [SDL2](https://www.libsdl.org/) and SDL2_ttf
- [libcurl](https://curl.se/) and [libjpeg-turbo](https://libjpeg-turbo.org/)
- [Project Nayuki](https://www.nayuki.io/page/qr-code-generator-library), for the QR code generator
- [WerWolv](https://github.com/WerWolv), for libtesla
- [ppkantorski](https://github.com/ppkantorski/Ultrahand-Overlay), for Ultrahand
- [Nextendo](https://nextendo.network), whose public status API this displays
