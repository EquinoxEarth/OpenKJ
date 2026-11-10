# Coordinated betas and previews

The main release path is `.github/workflows/release.yml`. An annotated tag such
as `v2.2.7-beta.1` builds macOS ARM64, Windows x64 ZIP/MSI, and Linux x86_64
Flatpak from the same commit. Beta tags use `vX.Y.Z-beta.N`; older preview tags remain supported. Packages
remain experimental and use GitHub's Pre-release flag until a stable release.

1. Update `src/okjversion.h`, both release-note files, and the first release in
   `src/org.openkj.OpenKJ.metainfo.xml`. Keep numeric versions consistent and set `OKJ_VERSION_BRANCH` to `beta` for
   beta tags or `prerelease` for preview tags.
2. Commit the changes on `master`. Run the Cross-platform release workflow
   manually on that commit for a complete rehearsal without publishing.
3. Review artifacts and check the Windows MSI wizard on a real Windows PC.
4. Push an annotated tag `vX.Y.Z-beta.N` pointing to the reviewed commit.
5. The reusable build workflows test their packages. Windows also tests install,
   upgrade, repair, installed runtime, downgrade rejection and uninstall. Flatpak
   also tests clean Ubuntu/Fedora installations without a development SDK.
6. Only after all jobs pass does the publisher verify source provenance and
   checksums, create a draft, attach all packages/metadata, and publish it with the
   matching beta/preview title and GitHub Pre-release flag. It never overwrites an existing release or existing assets.

Each package has a SHA-256 file; `SHA256SUMS` also covers release notes and build
metadata. ZIP/Flatpak names are versioned during release staging, after checking
their original CI checksums. The portable ZIP, installed MSI, DMG and Flatpak
are the distribution assets. Development builds and CI diagnostics stay in
Actions artifacts rather than the public release.

The older `macos-v...` and `windows-v...` tag paths remain available for platform
previews; they are not needed for a coordinated release. Selecting the new
workflow manually builds/tests all platforms and does not create a release.
If upload fails after draft creation, inspect the draft and failed run before
retrying; publishing refuses to overwrite that draft automatically. Never move
an already published tag to a different commit.
