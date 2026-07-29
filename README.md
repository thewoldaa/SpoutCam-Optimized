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

### Rotation

The output can be turned by a quarter, a half or three quarters. A quarter turn
swaps the advertised width and height, which is how a landscape sender becomes a
portrait camera for a vertical stream.

### Orientation changes apply while running

Mirror, flip, swap and the key colour are re-read about once a second, so
changing them in the settings program takes effect without the host having to
disconnect and reconnect the camera. Frame rate, resolution and rotation all
change the media type, and DirectShow fixes that when the pins connect, so those
still need the camera source removed and added again.

### No more static

With no sender running the output was random noise. It now reads "SpoutCam", drawn
once and copied per frame.

## SpoutCamSettings

The original settings program was never published in source form, so this is a new
one. It builds to a single executable with no installer and no runtime DLL to ship.

- Frame rate, resolution, sender lock, mirror / flip / swap, rotation
- Key colour: on/off, preset or custom colour, hard edge, edge threshold
- Live preview over a checkerboard, so you can see at a glance whether a sender
  actually carries alpha. It follows the orientation controls as they are
  edited, so you can see a rotation before committing to it
- Registers the filter through the elevation prompt

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
`SpoutCamSettings.exe`, next to the `SpoutCam` folder. Registration looks for the
filter relative to the program, so leaving it there means the Register button
works without being pointed at anything. `build.cmd x86` produces
`SpoutCamSettings32.exe` beside it.

It needs the WebView2 SDK in `SpoutCamSettings\packages`:

```
nuget install Microsoft.Web.WebView2 -OutputDirectory packages
```

## Installing the camera

Run `SpoutCamSettings.exe` and press Install under Camera. That is the whole
process. Windows will ask for administrator rights once, because a DirectShow
filter is registered system wide. Every virtual camera on Windows works this way.

The Camera section shows what is actually registered, which matters more than it
sounds. Registration records the path of the filter file, so if a copy was ever
registered from somewhere else, that is the one Windows keeps loading. The panel
says so and offers to replace it.

Installing once is enough. Rebuilding over the same path is picked up without
registering again, since only the path is recorded. What a rebuild does need is
for any program holding the camera open to be closed first, because a loaded DLL
cannot be overwritten.

`install.cmd` in the repository root does the same thing from a command line, and
takes `/u` to remove. Or by hand:

```
regsvr32 SpoutCam64.ax
```

32-bit hosts need `SpoutCam32.ax` registered as well.

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
