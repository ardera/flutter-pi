# GStreamer video frame and subscription lifetime

The OpenGL video player supports overlapping players, for example while crossfading
between two videos. Each player owns an EGL upload context, but the players use
the renderer's GBM device. Software-decoded frames need to be copied into DMA-BUFs
before EGL can import them. Decoder-provided DMA-BUFs continue to use the existing
import path.

## Corrected ownership and ordering

* A texture frame retains its `GstBuffer`, rather than the complete `GstSample`.
  This preserves the decoded pixels until Flutter releases the texture without
  preventing appsink from updating its reusable sample during flushing seeks.
  In GStreamer 1.22, the post-flush caps restoration path calls
  `gst_sample_set_caps` without first making the sample writable. Holding an
  extra sample reference can therefore trigger `gst_sample_is_writable` errors.
  See the [buffer ownership contract](https://gstreamer.freedesktop.org/documentation/gstreamer/gstsample.html#gst_sample_get_buffer)
  and [GStreamer 1.22 appsink implementation](https://gitlab.freedesktop.org/gstreamer/gstreamer/-/blob/1.22.0/subprojects/gst-plugins-base/gst-libs/gst/app/gstappsink.c).
* A shared lock protects software GBM uploads, from BO creation through
  map/copy/unmap, fd export and BO destruction. Per-player EGL locks cannot
  protect a mapping context shared by a GBM backend. The lock does not cover
  decoding, DMA-BUF imports or texture rendering.
* Partial plane-import failure closes all fds already exported for that frame.
* Listener handles start at `NULL`. A replacement `listen` removes the old
  subscription. Video-info notification no longer frees a listener handle from
  an internal streaming thread while the platform thread may cancel it.
* The notifier's initial-value callback and registration use the same mutex as
  later updates, so initial delivery cannot race replacement of that value.
* Disposal removes listeners and stops the pipeline before releasing the sink,
  bus, notifiers, upload interface and texture. Streaming callbacks can still
  access their dependencies during the downward state transition.
* Looping restarts from the pipeline's aggregated `GST_MESSAGE_EOS`, through the
  normal seek path, with failed seeks reported. The earlier appsink callback can
  run before that sink finishes processing the EOS event. Waiting for the
  pipeline avoids racing its pending EOS with a flushing seek; see
  [GStreamer EOS message semantics](https://gstreamer.freedesktop.org/documentation/additional/design/messages.html).
* Empty HTTP headers are not applied to file or custom-pipeline sources.
  Configured header values use initialized `GValue` strings; unsupported sources
  still report an error when nonempty headers were explicitly requested.
* Native disposal and EventChannel cancellation can arrive in either order.
  If disposal arrives first, the decoder and texture are released immediately;
  only channel metadata and a terminal receiver remain until the pending
  `cancel`. That receiver rejects `listen` and unknown methods. Channels for
  which cancellation never arrives are removed at plugin shutdown. Unknown
  channels are still reported as unimplemented.

The shared upload lock is a conservative workaround for a crash observed inside
`gbm_bo_unmap` with two software-decoding players on Raspberry Pi 5 / Mesa 24.2.8.
It is not a general claim that every GBM backend has the same implementation,
or that a short successful test proves the absence of all driver or lifecycle
races.

## Native regression tests

With the development dependencies from the [build instructions](../README.md#dependencies)
and the Unity submodule checked out:

```sh
cmake -S . -B build/video-tests -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTS=ON \
  -DENABLE_VULKAN=OFF -DLTO=OFF \
  -DTRY_BUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN=OFF
cmake --build build/video-tests -j3
ctest --test-dir build/video-tests --output-on-failure
```

The video tests mock only the graphics and platform-channel boundaries; they do
not open DRM or an EGL display. They check retained pixel lifetime, sample
writability, fd cleanup, shared upload serialization, allocation/export failure,
both cancel/dispose orderings, replacement subscriptions, rapid disposal,
pending-channel shutdown and pipeline teardown ordering. The notifier tests
check initial-value synchronization and immediate self-unsubscription.

For runnable hardware commands, see the
[OpenGL video benchmark guide](video-benchmarks.md).

Mocks cannot reproduce a Mesa crash or validate display scanout. For hardware
acceptance, preserve the installed runtime and compare separate baseline and
candidate release binaries with the same engine, application bundle, original
media, display mode and renderer. Stop the normal application before acquiring
DRM, use a supervisor independent of SSH, and restore it on success or failure.
Do not run two embedders against the same display.

Use short, explicit OpenGL scenarios covering photo to video, video to photo,
video to video, two simultaneous decoders, repeated flushing seeks, playback
past the fixture duration to exercise looping, and repeated create/dispose
cycles. Preserve native stderr, the active operation, build/source hashes,
backtraces for crashes, FrameTiming distributions, RSS, fd and thread samples,
and texture/visual evidence. Keep crashes and missing measurements as failures;
do not replace missing data with zero or suppress cancellation errors.
