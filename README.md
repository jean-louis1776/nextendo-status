# NexStatus

Nintendo Switch homebrew that shows which [Nextendo](https://nextendo.network) game servers are up and how many people are playing. The list refreshes every 15 seconds. No sign-in is required.

## Install

Copy `nexstatus.nro` to `sdmc:/switch/nexstatus.nro` and open it from the Homebrew Menu.

Releases are published on GitHub. When a newer one exists, NexStatus stops on a red screen. Press **A** to install it, then open the app again.

## Build

Install [devkitPro](https://devkitpro.org/) with `switch-dev`, `switch-sdl2`, `switch-sdl2_ttf`, `switch-libjpeg-turbo`, `switch-curl` and `switch-mbedtls`, then:

```sh
make -C app
```

The overlay in `overlay/` is a separate Tesla module and is not required for the status screen.

## License

NexStatus is released under the GNU General Public License v2.0. See [LICENSE](LICENSE).

The QR code generator in `app/source/qrcodegen.cpp` is copyright Project Nayuki and is used under the MIT license. The Tesla overlay uses [libtesla](https://github.com/WerWolv/libtesla), which includes Sean Barrett's stb_truetype (public domain).

## Acknowledgements

- [devkitPro](https://devkitpro.org/) and [libnx](https://github.com/switchbrew/libnx)
- [SDL2](https://www.libsdl.org/) and SDL2_ttf
- [libcurl](https://curl.se/) and [libjpeg-turbo](https://libjpeg-turbo.org/)
- [Project Nayuki](https://www.nayuki.io/page/qr-code-generator-library), for the QR code generator
- [WerWolv](https://github.com/WerWolv), for libtesla
- [Nextendo](https://nextendo.network), whose public status API this app displays

NexStatus is not affiliated with Nintendo or Nextendo.
