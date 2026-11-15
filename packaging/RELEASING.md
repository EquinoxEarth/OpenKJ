# Coordinated releases

The **Cross-platform release** workflow runs **Windows Build**, **macOS Build**
and **Linux Build** from one commit. Windows builds the x64 portable ZIP and
MSI. macOS builds an Apple Silicon DMG. Linux builds a Fedora 44 x86_64 portable
tarball and an x86_64 Flatpak.

1. Update `src/okjversion.h`, both release-note files, and the first AppStream
   release in `src/org.openkj.OpenKJ.metainfo.xml`. For a stable release, use
   `OKJ_VERSION_BRANCH "stable"`, a `stable` AppStream entry and a tag of the
   form `vX.Y.Z`.
2. Commit the changes on `master`. Push an annotated tag such as `v3.0.0`
   pointing to the release commit. The tag run builds the five packages and
   saves them as the `OpenKJ-release-candidate` artifact in GitHub Actions.
   You can also dispatch a branch build with both inputs blank for a rehearsal.
3. Download the candidate from the successful tag run. Review the notes and
   license files, and test the Windows MSI wizard, macOS DMG and Linux packages
   on target machines. Windows packages are unsigned, and the macOS bundle
   uses ad hoc signing without notarization. Record the run ID and compute the
   SHA-256 of the downloaded `SHA256SUMS`. Compare it with the run summary.
4. Dispatch the workflow on the same tag with the reviewed run ID and manifest
   SHA-256. For example, from the downloaded candidate directory:

   ```sh
   gh workflow run release.yml --repo EquinoxEarth/OpenKJ --ref v3.0.0 \
     -f candidate_run_id=123456789 \
     -f manifest_sha256="$(shasum -a 256 SHA256SUMS | cut -d ' ' -f 1)"
   ```

   This run verifies the original build and uploads the reviewed files to a
   draft. It checks every uploaded SHA-256 before publishing. Stable tags
   produce a regular GitHub release marked Latest.

The Fedora archive requires Fedora system dependencies. The Flatpak uses the
stable application branch and requires its Flatpak runtime. Each package ships
with a SHA-256 file and the release includes a `SHA256SUMS` file covering its
packages and metadata. Diagnostics stay in GitHub Actions.

Beta and preview tags remain supported with their matching source channel.
Standalone platform preview tags still use the platform workflows. If a
coordinated upload is interrupted, dispatch it again with the same tag, run ID
and manifest SHA-256 while the artifact is available. The publisher verifies
the existing draft and uploaded assets, then uploads what is missing. It can
remove an empty placeholder left by a failed upload. An existing draft for a
different candidate, a changed asset, or a published release stops the workflow.
