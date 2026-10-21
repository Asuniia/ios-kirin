# ios-sailfish

**Apple's iOS 16.7.16 running on a Google Pixel 1 (`sailfish`, Snapdragon 821), with GPU-accelerated graphics.**

This project ports Apple's open-source XNU kernel (xnu-8796, the iOS 16 generation) to the first Google Pixel and runs the **unmodified iOS 16.7.16 (20H392) userland of the iPhone 8** on it: launchd, the system daemons, backboardd, SpringBoard and the stock apps. It is a fork of [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet), which brought the same kernel up on a Redmi Note 10 Pro; that history is kept, and the Pixel work starts at the commit "tools: granule-probe, 16K page check for other phones".

> Experimental, not a usable phone OS. Many things are missing or stubbed (see below) and Apple's security model is mostly switched off. It installs into the `userdata` partition and so **erases Android's user data**.

## Status (2026-10-21)

- Boots from `fastboot boot` to the lock screen in **about 20 seconds**.
- **GPU acceleration on the Adreno 530**: CoreAnimation's own OpenGL ES renderer runs on Mesa's freedreno driver, through our own kernel GPU driver.
- Display updates **up to 60 Hz**. The GPU scales each frame onto the panel and the display flips between two buffers, so there is no tearing and no CPU copy. The UI runs at roughly 15–35 frames/s depending on the screen: CoreAnimation's CPU work in backboardd is now the limit, not the GPU.
- Lock screen, home screen, App Library, Control Center, notification center, Settings, the stock iOS 16 wallpaper with its layered artwork, and **all stock Apple apps** (Weather, Calendar, Clock, Mail, Notes, Maps, Music, Calculator, Files, ...).
- Touchscreen, power / volume buttons, **real battery percentage and charging**.

## What works

| Area | State |
| --- | --- |
| Boot | Stock Pixel bootloader → `fastboot boot` of our shim → XNU. Nothing is flashed to boot partitions |
| CPU | All 4 Kryo cores via PSCI (1228 / 1363 MHz; optional turbo to 1594 / 2150 MHz with `sweetturbo=1`, runs hot), 16K pages, GICv3, timers |
| Memory | 2 GB (the upper DDR bank), DDR clocked to 1804 MHz through the RPM |
| Storage | Own UFS host driver; the iOS root is a journaled HFS+ volume in `userdata`, read/write |
| iOS userland | Real iPhone 8 16.7.16 root filesystem, cryptexes and shared cache (the userland is ARMv8.0-clean, so it runs on Kryo) |
| GPU | Adreno 530 at 600 MHz: power, clocks, SMMU, CP microcode, zap shader via TrustZone; buffer objects and command submission for user space. Mesa (freedreno, GLES 3.1) runs inside backboardd as `sweetgl.dylib`, behind an EAGL shim that CoreAnimation's GL renderer uses as if it were Apple's |
| Display | Command-mode Samsung panel through the MDP: iOS sees an iPhone 8 panel (750×1334 @2x), scaled (bilinear, on the GPU) to 1080×1920; double-buffered, up to 60 Hz |
| Touch | Synaptics RMI4 over I²C (BLSP QUP), rails switched on through the RPM, delivered as HID digitizer events |
| Buttons | Power, volume up, volume down = **Home** (PMIC/TLMM), as HID keys |
| Battery | Fuel gauge readout and charging at 1.5 A (PMI8994) |
| Device identity | iPhone 8 (`iPhone10,1` / `D20AP`), Home-button UI |
| Activation | mobileactivationd's own "Hactivation" path, chosen through device-tree security status. No Apple servers involved |

## What does not work (yet)

- **No Wi-Fi, Bluetooth, cellular, audio, camera, sensors, GPS, Touch ID or SEP.**
- **No hardware video decoder:** HEIC images and video do not decode. The stock wallpapers' HEIC layers are converted to PNG when the root is built (`tools/wallpaper-heic2png.sh`).
- **No Metal.** CoreAnimation falls back to its OpenGL ES renderer, which is what runs on the GPU. Apps that need Metal do not work.
- **No display sleep or brightness control:** the panel stays on.
- **No thermal management:** keep turbo off for normal use.
- **Security is off:** permissive code signing (AMFI) and sandbox policies, a stub AppleKeyStore, no data protection. Do not put anything personal on it.

## How it boots

```
Pixel bootloader (aboot, unlocked)
  -> fastboot boot pixel-boot.img             (temporary; nothing is flashed to boot)
     -> shim: stops the watchdog, loads the payload appended to the kernel image
        -> XNU Mach-O + generated Apple Device Tree (ADT) + boot-args + GPU firmware
           -> XNU -> UFS driver -> HFS+ root in the userdata partition
              -> dyld + shared cache -> launchd -> daemons -> backboardd (+ sweetgl.dylib) + SpringBoard
```

There is no serial console. The kernel log goes to a persistent 4 MiB ring at the end of `userdata` and to the RAM console (pstore) that survives a warm reset; both are read from TWRP (`tools/sweet-log-pull.sh`).

## How to do it yourself

