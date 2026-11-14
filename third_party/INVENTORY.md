# Third-party inventory

The listed archives were acquired explicitly while updating dependencies. CMake never downloads them.

| Dependency | Version | Commit | Archive SHA-256 | License |
| --- | --- | --- | --- | --- |
| SDL | `release-3.4.16` | `fa2c02bb6e21974a89ea9824bc53c9932abe5f9c` | `d246967187a4f37cc850608dbaf5cff9d1df4d731259903d25539755cdfcc657` | `SDL/LICENSE.txt` |
| imgui | `v1.92.9b` | `f1cc2ae15e53a861a874c3034aae6798fde194ab` | `f30be7f3e2136615b1848a9afb72f6dfd8a60820ddf1c1a85997bf3b722e7cfc` | `imgui/LICENSE.txt` |
| json | `v3.12.0` | `55f93686c01528224f448c19128836e7df245f72` | `67f4cdd9ca930c9c1e130af4a437c7fc98fab77a2846fc2d2a14b4943831f8ef` | `nlohmann_json/LICENSE.MIT` |
| spdlog | `v1.17.0` | `79524ddd08a4ec981b7fea76afd08ee05f83755d` | `c6dee844e35a54cc80e3ab76aae2310763e0b49c7397549dfa4d4657d9737138` | `spdlog/LICENSE and spdlog/include/spdlog/fmt/bundled/fmt.license.rst` |
| glad | `v2.0.8` | `73db193f853e2ee079bf3ca8a64aa2eaf6459043` | `33dbeae44d8315ece57e14eba4b1b4a02ac5406d1c4f49cd20048c91522a6b9a` | `glad/LICENSE and notices in generated files` |

## libserialport

- Version/tag: `libserialport-0.1.2`
- Commit: `bc8f88ef3399b76da7992a99badebada419d0e19`
- Archive: `https://github.com/sigrokproject/libserialport/archive/bc8f88ef3399b76da7992a99badebada419d0e19.tar.gz`
- SHA-256: `8850669c1867e466d87edfbd2120bac5b35f8e6df2c330b0e77e083a52249b28`
- License: LGPL-3.0-or-later, retained as `libserialport/COPYING`
- Local changes: none to upstream sources; KVMux supplies its CMake build.

## KCP

- Source: https://github.com/skywind3000/kcp
- Commit: `32da082e529a26730aea3eb19922f80634ee6dea`
- License: MIT, retained as `kcp/LICENSE`
- Local changes: none; only `ikcp.c`, `ikcp.h`, and `LICENSE` vendored.
- File SHA-256 (unchanged upstream bytes):
  - `kcp/ikcp.c`: `dfd4a9a1c0c928238b10be2b0f10d178b314bccc67ca577f9da8455a1fa0b77d`
  - `kcp/ikcp.h`: `531991aa43f1055d6e9ddf233214bce9e66d63fd55fe95b55a65f295252f6692`
  - `kcp/LICENSE`: `6587fd02a924b97dbd6758b0573ddec056fb494a84f3709c5aaa33ce1f7f029c`

## FFmpeg runtime (not vendored)

- Windows CI SDK: Gyan FFmpeg `8.0.1`, `ffmpeg-8.0.1-full_build-shared.zip`.
- Archive: https://github.com/GyanD/codexffmpeg/releases/download/8.0.1/ffmpeg-8.0.1-full_build-shared.zip
- SHA-256: `e4a40b46e3a3f3e5f2ed28352bd1fd6c733ae3808a8743682ef7f0d4e20c7d51`.
- The full shared build includes GPL components. Installation preserves the SDK's
  root LICENSE and README files under `share/kvmux/licenses/ffmpeg`.
- Before distributing binaries, retain the exact build configuration, matching
  FFmpeg and bundled codec-library sources, patches and build instructions, or a
  valid corresponding-source offer. A link to upstream alone is not a source offer.
- macOS/Linux use host FFmpeg libraries; their version, enabled features and
  license obligations depend on the host package. No FFmpeg sources are modified
  by KVMux, and configure does not download FFmpeg.

## Inter

- Version: `4.1`, released 2024-11-15.
- Upstream archive: `https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip`
- Retained subset: `fonts/Inter-Regular.ttf` and `fonts/LICENSE.txt`. The
  variable fonts and `Inter.ttc` are not vendored.
- SHA-256:
  - `fonts/Inter-Regular.ttf`: `40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82`
  - `fonts/LICENSE.txt`: `262481e844521b326f5ecd053e59b98c8b2da78c8ee1bdbb6e8174305e54935a`
- License: SIL Open Font License 1.1, retained as `fonts/LICENSE.txt`.
- Local changes: none. The GUI embeds the TTF with `AddFontFromMemoryTTF`.
  CMake does not download the font. CJK glyphs stay on a system font.

## libusb runtime (not vendored)

- Not downloaded by CMake and not stored under `third_party/`.
- License: LGPL-2.1-or-later. Upstream `COPYING` is the GNU Lesser General
  Public License, version 2.1, and `libusb.h` grants version 2.1 or any later
  version. KVMux does not modify libusb.
- Development verified with 1.0.27 (`pkg-config --modversion libusb-1.0` on the
  Linux host used for this change). Use 1.0.27 or newer. Ubuntu 24.04's
  `libusb-1.0-0-dev` is 1.0.27. A macOS Homebrew install may be newer.
- Linux and macOS: pkg-config module `libusb-1.0`.
- Windows CI binary: official `libusb-1.0.27.7z`.
  Archive: https://github.com/libusb/libusb/releases/download/v1.0.27/libusb-1.0.27.7z
  SHA-256: `19835e290f46fab6bd8ce4be6ab7dc5209f1c04bad177065df485e51dc4118c8`.
  That archive has the DLL and import library. It does not include `COPYING`.
- Windows license text: official source `libusb-1.0.27.tar.bz2`.
  Archive: https://github.com/libusb/libusb/releases/download/v1.0.27/libusb-1.0.27.tar.bz2
  SHA-256: `ffaa41d741a8a3bee244ac8e54a72ea05bf2879663c098c82fc5757853441575`.
  CI copies `libusb-1.0.27/COPYING` to the root of `LIBUSB_ROOT`. The Windows
  build then copies `libusb-1.0.dll` and that `COPYING` next to the executables.
- A binary distribution must ship the LGPL notice and a corresponding-source
  offer for the libusb version that was actually linked. A link to upstream
  alone is not that offer.
