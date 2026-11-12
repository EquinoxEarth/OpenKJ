# Coordinated releases

The **Cross-platform release** workflow runs **Windows Build**, **macOS Build**
and **Linux Build** from one commit. Windows builds the x64 portable ZIP and
MSI. macOS builds an Apple Silicon DMG. Linux builds a Fedora 44 x86_64 portable
tarball and an x86_64 Flatpak.

1. Update `src/okjversion.h`, both release-note files, and the first AppStream
   release in `src/org.openkj.OpenKJ.metainfo.xml`. For a stable release, use
   `OKJ_VERSION_BRANCH "stable"`, a `stable` AppStream entry and a tag of the
   form `vX.Y.Z`.
2. Commit the changes on `master`. Run **Cross-platform release** manually on
   the proposed commit and download its `OpenKJ-release-candidate` artifact.
   This run builds and verifies the same versioned assets used for publication.
3. Review the notes, third-party redistribution requirements and license files.
   Test the Windows MSI wizard, macOS DMG and Linux packages on target machines.
   Windows packages are unsigned, and the macOS bundle uses ad hoc signing
   without notarization.
4. Push an annotated tag such as `v3.0.0` pointing to the reviewed commit.
   After all build and installation checks pass, the workflow verifies
   provenance and SHA-256 checksums, uploads the packages to a draft and then
   publishes it. Stable tags produce a regular GitHub release marked Latest.

The Fedora archive requires Fedora system dependencies. The Flatpak uses the
stable application branch and requires its Flatpak runtime. Each package ships
with a SHA-256 file and the release includes a `SHA256SUMS` file covering its
packages and metadata. Diagnostics stay in GitHub Actions.

Beta and preview tags remain supported with their matching source channel.
Standalone platform preview tags still use the platform workflows. If upload
fails after draft creation, inspect the draft and failed run before retrying.
The publisher refuses to overwrite an existing release or its assets.
