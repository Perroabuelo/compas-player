# Experimental Compás / Rockbox selector

This is a Linux-hosted selector derived from Rockbox's `hibyos_linux.c`, not
a replacement for the device's pre-kernel U-Boot. The patch is pinned to
Rockbox commit `2b12d568ff32089a9efcbc2db95e9a18c7d9c68f` and retains its
GPL-2.0-or-later source notice. The build downloads the corresponding source
and keeps it with the generated files under `build/rockbox-bootloader/`.

Build explicitly with `make rockbox-bootloader BOARD=r1` or
`make rockbox-bootloader BOARD=r3proii`. `ROCKBOX_SOURCE` can name an existing
Rockbox Git checkout containing the pinned commit; it is cloned into the
build directory and is never modified. The ordinary `bootloader` target,
release workflows and firmware repacker continue to use the existing loader.
There is no verified upstream R3II 2025 target.

The combined selector draws the Compás splash and prepares the SD card before
checking for Rockbox. If a valid matching installation is missing and no
selector request is pending, it skips the menu and enters the shared Compás
loader directly. Otherwise the native Compás menu opens before Rockbox
initialization. Compás is always the initial selection. With a valid Rockbox
installation and no forced selector request, Compás starts after a one-second
countdown; volume input, touch input, or opening Tools cancels that countdown
for the rest of the selection session. A forced selector request opens the
menu without a countdown. Selecting Compás re-enters loader mode in the same
binary, before Rockbox initialization. The native Tools menu provides ADB
toggle, SD script, SD remount, filesystem repair, restart, shutdown and
recovery actions. Loader mode installs the canonical SD update, selects the
installed or packaged player, and supervises it with the normal crash, reboot,
shutdown and logging behavior. It does not invoke a separate
`compas_loader` executable. The player also retains its Settings > Updates >
Player Update action.
Rockbox payloads use the standard SD `/.rockbox/rockbox.r1` or
`/.rockbox/rockbox.r3proii` location and matching `rockbox-info.txt`. Dynamic
R1 payloads also require their companion `libasound.so.2`. Executables are
validated and staged atomically under persistent `/usr/data/.compas/rockbox/`, rather than
RAM-backed storage. A missing, invalid or unstaged payload falls back to
Compás. The source on the SD card is retained. A successful exec replaces
the selector; it does not remain as a supervisor.

Compás reboot requests ask BusyBox init to run its shutdown hooks, with a
bounded kernel reboot fallback. The shared supervisor remains active for both
the standalone loader and this optional selector.

This target is a prototype and is not enabled in firmware images. The built
`compas_rockbox_bootloader` must be installed as the device's Linux-hosted
bootloader by a separate, board-specific deployment step. U-Boot and release
packaging are unchanged. Test both menu choices, updates, crash recovery,
shutdown and card removal on each supported device before deployment.