You need a Google Pixel 1 (`sailfish`) with an **unlocked bootloader**, a Mac with Xcode 15.4 and the KDK 13.5 (for the kernel build), and [TWRP for sailfish](https://dl.twrp.me/sailfish/) (booted temporarily, never flashed). **Everything on `userdata` is lost.** Bootloaders, recovery, modem and persist are never touched. Keep your partition backups and any device dumps out of git (`.gitignore` covers `backup/`, `research/`, `logs/`).

1. **Kernel:** `tools/xnu-build-local.sh xnu/out-local-NNNN` builds XNU with all patches in `xnu/patches/` (blacktop/darwin-xnu-build, Xcode 15.4 and KDK 13.5 unpacked in `research/`).
2. **iOS root:** download the iPhone 8 IPSW `iPhone_4.7_P3_16.7.16_20H392` and build the image with `sudo tools/mkiosroot-mac.sh OS.dmg SYSTEMOS.dmg APPOS.dmg ios16-root.img`. Before flashing, attach it read-write on the Mac (`hdiutil attach -imagekey diskimage-class=CRawDiskImage ios16-root.img`) and add what the build script does not do yet:
   - the GL library: build it with `tools/sweetgl/build.sh` and copy `tools/sweetgl/out/sweetgl.dylib` to `/usr/lib/sweetgl.dylib` (mode 755);
   - the stock apps: move every app from `/private/var/staged_system_apps` into `/Applications` (iOS's own first-boot install of them fails on code signing here);
   - the stock wallpapers: `tools/wallpaper-heic2png.sh MOUNTED_ROOT`.

   Then detach it, convert it with `tools/img2simg.py` and flash it with `fastboot flash userdata`. The first boot takes a few minutes longer while iOS builds its caches.
3. **GPU firmware:** the Adreno 530 microcode (`a530_pm4.fw`, `a530_pfp.fw`, without their 4-byte headers) and the zap shader (`a530_zap.mdt` + `.b02`) come from the phone's own vendor firmware. They are not in this repository.
4. **Boot image:** `tools/mkadt.py ADT --pixel1` (needs the iPhone 8 device tree `d20ap` from the IPSW), then

   ```sh
   python3 tools/mkpayload.py KERNEL ADT payload --cmdline "$ARGS" \
       --blob GPU-PM4=pm4.bin --blob GPU-PFP=pfp.bin \
       --blob GPU-ZAP-MDT=a530_zap.mdt --blob GPU-ZAP-SEG=a530_zap.b02
   WDT_SECS=0 shim/build-pixel.sh STOCK_BOOT.img payload     # -> shim/out/pixel-boot.img
   fastboot boot shim/out/pixel-boot.img
   ```

   with the boot-args in use:

   ```
   debug=0xc serial=3 cpus=4 keepsyms=1 -noprogress sweetusb=0 rd=ufsroot0 unentitled_ios_sim_launch=1
   ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1
   sweetfb_noconsole=1 sweetsample=- sweetnull=backboardd cs_enforcement_disable=1 sweetprof=-
   sweetgpu=8 sweetgpuclk=600 sweetddr=1804 sweetenv=backboardd:DYLD_INSERT_LIBRARIES=/usr/lib/sweetgl.dylib
   ```

5. **Logs:** boot TWRP (`fastboot boot twrp.img`), then `tools/sweet-log-pull.sh OUT.txt`.

## Repository layout

| Path | Contents |
| --- | --- |
| `xnu/patches/` | Kernel patches, applied in order on top of xnu-8796 (`0079`+ are the Pixel port) |
| `shim/` | The boot shim (`build-pixel.sh`, `pixel.c`, console, pstore) |
| `tools/mkadt.py`, `tools/mkpayload.py` | Apple Device Tree generator and kernel payload packer |
| `tools/sweetgl/` | `sweetgl.dylib`: Mesa freedreno + libdrm/msm emulation over the kernel GPU driver + the EAGL shim for CoreAnimation (Mesa itself is fetched and built by `mesa-setup.sh`) |
| `tools/gpu-shaders/`, `tools/ir3asm/` | Hand-written Adreno compute shaders used by the kernel (frame scaling, self-tests) and their assembler |
| `tools/mkiosroot*.sh`, `tools/sweet-root-tweaks.py` | iOS root image builder and the userland changes it needs |

## Notable kernel pieces

- `bsd/dev/sweet_gpu.c`, `iokit/Kernel/arm/SweetGPU.cpp`: Adreno 530 bring-up and a msm-style buffer/submit interface for user space.
- `iokit/Kernel/arm/SweetSurface.cpp`, `SweetFramebuffer.cpp`: IOSurface and IOMobileFramebuffer (`AppleCLCD`) as backboardd expects them; GPU present into double-buffered panel memory.
- `bsd/dev/sweet_rpm.c`: talks to the RPM like Linux (GLINK, smd-rpm) for regulators, clocks, DDR and the memory rail.
- `bsd/dev/sweet_ufs.c`, `sweet_keys.c`, `sweet_touch.c`, `sweet_perf.c`: storage, buttons, touch, CPU clocks.

## Credits

- [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet): the original XNU-on-Qualcomm port this builds on.
- [Mesa](https://mesa3d.org/) / freedreno: the open-source Adreno driver that renders iOS here.
- [MakrSas/ios-topaz](https://github.com/MakrSas/ios-topaz): the IOAcceleratorES and IOKit-quiesce findings behind the faster boot.
- Apple's open-source XNU, and [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build) for building it.

iOS itself is Apple's and is not distributed here: you build the root from Apple's own IPSW.
