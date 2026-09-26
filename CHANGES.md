# Changes in this fork

This is a modified version of SpoutCam. It is not the original work, and any
problem in it is not attributable to the authors of the versions it came from.

The licence requires that modifications be stated. This file is that statement,
and it is kept current with each release.

## Lineage

| Version | Source |
|---|---|
| Original capture source filter | Vivek, via [The March Hare](http://tmhare.mvps.org/downloads/vcam.zip) |
| Skype compatibility | [John MacCormick](https://github.com/johnmaccormick/MultiCam) |
| Visual Studio project, 64-bit build | [Valentin Schmidt](https://github.com/59de44955ebd/SpoutCam) |
| SpoutCam, Spout SDK | [Lynn Jarvis](https://github.com/leadedge/SpoutCam) |
| Streaming build, settings program, key colour, resampling | [ardha27](https://github.com/ardha27/SpoutCam) |
| This fork | [thewoldaa](https://github.com/thewoldaa/SpoutCam-Optimized) |

## This fork

Starting from ardha27/SpoutCam at commit `25247db`. The parent's own changes are
described in its README and are not repeated here; what follows is what this
fork adds on top.

### Fixed

**Race on the media type during a settings change.** `m_cSharedState` was
declared on the pin and never used. `put_Settings` runs on the property page
thread and its final `GetMediaType` call can reallocate the format buffer that
`m_mt` owns, while `FillBuffer` on the streaming thread held a pointer into that
same buffer for the length of the function. Reading through it after a settings
change was reading freed memory. The frame dimensions are now copied out under
that lock, and `put_Settings` takes it.

**Uninitialised variable in `SetResolution`.** `unsigned int width, height = 0;`
left `width` uninitialised before being passed to `GetSenderInfo` as an output
parameter.

**Capability count did not match what the pin offered.** `GetNumberOfCapabilities`
reported one format while `GetMediaType` served two, and both positions built
the same media type from the same globals. Reduced to the single position the
revision history already intended.

**Live settings were re-read on a frame count.** `NumFrames % 60` meant the
interval was whatever sixty frames happened to be, so a checkbox took six
seconds to register at 10 fps and one second at 60 fps. Now timed.

### Performance

**Registry reads in the settings panel.** The rate line re-read the frame rate
selection from `HKEY_CURRENT_USER` on every repaint, thirty times a second, for
a value only the panel itself writes. Cached, refreshed on load and on save.

**Sender probe on every repaint.** The header lamp opened, mapped and closed the
sender's shared memory thirty times a second. Reduced to once a second, which is
far finer than a lamp needs.

**GDI objects rebuilt on every repaint.** The preview created and destroyed two
device contexts, a compatible bitmap, a DIB section, a region, three brushes and
a font on each of thirty repaints a second, around four hundred and eighty
object operations per second for a picture that changes size only when the
window does. All of it is now held between repaints and rebuilt only on a size
change. The clip region is gone entirely, the checkerboard is drawn at its
destination instead.

### Security

**Shared memory access narrowed.** The frame rate mapping granted generic all to
Everyone, so any process on the machine could rewrite or clear a value the panel
displays. The logged on user now has the write the filter needs and Everyone has
read only. The low mandatory label and the app package grant are unchanged,
since those are what let a low integrity host report through it. Falls back to
the previous descriptor if the user SID cannot be converted.

### Build

**Toolset v143.** The projects targeted v145, which is a Visual Studio 2026
toolset and not generally available. Changed in the three `.vcxproj` files so
the solution builds with a current Visual Studio 2022 install without a
`/p:PlatformToolset` override.

**x86 builds produced.** The parent's build instructions describe `build.cmd x86`
but no 32-bit binaries were published. Both architectures are built and attached
to releases here.

### Not changed, and why

**The resampler.** Two rewrites of `rgba2rgbResample` were written, measured and
discarded. Replacing the per pixel division with a 32.32 reciprocal multiply
measured 0.89x to 1.02x; accumulating a destination row at a time measured 0.53x
to 0.70x. The loop is bound by reading the source rather than by the arithmetic
at the end of it. Both are recorded in the source so the attempt is not repeated.

**Rotation.** The parent deliberately does not offer it, because a quarter turn
swaps the width and height the camera advertises and could never apply without
the host reconnecting. That reasoning still holds.
