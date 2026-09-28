# SpoutCam 1.1.1

A DirectShow virtual webcam that receives a Spout sender, built for getting
VRChat into streaming software with no Spout support of its own.

This is a fork of [ardha27/SpoutCam](https://github.com/ardha27/SpoutCam). What
changed is in `CHANGES.md` in the repository and in the release notes.

## Install

1. Unzip anywhere.
2. Run `SpoutCam.exe`.
3. Press **Install** under Camera.

Windows asks for administrator rights once, because a DirectShow filter is
registered machine wide. Every virtual camera on Windows works this way. The
program copies itself to `C:\Program Files\SpoutCam`, adds a Start menu entry,
and turns up in Settings, Apps with a working uninstall, so the folder you
unzipped to can be deleted afterwards.

**Then close and reopen any program that was already running.** A loaded filter
is never reloaded, so removing and re-adding the camera source is not enough on
its own.

## What is in the download

```
SpoutCam\README.md              this file
SpoutCam\SpoutCam.exe           64-bit settings panel — run this
SpoutCam\SpoutCam32.exe         32-bit settings panel
SpoutCam\filter\SpoutCam64.ax   64-bit camera
SpoutCam\filter\SpoutCam32.ax   32-bit camera
SpoutCam\LICENSE                LGPL v3
SpoutCam\COPYING                GPL v3
```

Keep the whole folder together. The panel looks for the filter in `filter\`
beside itself, and the Install button will not find it if they are separated.

Take `SpoutCam.exe` unless you are feeding a 32-bit program. Both are the same
camera and register under the same name, so installing one over the other
replaces it rather than adding a second camera.

## Using it

The camera only passes frames while the settings panel is running, the same way
OBS owns its virtual camera. Minimise it and it drops to the notification area
and keeps working. Quit it and the camera goes idle and shows its name plate
instead of the picture.

Start a Spout sender — VRChat with Spout enabled, or any Spout output — then open
the panel and turn on **Live preview** to confirm frames are arriving.

### Resolution

The camera advertises a list of sizes from 320x240 up to 3840x2160, plus the
sender's own size first. Pick the one your streaming program wants, or leave it
on **Active sender** to follow whatever the sender is producing.

Whatever you pick, the filter resamples the sender into it, so the camera and
the sender do not have to agree. A mismatch does cost a resample per frame, so
if your stream is 1080p, setting the sender to 1080p as well is the cheaper
arrangement.

Frame rate and resolution are fixed by DirectShow when the pins connect, so
changing either needs the camera source **removed and added again** in your
streaming program. Mirror, flip, swap and the key colour apply while running.

## Antivirus

The binaries are unsigned, so SmartScreen warns on first run and some scanners
flag DirectShow filters on principle, because they are DLLs that register
themselves into the system. Only an Authenticode certificate fixes that.
Nothing here is packed or obfuscated, and the source is all in the repository.

## Verifying the download

```
sha256sum -c SHA256SUMS.txt
```

or on Windows:

```
certutil -hashfile SpoutCam-1.1.1-win64.zip SHA256
```

## Licence

LGPL v3. The source is at
https://github.com/thewoldaa/SpoutCam-Optimized, and the licence texts are in
this folder as well as in the repository.

This is a modified version of SpoutCam. The modifications are stated in
`CHANGES.md` as the licence requires, and problems in it are not attributable to
the authors of the versions it came from.
