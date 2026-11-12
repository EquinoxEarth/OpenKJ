# OpenKJ 3.0.0

OpenKJ 3.0.0 is the first stable release of this Qt 6 port. It includes the changes since 2.2.8 Beta 1 and brings the Windows, macOS and Linux packages together in one release.

## Downloads

| Platform | Package | How to use it |
| --- | --- | --- |
| Windows x64 | `OpenKJ-3.0.0-windows-x64.msi` | Run the installer for an installed copy. |
| Windows x64 | `OpenKJ-3.0.0-windows-x64.zip` | Extract the entire archive and run `openkj.exe`. Keep the bundled DLLs and plugins together. |
| macOS Apple Silicon | `OpenKJ-3.0.0-macos-arm64.dmg` | Open the DMG and copy OpenKJ to Applications. The app uses ad hoc signing and has no Apple notarization. |
| Linux x86_64 | `OpenKJ-3.0.0-linux-x86_64.flatpak` | Install with `flatpak install --user ./OpenKJ-3.0.0-linux-x86_64.flatpak`. Launch with `flatpak run org.openkj.OpenKJ//stable`. |
| Fedora 44 x86_64 | `OpenKJ-3.0.0-linux-fedora44-x86_64-portable.tar.gz` | Extract and run `./run-openkj.sh`. Install the Fedora runtime dependencies listed in the included `LINUX_DEVELOPMENT.md`. |

Windows packages are unsigned. Back up your OpenKJ data before upgrading, especially if you use a large library or external media. Download the package's SHA-256 file to verify it, or use the release-wide `SHA256SUMS` file. Linux users on other distributions can use the Flatpak with the KDE runtime from Flathub. An installed preview Flatpak can coexist with the stable branch.

## Singer and Rotation Displays

- Added a configurable audience-facing Rotation Display that shows the current singer, upcoming singers and synchronized karaoke lyrics or video. [`3ce08430`](https://github.com/EquinoxEarth/OpenKJ/commit/3ce08430)
- Added a setting to hide parenthetical notes in names shown to the audience while preserving the host's singer records. [`a0989647`](https://github.com/EquinoxEarth/OpenKJ/commit/a0989647)
- Moved Rotation Display settings beside Display, fixed settings ownership in the video views and reused the settings object across repaints. [`ff092389`](https://github.com/EquinoxEarth/OpenKJ/commit/ff092389), [`3c86ca8e`](https://github.com/EquinoxEarth/OpenKJ/commit/3c86ca8e)
- Preserved display preferences across app rebuilds, capped the size of singer rows in short rotations and gave lyrics more of the window width. Added fallback handling for unavailable monitor selections. [`28283d14`](https://github.com/EquinoxEarth/OpenKJ/commit/28283d14), [`18ddbadd`](https://github.com/EquinoxEarth/OpenKJ/commit/18ddbadd)
- Matched the Preview and Rotation Display settings backgrounds to the rest of Settings. Added an accent colour for the current singer and upcoming position numbers, a background image, and tighter framing for both CDG and video. [`b197c390`](https://github.com/EquinoxEarth/OpenKJ/commit/b197c390)
- Added a 16:9 resize button beside fullscreen on the Singer Display. [`20af2ba3`](https://github.com/EquinoxEarth/OpenKJ/commit/20af2ba3)
- Added a customizable idle image or logo for the Rotation Display video area, with the OpenKJ logo as the default. Added singer-row opacity, background dimming and optional looping video backgrounds for the Rotation and Singer Displays. [`413f2dff`](https://github.com/EquinoxEarth/OpenKJ/commit/413f2dff)

## Playback, audio and preview

- Added a dedicated audio output for Song Preview under Settings → Audio → Preview. The default output is None and preview starts muted by default. The preview window has a mute button and volume slider for monitoring on a separate device. The settings panel advises using an output separate from karaoke and break music. [`292d9511`](https://github.com/EquinoxEarth/OpenKJ/commit/292d9511)
- Fixed karaoke video ending early when silence detection is enabled. MP4 playback now runs through the end of the video stream. [`07d00ebc`](https://github.com/EquinoxEarth/OpenKJ/commit/07d00ebc)
- Improved recording startup and shutdown, and added an independent karaoke mix for recording. [`2d611e02`](https://github.com/EquinoxEarth/OpenKJ/commit/2d611e02)
- Moved library scanning and song transitions off the UI thread. Coalesced video frame updates and shared rendering work across displays. [`83011a79`](https://github.com/EquinoxEarth/OpenKJ/commit/83011a79), [`70f3a069`](https://github.com/EquinoxEarth/OpenKJ/commit/70f3a069)

## Rotation, queues and play history

- Unified rotation ordering so the next singer shown on the display agrees with the singer selected by autoplay. [`f9f75efe`](https://github.com/EquinoxEarth/OpenKJ/commit/f9f75efe)
- Discarded stale queue responses when switching singers and guarded queue edits during the switch. [`b39d4af8`](https://github.com/EquinoxEarth/OpenKJ/commit/b39d4af8)
- Recorded karaoke plays after playback starts so songs that fail to start do not create play history entries. [`3b5c7556`](https://github.com/EquinoxEarth/OpenKJ/commit/3b5c7556)

## Library, startup and data integrity

- Matched source-folder paths literally and preserved entries for files on unavailable drives. Guarded stale database snapshots and rolled back failed writes. [`cac73cfa`](https://github.com/EquinoxEarth/OpenKJ/commit/cac73cfa), [`ec42852d`](https://github.com/EquinoxEarth/OpenKJ/commit/ec42852d)
- Made startup database migrations atomic and made karaoke file renames recoverable if a later step fails. [`402e88a7`](https://github.com/EquinoxEarth/OpenKJ/commit/402e88a7), [`2cbcbb53`](https://github.com/EquinoxEarth/OpenKJ/commit/2cbcbb53)
- Kept single-instance protection active through shutdown, preserved startup logging when file logging fails, and applied safe startup mode across all windows. [`99d42e00`](https://github.com/EquinoxEarth/OpenKJ/commit/99d42e00), [`312b088d`](https://github.com/EquinoxEarth/OpenKJ/commit/312b088d), [`df20a436`](https://github.com/EquinoxEarth/OpenKJ/commit/df20a436)

## Songbook, downloads and updates

- Validated Songbook uploads and reported partial failures so hosts can see which uploads need attention. [`c31c1442`](https://github.com/EquinoxEarth/OpenKJ/commit/c31c1442)
- Validated song downloads and moved update checks for this fork to HTTPS. [`25d806ab`](https://github.com/EquinoxEarth/OpenKJ/commit/25d806ab)

## Packaging and verification

- The Windows package builds on the tested portable payload. MSI CI exercises installation, a simulated older-version upgrade, repair, installed runtime checks, downgrade rejection and uninstall. [`2764a13b`](https://github.com/EquinoxEarth/OpenKJ/commit/2764a13b)
- The Linux Flatpak includes the media plugins and is checked on Ubuntu and Fedora without a development SDK. The Fedora portable archive packages the application and extra plugins for use with Fedora's system libraries. [`9890c86b`](https://github.com/EquinoxEarth/OpenKJ/commit/9890c86b), [`34893b3c`](https://github.com/EquinoxEarth/OpenKJ/commit/34893b3c)
- The release workflow stages all five packages from one source commit, verifies their checksums and publishes them together. [`f0feb6a7`](https://github.com/EquinoxEarth/OpenKJ/commit/f0feb6a7)
