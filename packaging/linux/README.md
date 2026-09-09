# Linux release directory

`cmake --install build/release --prefix stage` produces the KVMux executable.
The target Ubuntu 24.04 installation must provide SDL/OpenGL and FFmpeg runtime
libraries matching the package build. This first release does not create an
AppImage or Flatpak.

The V4L2 adapter supports common single-plane UVC capture nodes. Permissions
for `/dev/video*` and `/dev/tty*` are host policy; the package does not change
them. Both X11 and Wayland require separate real-window/input validation.
