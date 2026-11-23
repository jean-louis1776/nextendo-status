# NexStatus

NexStatus is a Nintendo Switch homebrew application and an unofficial Android companion for [Nextendo](https://nextendo.network) server status. It lists game servers, player counts, and, after sign-in, lobbies and players. An [Ultrahand](https://github.com/ppkantorski/Ultrahand-Overlay) overlay shows the same servers and, after sign-in, lobbies.

NexStatus is not affiliated with Nintendo or Nextendo.

## Install

### Switch

The Switch release is one zip. Extract it at the root of the SD card.

| Path | Contents |
| --- | --- |
| `switch/nexstatus.nro` | Homebrew Menu application |
| `switch/.overlays/nexstatus.ovl` | Ultrahand overlay |

The Homebrew Menu is required for the application. Ultrahand is required for the overlay. Overlay files must be in `sdmc:/switch/.overlays/`.

In Ultrahand, set **Overlay memory** to **8 MB**. The 4 MB default is too small: the overlay asks for an 8 MB heap so the Tesla screen and HTTPS fit together. With 4 MB it can fail to load or crash Atmosphere.

### Android

Install `nexstatus.apk` from the GitHub release. The phone app is personal / unofficial and is not on Google Play. Allow installs from the browser or file manager when prompted.

## Application (Switch)

Open NexStatus from the Homebrew Menu.

The main screen lists game servers, busiest first. Each entry shows the game icon, name, server state, and player count. The header shows the number of players online, the number of active servers, and the time of the last count update. Counts refresh about every 15 seconds.

![Server list](screenshots/servers.jpg)

The account control is in the header.

- Signed out: **A** opens sign-in. The console shows a code and a QR code. The phone completes sign-in. The console does not open a browser.
- Signed in: **A** opens the account screen, which shows the username and friend code. **X** logs out.

**A** on a server opens that game. Signed out, the page asks for sign-in. Signed in, it shows lobbies and players from Nextendo, including mode, room state, occupied slots, and a friend marker. The page loads on open and refreshes about every 15 seconds while it stays open. **B** returns to the list.

![Lobby](screenshots/lobby.jpg)

**(−)** on the main screen opens credits. It does nothing on other screens. **(+)** exits. The bottom right shows the version and the battery level.

### Updates (Switch)

On launch the application checks GitHub for a newer release. If one is available, an update dialog is shown.

- **A** downloads and installs it. The dialog shows progress.
- **(+)** does nothing while the download is in progress.
- On success, the dialog asks for **A** to close the application. Start it again from the Homebrew Menu.
- On failure, **A** retries.

The installer uses `nexstatus.zip` and replaces both `switch/nexstatus.nro` and `switch/.overlays/nexstatus.ovl`.

## Android app

The Android build shows the same public counts, opens a game page with lobbies and players after Nextendo sign-in, and can load an eShop-style about page with screenshots. Account, language (system / Russian / English), about-app details, and signed-in game history live on the Account tab.

![Android games](screenshots/android/games.jpg)

![Android lobby](screenshots/android/lobby.jpg)

![Android about game](screenshots/android/about-game.jpg)

![Android account](screenshots/android/account.png)

### Updates (Android)

On launch the app checks GitHub for a newer Android build (`android-version.txt` + `nexstatus.apk`). If one is available, a dialog offers **Update** or **Later**. Later dismisses it until the next cold start. Update downloads the APK with a progress bar, percent, and speed, then opens the system install screen.

## Overlay

Open Ultrahand, then NexStatus. Set **Overlay memory** to **8 MB** before use (see Install). The 4 MB default is too small and can fail to load the overlay or crash Atmosphere. The overlay lists the same servers as the homebrew app, but only games with at least one player, busiest first. **Refresh** reloads the list. It does not reload on its own, and it does not request counts while the overlay is closed.

**A** on a game shows the player count, lobbies with the mode name, and every player. **A** on a lobby shows who is in that room. **Refresh** on that page reloads it. **B** returns to the list. Signed out, **A** tells you to sign in through the NexStatus homebrew app (hold **R**, launch a game, open the Homebrew Menu, start NexStatus, sign in, then open the overlay again).

## Build

### Switch

Install [devkitPro](https://devkitpro.org/) with `switch-dev`, `switch-sdl2`, `switch-sdl2_ttf`, `switch-libjpeg-turbo`, `switch-curl`, and `switch-mbedtls`. From the repository root:

```sh
make
```

This builds the application and the overlay and writes `nexstatus.zip` at the repository root. `make clean` removes the build output and the zip.

`APP_VERSION` in the root Makefile is the version stored in the Switch application. A release tag must be newer than the installed version for the console to offer the update.

### Android

From `android/`:

```sh
./gradlew :app:assembleRelease
```

`versionName` / `versionCode` live in `android/app/build.gradle.kts`. GitHub releases attach `nexstatus.apk` and `android-version.txt` so the phone can detect updates independently of the Switch tag number.

## License

GNU General Public License v2.0. See [LICENSE](LICENSE).

The overlay UI uses [libtesla](https://github.com/WerWolv/libtesla), which includes Sean Barrett's stb_truetype (public domain).

## Acknowledgements

- [devkitPro](https://devkitpro.org/) and [libnx](https://github.com/switchbrew/libnx)
- [SDL2](https://www.libsdl.org/) and SDL2_ttf
- [libcurl](https://curl.se/) and [libjpeg-turbo](https://libjpeg-turbo.org/)
- [WerWolv](https://github.com/WerWolv), for libtesla
- [ppkantorski](https://github.com/ppkantorski/Ultrahand-Overlay), for Ultrahand
- [Nextendo](https://nextendo.network), for the status API
