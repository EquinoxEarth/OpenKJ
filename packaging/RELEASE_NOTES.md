# OpenKJ 3.1.0

OpenKJ 3.1.0 is a stable release for Windows, Apple Silicon macOS and Linux. It adds paused singers, optional audio normalization and a Rotation Display queue summary. It also improves playback transitions and makes several library and queue operations safer.

## Downloads

| Platform | Package | How to use it |
| --- | --- | --- |
| Windows x64 | `OpenKJ-3.1.0-windows-x64.msi` | Run the installer. |
| Windows x64 | `OpenKJ-3.1.0-windows-x64.zip` | Extract the entire archive and run `openkj.exe` with the bundled DLLs and plugins in place. |
| macOS Apple Silicon | `OpenKJ-3.1.0-macos-arm64.dmg` | Open the DMG and copy OpenKJ to Applications. The app is ad hoc signed and has no Apple notarization. |
| Linux x86_64 | `OpenKJ-3.1.0-linux-x86_64.flatpak` | Install with `flatpak install --user ./OpenKJ-3.1.0-linux-x86_64.flatpak`, then launch with `flatpak run org.openkj.OpenKJ//stable`. The KDE runtime is available from Flathub. |
| Fedora 44 x86_64 | `OpenKJ-3.1.0-linux-fedora44-x86_64-portable.tar.gz` | Extract the archive, install the Fedora dependencies listed in the included `LINUX_DEVELOPMENT.md`, and run `./run-openkj.sh`. |

The Windows packages are unsigned. Back up your OpenKJ data before upgrading. Each package has a SHA-256 checksum, and `SHA256SUMS` lists the checksums for all five downloads.

## Singer rotation and audience display

- Pause a singer from the rotation context menu to move them to the **Paused Singers** list with their song queue intact. Open the list using the pause button at the bottom of the rotation panel. Resuming puts the singer after the on-deck singer by default. [`6c3ef939`](https://github.com/EquinoxEarth/OpenKJ/commit/6c3ef939)
- Pause directly from a singer row using the button between the regular-singer toggle and delete. The status bar now includes the paused singer count. [`6e99f549`](https://github.com/EquinoxEarth/OpenKJ/commit/6e99f549)
- Check **Up Next** next to **Resume** to put a singer immediately after the current singer. The checkbox starts unchecked when opening the dialog. [`665e937f`](https://github.com/EquinoxEarth/OpenKJ/commit/665e937f)
- Optionally show the total singers in the queue and estimated rotation duration at the bottom right of the Rotation Display. The setting is off by default. [`04d5ba79`](https://github.com/EquinoxEarth/OpenKJ/commit/04d5ba79)
- Refresh rotation estimates when relevant settings or queued song durations change. [`107595a1`](https://github.com/EquinoxEarth/OpenKJ/commit/107595a1)

## Audio normalization

- Enable normalization separately for Karaoke and Break Music under **Settings → Audio**. Normalization reduces the level of louder tracks and leaves quieter tracks at their original level. Playback analysis saves track gain in a cache beside the song database so it can be reused. Clearing the song database also clears that cache. [`eb996ab5`](https://github.com/EquinoxEarth/OpenKJ/commit/eb996ab5)
- With Karaoke normalization enabled, uncached library tracks are analyzed in the background after the duration scan. The database status identifies the **Durations** and **Normalization** stages and shows the remaining work. [`c5581e2d`](https://github.com/EquinoxEarth/OpenKJ/commit/c5581e2d)
- Pause background analysis during karaoke playback, report decode, timeout and cache-write problems, and show whether normalization is on in the main window status bar. [`ff310acf`](https://github.com/EquinoxEarth/OpenKJ/commit/ff310acf)

## Playback and media compatibility

- Restore the selected Karaoke, Break Music and Preview output devices when their pipelines are recreated after playback stops. [`57a08b33`](https://github.com/EquinoxEarth/OpenKJ/commit/57a08b33)
- Handle karaoke startup and playback failures more reliably and preserve the handoff between Break Music and Karaoke during repeated starts and stops. [`1291ef44`](https://github.com/EquinoxEarth/OpenKJ/commit/1291ef44)
- Restore WAV playback in the packaged macOS app and add smoke checks for the bundled media runtime. [`1fa7e26b`](https://github.com/EquinoxEarth/OpenKJ/commit/1fa7e26b)
- Open karaoke ZIP archives whose Windows paths contain Unicode characters. [`f0150342`](https://github.com/EquinoxEarth/OpenKJ/commit/f0150342)
- Prepare CD+G media on a worker thread for song startup so extraction and file preparation leave the interface responsive. [`b51002ef`](https://github.com/EquinoxEarth/OpenKJ/commit/b51002ef)

## Library, queues and history

- Make library clearing and playlist deletion transactional. Clearing the karaoke library preserves singer history. [`ded3d48c`](https://github.com/EquinoxEarth/OpenKJ/commit/ded3d48c)
- Save the karaoke play count and singer playback bookkeeping in one transaction so related updates succeed or roll back together. [`8906b131`](https://github.com/EquinoxEarth/OpenKJ/commit/8906b131)
- Make karaoke file removal recoverable if a related step fails. [`efd7c4a6`](https://github.com/EquinoxEarth/OpenKJ/commit/efd7c4a6)
- Verify media identity before automatically relinking moved library entries, reducing the risk of matching a different file with the same name. [`db15ee04`](https://github.com/EquinoxEarth/OpenKJ/commit/db15ee04)
- Add indexes for queue lookups and consolidate rotation loading so the application can retrieve singer and queue information with fewer database queries. [`8227b6c8`](https://github.com/EquinoxEarth/OpenKJ/commit/8227b6c8)
- Import regular singers and their associated data transactionally so failed imports can roll back together. [`620df4a9`](https://github.com/EquinoxEarth/OpenKJ/commit/620df4a9)

## Songbook and release verification

- Bound Songbook polling and connection-test requests so timeouts and slow responses do not pile up or hang the settings interface. [`faf4c736`](https://github.com/EquinoxEarth/OpenKJ/commit/faf4c736)
- Stage the five platform packages from the tagged build and verify the candidate run and checksums before publishing the GitHub release. [`205409c4`](https://github.com/EquinoxEarth/OpenKJ/commit/205409c4)
