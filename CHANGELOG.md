# Changelog

## 1.3.0

Switch homebrew and overlay. Android is 1.2.2.

- Major redesign of the homebrew app. Dark glass panels and the same accent colors as Android. The menu sits in a column on the left (Servers, Account, Credits), and the list or page is on the right.
- Button hints are white circles with a centered letter, plus, or minus.
- The account card is only as tall as the name and friend code. Lobby cards in a row share the height of the taller card. The room name and places sit on the first line, the mode and state on the second.
- A player who is not in a lobby is shown by name only. Lobby N appears only when they are in a room. The same rule is on Android.
- Credits show the author photo and the version. The version left the bottom bar.
- The server list scrolls far enough that the last game is fully on screen.
- Signing out while the account is still loading shows Sign in, not the previous name.
- The overlay stores the full renewed sign-in token, so lobbies keep loading after the old token expires.

Set Ultrahand **Overlay memory** to **8 MB** before opening the overlay. The 4 MB default is too small and can fail to load the overlay or crash Atmosphere.

## 1.2.2

Switch homebrew and overlay. Android is 1.2.1.

- The overlay renews an expired sign-in token on its own and loads lobbies again. The game list was unaffected, because it does not use that token.
- Background music is included in the homebrew app and on Android. It starts on, and the choice is remembered. Minus turns it off and on in the homebrew app. On Android, Account has a Music switch under Language. The Android track pauses while the app is in the background.
- On the console the track is decoded ahead of playback on its own thread, so the interface stays responsive while the music plays.
- Credits open from a button on the homebrew banner, in a card over the list.

Set Ultrahand **Overlay memory** to **8 MB** before opening the overlay. The 4 MB default is too small and can fail to load the overlay or crash Atmosphere.

## 1.2.1

Switch homebrew and overlay. Android is 1.2.0.

- The homebrew list includes all 63 games from the status page, including Portal 2. Servers that do not answer stay in the list as Offline. If a status cannot be read, the game shows No status.
- Player counts use the same sources as the website. Catalog games, including Stardew Valley, use the service catalog. When signed in, the number matches the live player list, including someone who is in the game but not in a lobby.
- Under the player total, homebrew and Android show Servers, then Online and Offline. Offline includes games with no status, so the two numbers add up to the full list.
- The overlay still lists only games with at least one player. A game page always shows Lobbies. If there are none, it says there are no lobbies right now, then lists the players.

Set Ultrahand **Overlay memory** to **8 MB** before opening the overlay. The 4 MB default is too small and can fail to load the overlay or crash Atmosphere.

## 1.2.0

Switch homebrew and overlay.

- The overlay lists the same servers as the homebrew app. Only games with at least one player are shown.
- **A** on a game shows the player count, lobbies with the mode name, and every player. **A** on a lobby shows who is in that room.
- **Refresh** reloads the list or the open game. Nothing reloads on its own.
- Signed out, **A** explains how to sign in through the NexStatus homebrew app from the Homebrew Menu.

Set Ultrahand **Overlay memory** to **8 MB** before opening the overlay. The 4 MB default is too small and can fail to load the overlay or crash Atmosphere.

## 1.1.12

Switch homebrew and overlay.

- The overlay lists the same servers as the homebrew app, including the extra count sources (game-stats, service catalog, UNO, Crypt of the NecroDancer, SpeedRunners 2). Only games with at least one player are shown.
- **A** on a game opens lobbies with the mode name and the players in that room. The page loads once when opened.
- Signed out, **A** explains how to sign in through the NexStatus homebrew app from the Homebrew Menu.

Set Ultrahand **Overlay memory** to **8 MB** before opening the overlay. The 4 MB default is too small and can fail to load the overlay or crash Atmosphere.
