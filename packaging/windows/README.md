# Windows release directory

`cmake --install build/windows-release --prefix stage` produces the KVMux executable and
its local runtime layout. The release workflow must add the FFmpeg shared DLLs
that match the configured development package, together with their license and
source/build-offer files. KVMux does not download drivers or DLLs.

Media Foundation is a Windows component. CH343/CH340 driver installation and
CH9329 hardware configuration are external hardware-setup steps, not package
installation actions.
