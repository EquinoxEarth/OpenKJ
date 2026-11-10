# OpenKJ 2.2.7 — Windows x64 Beta

Experimental, unsigned Windows x64 beta using Qt 6 and GStreamer.

Changes since 2.2.6:

- Faster large-library searches and indexed break-music lookups.
- Background break-music playlist imports and database scans, with scan progress
  and cancellation.
- Volume changes preserve pending stops and fades.
- OpenKJ logo startup splash and initialization timing logs.
- Curated set of 34 GStreamer plugins, preserving SoundTouch, EQ, CDG and native
  Windows audio outputs. ASF parsing supports WMV/WMA playback.
- Isolated bundle checks decode synthetic MP3, FLAC, Vorbis/Opus Ogg, M4A, MP4,
  MKV, AVI, MPEG program/transport streams, WMV and WMA with a fresh plugin cache.
- Updated Qt APIs, directory scans and logging dependencies.

- Native Windows playback, full-library import and singer rotation have been
  tested by the maintainer.
- Correct Windows local-file URLs, including spaces and Unicode filenames.
- Audio format/rate conversion between the existing SoundTouch/EQ chain and
  native output devices.
- Portable ZIP with Qt, GStreamer, FFmpeg, SoundTouch and runtime DLLs;
  MSYS2 is not required on the destination computer.
- Isolated checks cover SQLite, ZIP I/O, file playback, DSP/output negotiation,
  CDG decoding and bundled plugin discovery without development SDK paths.

Extract the **entire ZIP**, then open `openkj.exe`. Keep all DLLs and plugin
folders together. This uses your normal OpenKJ settings/library; back up your
existing data before trying a preview. Windows may prompt because the build is
unsigned.

Alternatively, run the **MSI installer** for a Start Menu entry, optional desktop
shortcut and uninstall support. Installation requires administrator approval;
settings, the song database and media are retained when uninstalling. MSI
upgrades apply to this installer family, not older Inno Setup installations.
No automatic updater is included.

Automated tests use offscreen windows and fake audio/video outputs; they do not
certify every audio device, microphone or multi-monitor configuration. Please
report playback, display, recording or library issues with the application log.

The archive includes dependency versions, license notices and a file manifest.
Source and build scripts: [EquinoxEarth/OpenKJ](https://github.com/EquinoxEarth/OpenKJ).
