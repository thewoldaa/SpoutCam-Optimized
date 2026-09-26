# SpoutCam Optimized — release files

Two architectures. Take the one that matches the program you want the camera
in, not the one that matches Windows. A 64-bit Windows runs both.

## Which one

| You want the camera in | Download |
|---|---|
| OBS, TikTok Live Studio, most modern 64-bit programs | `SpoutCam-Optimized-x64.zip` |
| A 32-bit program | `SpoutCam-Optimized-x86.zip` |

If you are not sure, take x64. Nearly everything is 64-bit now.

You can install both. They are separate builds of the same camera and register
under the same name, so installing the second replaces the first rather than
adding a second camera.

## What is in each zip

```
SpoutCam.exe        the settings panel — run this
SpoutCam64.ax       the camera itself (x86 zip has SpoutCam32.ax)
```

Both files must stay in the same folder. The panel looks for the filter beside
itself, and the Install button will not work if they are separated.

## Installing

1. Unzip anywhere — Desktop is fine.
2. Run `SpoutCam.exe`.
3. Press **Install** under Camera. Windows asks for administrator rights once,
   because a DirectShow filter is registered machine wide. Every virtual camera
   on Windows works this way.
4. The program copies itself to `C:\Program Files\SpoutCam` and registers from
   there, so the folder you unzipped to can be deleted afterwards.

Programs that were already open need restarting before they will see the
camera.

## Using it

The camera only passes frames through while the settings panel is running —
the same way OBS owns its virtual camera. Minimise it and it drops to the
notification area and keeps working. Quit it and the camera goes idle and shows
its name plate instead of the picture.

Start a Spout sender (VRChat with Spout enabled, or any Spout output), then open
the panel and turn on **Live preview** to confirm frames are arriving.

## Antivirus

The binaries are unsigned, so SmartScreen warns on first run and some scanners
flag DirectShow filters on principle because they are DLLs that register
themselves into the system. No trick fixes this, only an Authenticode
certificate. Nothing here is packed or obfuscated.

## Verifying the download

```
sha256sum -c SHA256SUMS.txt
```

or on Windows:

```
certutil -hashfile SpoutCam.exe SHA256
```

## Source

Built from https://github.com/thewoldaa/SpoutCam-Optimized at the tag named in
the release. LGPL v3 — the source and the licence texts are in the repository.

This is a modified version of SpoutCam. The modifications are listed in
`CHANGES.md` there. Problems in it are not attributable to the authors of the
versions it came from.
