# webview_cef

Platform side of the [`webview_cef`](https://pub.dev/packages/webview_cef) pub
package, so a flutter app can embed web pages on flutter-pi with the same dart
code it uses on desktop.

Off by default; enable with `-DBUILD_WEBVIEW_CEF_PLUGIN=ON`.

## How it works

flutter-pi has no platform view support and owns the DRM master, so a browser
cannot be given a window of its own. Instead:

1. CEF renders the page **off-screen** (`windowless_rendering_enabled`) and hands
   flutter-pi a BGRA pixel buffer through `CefRenderHandler::OnPaint`.
2. The plugin uploads that buffer into a GL texture on a context that shares
   flutter's root context, and publishes it through flutter-pi's texture
   registry.
3. The package's dart side shows it with a plain `Texture` widget and forwards
   pointer input over the platform channel, because an off-screen browser gets no
   input of its own.

```
 dart                  flutter-pi (platform thread)          CEF (UI thread)
 ─────                 ─────────────────────────────         ───────────────
 Texture(id) ◄─── texture_push_frame ◄── on_paint ◄───────── OnPaint (BGRA)
 Listener    ───► cursorClickDown ─────► SendMouseClickEvent ────►
                  webview list, wv->browser ◄── post ◄─────── OnBeforeClose
```

The frame arrives on CEF's thread and is uploaded there; only the teardown has to
be handed back, because the webview list belongs to the platform thread.

### Threading

CEF runs the browser process message loop on a thread of its own
(`multi_threaded_message_loop`), so CEF's *UI thread* is not flutter-pi's platform
thread. Two threads therefore meet in the plugin:

- platform channel handlers run on the platform thread,
- every `cef_bridge` callback -- `OnPaint` included -- runs on CEF's UI thread.

Three rules keep that honest. The webview list belongs to the platform thread, so
`OnBeforeClose` posts its teardown there rather than unlinking anything itself.
Everything a frame touches on its way to a texture is behind one mutex, because
the platform thread may be releasing the same webview's textures and the plugin's
EGL context can only be current on one thread at a time. And sending a platform
message needs nothing at all: `flutterpi_send_platform_message` posts to the
platform thread when it isn't already on it.

Only one CEF call has to be marshalled. `CefBrowser` and `CefBrowserHost` are
documented as callable from any browser process thread, which covers input,
resize, navigation and close; `CreateBrowserSync` is the exception, so the bridge
posts it to the UI thread and waits for the browser id the channel reply needs.

The `wvcef_browser` handle is owned by the plugin, not the bridge, and that is
deliberate. CEF can close a browser on its own -- a renderer crash is the usual
reason -- and it reports that on its UI thread while the platform thread may be
part-way through a channel call holding the same handle. A bridge that freed the
handle from `OnBeforeClose` would pull it out from under that call, so instead the
handle survives its browser: every call on it becomes a no-op, and the plugin
releases it with `wvcef_browser_destroy` from the platform thread, where it is the
only one looking. The webview itself is kept in that case, still showing its last
frame, because the dart side holds a controller for it and would otherwise get a
black rectangle with no explanation.

### Why not the external message pump

`external_message_pump` is the other way to do this, and it looks strictly nicer:
the host drives `CefDoMessageLoopWork()` from its own event loop, the platform
thread *becomes* CEF's UI thread, and every callback can touch flutter-pi state
with no locks and no marshalling at all.

It also deadlocks. A Chromium task on the UI thread will sometimes block waiting
for more UI thread work -- something in the GPU and compositor paths does it
regularly. A real message loop nests and delivers that work; an external pump
cannot, because the host is stuck inside its one `CefDoMessageLoopWork()` call
and has no way to re-enter it. The process then stops for good.

It is worth knowing what that looks like, because none of it points at the pump.
Everything off the UI thread keeps running -- the network stack, the render
process -- so the page stays loaded, its JavaScript still runs and its WebSocket
stays up; it just never paints again. How long it survives depends only on how
often the blocking path is hit: with software WebGL it lasted about 130 frames,
with hardware GL about 1600, and with a page that painted but did nothing else it
looked fine indefinitely. CEF's own documentation recommends against the option.

### Processes

Chromium is multi-process, and on Linux it starts subprocesses by re-executing a
binary. If that binary were flutter-pi, every renderer would try to boot a
flutter engine. So the plugin builds a separate `flutter-pi-cef-helper`
executable and points `CefSettings::browser_subprocess_path` at it.

The sandbox is off (`no_sandbox`), which avoids having to ship the setuid
`chrome-sandbox` helper.

## Files

| file | what it is |
| --- | --- |
| `plugin.c` | the flutter-pi side: platform channel, textures, frame upload |
| `cef_bridge.h` | plain-C ABI between the two halves |
| `cef_bridge.cpp` | the CEF side: `CefApp`, `CefClient`, off-screen rendering |
| `helper_main.cpp` | `main()` of the subprocess helper |

The split exists because flutter-pi's headers cannot be included from C++ --
`platformchannel.h` and friends use compound literals, which C++ does not have.

## Building

```
cmake -B build -DBUILD_WEBVIEW_CEF_PLUGIN=ON -DCEF_ROOT=/path/to/cef_binary_..._linux64_minimal
```

`CEF_ROOT` must contain `include/cef_version.h`, `libcef.so` and
`libcef_dll_wrapper.a`. Leave it unset to search the (cross) sysroot instead.
`libcef_dll_wrapper` is the static library that translates CEF's C++ API to its
stable C ABI; CEF expects you to build it yourself with your own compiler, which
is why gcc/libstdc++ against a clang/libc++ `libcef.so` is fine.

CEF 126 or newer is required -- that's when `OnBeforePopup` got its `popup_id`
parameter. There is a `#error` guarding it at the top of `cef_bridge.cpp`.
Developed against 132.3.2.

Two more cache variables describe the **target** layout:

| variable | default | meaning |
| --- | --- | --- |
| `WEBVIEW_CEF_RUNTIME_DIR` | `/usr/lib/cef` | where `libcef.so`, `icudtl.dat`, `*.pak`, `*.bin` and `locales/` live |
| `WEBVIEW_CEF_HELPER_PATH` | `/usr/bin/flutter-pi-cef-helper` | the subprocess helper |

## Runtime configuration

The `webview_cef` channel only carries a user agent, so everything else is
configured through the environment. This is also handy while bringing the thing
up, since none of it needs a recompile.

| environment variable | meaning |
| --- | --- |
| `FLUTTERPI_CEF_RUNTIME_DIR` | overrides `WEBVIEW_CEF_RUNTIME_DIR` |
| `FLUTTERPI_CEF_HELPER` | overrides `WEBVIEW_CEF_HELPER_PATH` |
| `FLUTTERPI_CEF_CACHE_PATH` | where Chromium may persist its cache. Unset = in-memory profile |
| `FLUTTERPI_CEF_SWITCHES` | extra Chromium switches, comma separated, without `--` |
| `FLUTTERPI_CEF_NO_DEFAULT_SWITCHES` | if set, don't pass the default switches below |
| `FLUTTERPI_CEF_LOG_FILE` | Chromium's log file (default: stderr) |
| `FLUTTERPI_CEF_LOG_SEVERITY` | 1 verbose, 2 info, 3 warning, 4 error, 5 fatal, 99 off |
| `FLUTTERPI_CEF_FRAME_RATE` | `windowless_frame_rate`, 1..60 (default 30) |
| `FLUTTERPI_CEF_EAGER_INIT` | if set, start CEF during plugin init instead of on the first `init`/`create` |
| `FLUTTERPI_CEF_TRACE` | `1` logs browser creation, `setSize` and the frames arriving from CEF; `2` adds every frame step by step |
| `FLUTTERPI_CEF_FORCE_RGBA` | if set, convert frames to RGBA on the CPU instead of uploading BGRA directly |

`FLUTTERPI_CEF_TRACE=1` answers the first question a blank webview raises: is CEF
painting at all, and at what size? It prints the first three frames per browser
and then every hundredth, so it is usable on a running release build -- unlike
`LOG_DEBUG`, which is compiled out of exactly the builds that need explaining.

Level `2` prints each frame's steps, flushed as it goes, for the case where a
frame stops halfway: taking the texture mutex, making the EGL context current and
`glFinish` can all block, and the last line printed says which call didn't return.
Useful on a target with no debugger on it.

`FLUTTERPI_CEF_EAGER_INIT` exists because plugins are initialized *before* the
flutter engine is created. Starting CEF there means Chromium forks its helper
processes out of a process that is not heavily threaded yet, and installs its
signal handlers first. It is the safer ordering, and it moves `CefInitialize`'s
half second off the first `init` call -- at the cost of paying Chromium's memory
even if no webview is ever opened.

The switches passed by default:

```
--ozone-platform=headless           there is no X server and no compositor
--use-gl=angle                      ANGLE on native EGL/GLES, i.e. Mesa on a
--use-angle=gl-egl                    render node -- see "GPU" below
--ignore-gpu-blocklist              an embedded GPU won't be on the known-good list
--enable-unsafe-swiftshader         software WebGL, only if the above fails
--disable-dev-shm-usage
--autoplay-policy=no-user-gesture-required
```

The plugin also sets `EGL_PLATFORM=surfaceless` (without overwriting an existing
value) before `CefInitialize`, because the GL switches above do nothing without
it -- see "GPU". If the page stays blank, or Chromium dies during startup, this is
the list to experiment with:

```bash
FLUTTERPI_CEF_TRACE=1 FLUTTERPI_CEF_LOG_SEVERITY=1 FLUTTERPI_CEF_SWITCHES=enable-logging=stderr,v=1 flutter-pi --release /path/to/bundle
```

### GPU

Chromium and flutter-pi can share the GPU, and it is worth making sure they do.
flutter-pi holds DRM master on the card node, but Chromium doesn't need master --
it only needs a *render* node, `/dev/dri/renderD128`, which is exactly what render
nodes are for. So the two coexist, and WebGL and canvas run on the real GPU.

Getting there needs two things that are easy to miss, because missing them costs
performance rather than correctness:

1. `--use-gl=angle --use-angle=gl-egl`, so Chromium uses ANGLE over native
   EGL/GLES instead of its bundled SwiftShader.
2. `EGL_PLATFORM=surfaceless`, because Mesa's EGL otherwise defaults to the X11
   platform. There is no X server, `eglInitialize` fails with *"Could not open the
   default X display"*, and Chromium falls back to SwiftShader.

That fallback is the thing to watch for. It is silent: the page loads, WebGL
reports a working context, everything renders correctly -- just slowly, with the
CPU doing the rasterisation. On a Celeron J6412 a WebGL slot game ran at about
**3fps**, with SwiftShader's four worker threads saturating all four cores, and at
full speed with **~3%** CPU in the GPU process once Mesa was actually in use.

To check which one you got, on the target:

```bash
# find the GPU process, then look at what it loaded
for p in $(pidof flutter-pi-cef-helper); do grep -aq type=gpu-process /proc/$p/cmdline && echo $p; done
```

With hardware GL that process has `/dev/dri/renderD128` among its open `fd`s and
the Mesa driver (`iris_dri.so`, `v3d_dri.so`, ...) in its `maps`. On SwiftShader it
has neither, its command line says `--use-angle=swiftshader-webgl`, and its hottest
threads are named `Thread<00>`, `Thread<01>`, ... -- SwiftShader's worker pool, and
an unmistakable signature once you have seen it.

## Implemented channel methods

Channel `webview_cef`, standard method codec. Arguments are positional lists, or
a bare value for the single-argument methods -- that is what the package's dart
side sends.

| method | arguments | returns |
| --- | --- | --- |
| `init` | userAgent (String), or nothing | null |
| `create` | url (String) | `[browserId, textureId]` |
| `close` | browserId | null |
| `loadUrl` | `[browserId, url]` | null |
| `reload` / `goBack` / `goForward` | browserId | null |
| `setSize` | `[browserId, dpi, width, height]` | null |
| `cursorMove` / `cursorDragging` | `[browserId, x, y]` | null |
| `cursorClickDown` / `cursorClickUp` | `[browserId, x, y]` | null |
| `setScrollDelta` | `[browserId, x, y, deltaX, deltaY]` | null |
| `setClientFocus` | `[browserId, focus]` | null |
| `executeJavaScript` | `[browserId, code]` | null |
| `imeCommitText` / `imeSetComposition` | `[browserId, text]` | null |
| `quit` | none | null |

Sizes and coordinates are logical pixels; `dpi` is the device pixel ratio.

Events back to dart, as method calls on the same channel, all carrying
`browserId` in a map: `urlChanged`, `titleChanged`, `onLoadStart`, `onLoadEnd`,
`onTooltip`, `onCursorChanged`, `onConsoleMessage`.

`quit` only closes the browsers, it does not call `CefShutdown` -- CEF cannot be
initialized twice in one process, so shutting down on `quit` would break every
later webview. CEF is torn down in the plugin's deinit instead.

## Not implemented

These respond with "not implemented", so the dart side gets a clear
`MissingPluginException` rather than silence:

- `evaluateJavascript`, `setJavaScriptChannels`, `sendJavaScriptChannelCallBack`
  -- need a `CefRenderProcessHandler` in the helper plus IPC to get values back
  out of V8. `executeJavaScript` (fire and forget) does work.
- `openDevTools` -- needs a real window to put the inspector in.
- `setCookie`, `deleteCookie`, `visitAllCookies`, `visitUrlCookies` --
  straightforward on top of `CefCookieManager`, just not done yet.
- The `onFocusedNodeChangeMessage` and `onImeCompositionRangeChangedMessage`
  events -- `CefRenderProcessHandler::OnFocusedNodeChanged` fires in the render
  process, so reporting it needs process messages between the helper and here.

### Typing into a page

`imeCommitText` and `imeSetComposition` are implemented, so text input works --
but the package's dart side only attaches flutter's text input client after it
receives `onFocusedNodeChangeMessage`, which is in the list above. So a page that
focuses an `<input>` on its own does **not** get a keyboard yet.

An app with its own on-screen keyboard can drive it directly in the meantime:

```dart
controller.imeCommitText('5');
```

Wiring up the focus notification is the missing piece for automatic keyboard
handling, and it's the main thing left to do here.

## Other limitations

- **One GL texture per webview, reused every frame.** If flutter's rasterizer is
  sampling frame N while frame N+1 is uploaded, that frame can tear. Double
  buffering would fix it at the cost of memory.
- **Dirty rects are ignored**; every paint uploads the whole surface. GLES2 has
  no `GL_UNPACK_ROW_LENGTH`, so partial uploads would need a per-row loop.
- **Page compositing happens on the CPU**, whatever the GPU does. CEF forces
  `--disable-gpu-compositing` for windowless rendering unless shared textures are
  enabled, and it does so on its own -- it shows up in the renderer command lines
  without being passed. WebGL and canvas still get the GPU (see below); it is the
  final compositing step that is software, plus a `ReadPixels` per frame to get
  the result into the buffer `OnPaint` wants.
- **Popups** (`<select>` dropdowns) are composited into the view buffer while
  they are open, which costs one extra full-frame copy per paint. Nothing is
  copied when no popup is open.
- **`target=_blank` and `window.open()`** load into the same view instead of
  opening a second window.
- **Touch input arrives as mouse input**, because that is what the package's dart
  side sends. Multi-touch gestures inside the page are not available.
