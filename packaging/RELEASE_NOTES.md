# OpenKJ 3.2.0

OpenKJ 3.2.0 is a stable release for Windows, Apple Silicon macOS and Linux. It adds a horizontal singer ticker, improves Rotation Display video backgrounds, creates Break Music playlists from library folders, and adds search to singer history. This changelog covers changes since 3.1.0.

## Downloads

| Platform | Package | How to use it |
| --- | --- | --- |
| Windows x64 | `OpenKJ-3.2.0-windows-x64.msi` | Run the installer. |
| Windows x64 | `OpenKJ-3.2.0-windows-x64.zip` | Extract the entire archive and run `openkj.exe` with the bundled DLLs and plugins in place. |
| macOS Apple Silicon | `OpenKJ-3.2.0-macos-arm64.dmg` | Open the DMG and copy OpenKJ to Applications. The app is ad hoc signed and has no Apple notarization. |
| Linux x86_64 | `OpenKJ-3.2.0-linux-x86_64.flatpak` | Install with `flatpak install --user ./OpenKJ-3.2.0-linux-x86_64.flatpak`, then launch with `flatpak run org.openkj.OpenKJ//stable`. The KDE runtime is available from Flathub. |
| Fedora 44 x86_64 | `OpenKJ-3.2.0-linux-fedora44-x86_64-portable.tar.gz` | Extract the archive, install the Fedora dependencies listed in the included `LINUX_DEVELOPMENT.md`, and run `./run-openkj.sh`. |

The Windows packages are unsigned. Back up your OpenKJ data before upgrading. Each package has a SHA-256 checksum, and `SHA256SUMS` lists the checksums for all five downloads.

## Horizontal singer ticker

- Choose **Settings → Rotation Display → Singer list style → Horizontal ticker** to scroll upcoming singers from right to left along the bottom. It includes every eligible singer in rotation order and honors the empty-singer setting. Paused singers are excluded. [`0c6a62ec`](https://github.com/EquinoxEarth/OpenKJ/commit/0c6a62ec)
- The strip loops continuously and repeats short rotations across its width, including a single singer. Rounded cards use the existing text colour, accent colour, row opacity, and first-singer highlight, with padding before each number and after each name. [`0c6a62ec`](https://github.com/EquinoxEarth/OpenKJ/commit/0c6a62ec)
- Lyrics and video are centered above the strip with margins that reveal the configured background. Now Singing stays above the lyrics and the optional rotation summary sits below the ticker. The vertical list is the default. Switching back restores its saved singer limit and left/right video position. [`0c6a62ec`](https://github.com/EquinoxEarth/OpenKJ/commit/0c6a62ec)
- The ticker has its own cached rendering layer. Native macOS playback uses display refresh callbacks to present the ticker and background together. Other platforms use Qt's animation clock. [`0c6a62ec`](https://github.com/EquinoxEarth/OpenKJ/commit/0c6a62ec)

## Rotation Display video backgrounds

- Prepare frame conversion, scaling, cropping, and dimming on a worker thread. The display paints the prepared frame at the window's pixel size, reducing work on the UI thread as the window gets larger. Preparation keeps only the newest waiting frame and rejects obsolete results after changing backgrounds. [`0c6a62ec`](https://github.com/EquinoxEarth/OpenKJ/commit/0c6a62ec)
- Video files up to **256 MiB** load into memory asynchronously and loop from that buffer. Larger files, read failures, and media backends that cannot play from the buffer fall back to file playback. The cache contains the compressed video file, with frames decoded during playback. [`0c6a62ec`](https://github.com/EquinoxEarth/OpenKJ/commit/0c6a62ec)
- Hiding the Rotation Display pauses its background video and retains the source, cached bytes, and displayed frame. Reopening resumes playback and can show the retained frame immediately. Images continue to use a retained pixmap. These changes apply to the Rotation Display background. [`0c6a62ec`](https://github.com/EquinoxEarth/OpenKJ/commit/0c6a62ec)

## Break Music playlists from folders

- Added **Break Music → Playlist → Create From Folder…** beside M3U import. Choose a folder to collect tracks already indexed in the Break Music library, including subfolders, in path order. The folder name is suggested for the playlist title. [`0eed3139`](https://github.com/EquinoxEarth/OpenKJ/commit/0eed3139)
- Enter a new title or select an existing playlist to append to it. Repeated folder imports skip tracks already in the destination. A folder with no indexed matches produces a message and leaves playlist creation until tracks are available. [`0eed3139`](https://github.com/EquinoxEarth/OpenKJ/commit/0eed3139)
- Imports run in the background using saved library records and save changes transactionally. The current playlist selection is preserved while Break Music is playing or paused. Appending to the active playlist preserves its current track and respects Stop After Current. [`0eed3139`](https://github.com/EquinoxEarth/OpenKJ/commit/0eed3139)
- Folder matching treats punctuation literally and respects directory boundaries, including Unicode names. M3U import continues to support intentional repeated tracks. [`0eed3139`](https://github.com/EquinoxEarth/OpenKJ/commit/0eed3139)

## Singer history search

- Added **Search history** after the Double click dropdown at the bottom of History, with Clear immediately before Add to Queue. It matches artist, title, disc ID, and filename using case-insensitive search terms and the library's punctuation handling. [`bac03a9d`](https://github.com/EquinoxEarth/OpenKJ/commit/bac03a9d)
- Results update as you type when progressive search is enabled. Press Enter to search when it is disabled. Clear and Escape restore full history and return focus to search. Switching singers clears the filter, while refreshing the same singer retains it. [`bac03a9d`](https://github.com/EquinoxEarth/OpenKJ/commit/bac03a9d)
- Filtered and sorted results retain their song identity for preview, playback, and queue actions. Adding a history track preserves its saved key change. Queue and delete actions capture selected records before dialogs or model reloads can change row positions. [`bac03a9d`](https://github.com/EquinoxEarth/OpenKJ/commit/bac03a9d)

## Release publishing

- Resolve the fully qualified GitHub tag reference when validating a release candidate. The publisher checks the reviewed source commit and all asset checksums before publication. [`305b0758`](https://github.com/EquinoxEarth/OpenKJ/commit/305b0758)
