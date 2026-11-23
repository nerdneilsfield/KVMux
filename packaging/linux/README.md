# Linux release directory

`cmake --install build/linux-release --prefix stage` produces the KVMux executable.
Releases ship two kinds of Linux package, each for amd64 and arm64:

- `.deb` for Ubuntu 24.04 and 26.04, built with `cpack -G DEB` against that
  release's own FFmpeg and libusb. `dpkg-shlibdeps` writes the matching
  `Depends`, so apt installs them. The package installs to `/opt/kvmux` with
  its own `libSDL3.so.0` and does not register the desktop entry.
- A Homebrew archive linked against Homebrew's `ffmpeg` and `libusb`, whose
  library directory stays in the executables' RUNPATH. It needs Homebrew at
  `/home/linuxbrew/.linuxbrew` with those formulae, and glibc 2.39 or newer
  (built on Ubuntu 24.04), but not the distribution's FFmpeg.

Both need the system OpenGL and X11/Wayland libraries. No AppImage or Flatpak
is produced.

The V4L2 adapter supports common single-plane UVC capture nodes. Permissions
for `/dev/video*` and `/dev/tty*` are host policy; the package does not change
them. Both X11 and Wayland require separate real-window/input validation.
