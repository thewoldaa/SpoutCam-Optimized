# SpoutCam (VRChat / streaming build)

A fork of [SpoutCam](https://github.com/leadedge/SpoutCam) aimed at getting a Spout
sender into streaming software that has no Spout support of its own, TikTok Live
Studio in particular.

SpoutCam is a DirectShow virtual webcam that receives a Spout texture. Because it is
a filter DLL, it loads inside the host application, so there is no second process and
no second compositor. Routing VRChat through OBS just to reach a virtual camera costs
a few hundred megabytes and an extra render pass. This costs neither.

## What changed

### Downscaling no longer aliases

The resampler used point sampling, so reducing 1920x1080 to a smaller camera size
dropped pixels rather than averaging them. Thin lines and text broke up in a way that
looked like heavy compression. It now box filters when shrinking, and keeps point
sampling when enlarging, where averaging would do the same work for the same answer.

### The sender size is picked up at connection time

`SetResolution` only ran in the filter constructor. If the Spout sender was not
already running when the host opened the camera, the size stayed at the default for
the whole session and every frame was resampled down to it. Media type negotiation
now refreshes from the active sender.

A sender that changes size mid-stream is still resampled. DirectShow fixes the
allocator when the pins connect, so the host has to reconnect to follow it.

### Resolution ceiling raised to 3840x2160

`GetStreamCaps` advertised a maximum of 1920x1080, which is below what plenty of
senders produce now.

### Transparency can be composited onto a key colour

A webcam feed has no alpha channel, so a sender that carries transparency would lose
it. SpoutCam can now blend the frame over a solid colour instead of discarding alpha,
which keeps the matte in a form a chroma key can remove downstream.

This is worth more than pointing a renderer at a green backdrop. Rendered green gets
baked into anti-aliased edges. A hair pixel comes out as a mix of hair and green, and
no keyer can separate those again. That is where green screen fringing comes from.
Compositing from real alpha keeps the edge colour intact. Hard edge mode goes further
and writes the source colour unblended above an alpha threshold, so the key colour
never touches an edge pixel at all.

### One less GPU stall per frame

The receiver already ping-pongs two staging textures, but `ReadPixelData` called
`FlushWait` before mapping. That waits on every outstanding GPU command, including
the copy just issued into the other staging texture. A blocking `Map` for read
already waits for writes pending on that one resource, so the flush only threw away
the second texture's benefit.

### Two sample buffers instead of one

`DecideBufferSize` asked for a single buffer, which made every frame wait for the
downstream filter to release the previous one.

### Orientation changes apply while running

Mirror, flip, swap and the key colour are re-read about once a second, so
changing them in the settings program takes effect without the host having to
disconnect and reconnect the camera. Frame rate and resolution change the media
type, and DirectShow fixes that when the pins connect, so those still need the
camera source removed and added again.

There is deliberately no rotation option. A quarter turn swaps the width and
height the camera advertises, so it could never apply without that same
disconnect, and every streaming program can already rotate a source in place.
Doing it there is one click and takes effect immediately.

### The delivered frame rate is measured, not assumed

The frame rate control states an intention. Three separate things can quietly
overrule it, and from the outside they look identical.

DirectShow fixes the rate when the pins connect, so changing the setting does
nothing until the camera source is removed and added again. A sender running
slower caps the rate no matter what the camera asks for. And a machine that
cannot convert a frame inside the frame time simply produces fewer of them.

The filter now counts what it actually delivers, reads the sender's own rate
alongside it, and publishes both once a second. The settings program shows them
under the frame rate control, and turns the line amber when the camera is more
than ten percent below what was asked for. Guessing which of the three is
happening was the hard part; reading two numbers is not.

The camera reports through shared memory rather than the registry. It runs
inside whichever program opened it, and streaming software tends to put its
capture in a low integrity process, where opening a named object succeeds but
writing to `HKEY_CURRENT_USER` is refused. Shared memory carrying a low
mandatory label gets through, which is the same reason the filter can receive
Spout textures from in there in the first place.

The sender rate needs Spout's frame counting turned on, which it is not by
default. With it off, `GetSenderFps` hands back the monitor refresh rate, which
it takes as a starting value and never replaces. That is a plausible number
with nothing behind it, so the line says frame counting is off instead of
repeating it. Turn it on in SpoutSettings, or set `Framecount` to 1 under
`HKCU\Software\Leading Edge\Spout`. The camera's own rate is measured here and
needs none of that.

### No more static

With no sender running the output was random noise. It now reads "SpoutCam", drawn
once and copied per frame.

### The camera follows the settings program

The settings program holds a named event while it runs, and the filter checks for it.
Quit the program and the camera goes idle, the same way quitting OBS stops its
virtual camera. Minimise instead and it drops to the notification area, out of the
way but still feeding the sender through.

The camera stays present in the host's device list either way. Idle means showing
the name plate rather than disappearing, since a device that vanishes mid-session
tends to upset the program using it.

## The settings program

The original settings program was never published in source form, so this is a new
one. It builds to a single executable with no installer and no runtime DLL to ship.

- Frame rate, resolution, sender lock, mirror / flip / swap, with the rate the
  camera is really delivering shown under the control that asks for it
- Key colour: on/off, preset or custom colour, hard edge, edge threshold
- Live preview over a checkerboard, so you can see at a glance whether a sender
  actually carries alpha. It follows the orientation and key colour controls as
  they are edited, so what is on screen is what the camera sends
- Registers the filter through the elevation prompt
- Settings are written as they are edited, so there is no Save button and nothing
  to lose by closing the window. Minimise puts it in the notification area

The interface is HTML in a WebView2 control. Video frames do not go through the
JavaScript bridge. The preview is a plain child window drawn directly over a slot in
the page, which is why it can run at full rate without costing anything.

## Building

Needs Visual Studio Build Tools with the C++ workload and the Windows SDK.

The filter:

```
msbuild SpoutCam\SpoutCamDX.sln /p:Configuration=Release /p:Platform=x64
```

Projects target toolset v145. Older Visual Studio installs will need that changed in
the three `.vcxproj` files, or a retarget from the IDE.

The settings program:

```
cd SpoutCamSettings
build.cmd x64
```

The finished program lands at the top of the repository as
`SpoutCam.exe`, next to the `SpoutCam` folder. Registration looks for the
filter relative to the program, so leaving it there means the Install button
works without being pointed at anything. `build.cmd x86` produces
`SpoutCam32.exe` beside it.

It needs the WebView2 SDK in `SpoutCamSettings\packages`:

```
nuget install Microsoft.Web.WebView2 -OutputDirectory packages
```

## Installing

Unzip anywhere, run `SpoutCam.exe`, press Install under Camera. Windows
asks for administrator rights once, because a DirectShow filter is registered
machine wide. Every virtual camera on Windows works this way.

The program installs itself rather than shipping a separate installer, so no
console window ever appears. It copies itself and the filter to
`C:\Program Files\SpoutCam`, registers the filter from there, adds a Start menu
entry, and writes an uninstall entry so it turns up in Settings, Apps like
anything else. Remove undoes all of it and leaves your settings alone, so
reinstalling keeps them.

Copying out of the download folder is the point rather than tidiness.
Registration records the path of the .ax file, not its contents, so a filter
registered from a folder that later gets cleaned up leaves a camera Windows
cannot load.

The Camera section shows what is actually registered. If a copy was ever
registered from somewhere else, that is the one Windows keeps loading, and the
panel says so and offers to replace it.

Working on the filter itself is different: register the build output in place
and rebuilding over the same path is picked up without registering again.

```
regsvr32 SpoutCam\binaries\SPOUTCAM\SpoutCam64\SpoutCam64.ax
```

A rebuild does need any program holding the camera open to be closed first,
because a loaded DLL cannot be overwritten, and reopened afterwards, because a
loaded DLL is not reloaded either. 32-bit hosts need `SpoutCam32.ax`.

## A note on antivirus warnings

The binaries are unsigned, so SmartScreen will warn on first run, and some scanners
flag DirectShow filters on principle because they are DLLs that register themselves
into the system. No trick fixes this, only an Authenticode certificate. If you
distribute a build, sign it. Nothing here is packed or obfuscated, which is the usual
cause of false positives.

## What the key colour option can and cannot do

Alpha cannot reach a live stream. It dies twice over: webcam sources are treated as
opaque everywhere, and H.264 has no alpha channel, so the frame is flattened before
it leaves the encoder regardless. The key colour option exists because compositing
has to happen somewhere upstream of that, and a chroma key in the streaming software
is the only place left. If you can composite the background inside VRChat instead, do
that. It is free and lossless.

## Credit

The original DirectShow capture source filter was written by Vivek and published
through [The March Hare](http://tmhare.mvps.org/downloads/vcam.zip).

Skype compatibility work by [John MacCormick](https://github.com/johnmaccormick/MultiCam).

The Visual Studio project and the 64-bit build, including the bundled DirectShow base
classes, are the work of [Valentin Schmidt](https://github.com/59de44955ebd/SpoutCam).

Spout, the Spout SDK and SpoutCam itself are by Lynn Jarvis:
[leadedge/Spout2](https://github.com/leadedge/Spout2) and
[leadedge/SpoutCam](https://github.com/leadedge/SpoutCam). Everything here is a
modification of that work and would not exist without it.

DirectShow base classes are Microsoft sample code.
