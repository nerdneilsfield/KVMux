# KVMux packaging

KVMux installs a native executable on Windows/Linux and a standard `.app` on
macOS. It does not download runtimes, drivers or firmware. A release package
must include FFmpeg's actual dynamic runtime libraries and notices/source offer
for the exact distributed FFmpeg build; vendored dependency records and
licenses are in `third_party/`.

The macOS install step copies non-system dynamic dependencies into
`kvmux.app/Contents/Frameworks`, rewrites their install names, and applies an
ad-hoc signature so the locally staged bundle passes `codesign --verify`.
No Developer ID certificate or notarization credential is stored in this
repository. A release for other Macs must be signed with a Developer ID
identity and notarized outside this build.
