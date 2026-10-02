# WPE WebKit 2.52.6 integration investigation

Investigation performed against the installed headers and pkg-config metadata
on this Arch Linux development machine. `wpe-webkit-2.0` and `wpe-platform-2.0`
both report 2.52.6. The pkg-config imported target also brings in GLib/GObject,
libsoup 3 and libwpe as transitive dependencies; no include or library paths
need to be hardcoded.

The `wpe-platform-probe` CTest creates the default platform display and a
`WPEView`. On the current Wayland session it reports `WPEViewWayland` at
1024×768. This verifies platform plugin loading and view creation at runtime;
it does not demonstrate that WPE WebKit can use that view as its rendering
backend.

## APIs present

- `<wpe/wpe-platform.h>` is the umbrella for WPE Platform 2. `wpe_display_get_default()`
  obtains the selected platform display, `wpe_display_connect()` connects it,
  and `wpe_display_create_toplevel()` / `wpe_view_new()` are the platform display
  and view mechanisms. Platform backends are selected/loaded by the installed
  WPE Platform implementation; the Wayland and headless platform modules are
  installed here.
- `WPEView` exposes `wpe_view_resized()`, mapping/visibility/focus operations,
  `wpe_view_event()` for input and `wpe_view_render_buffer()` /
  `wpe_view_buffer_rendered()` / `wpe_view_buffer_released()` for buffer flow.
  Its class has `buffers_changed` and `render_buffer` virtual functions. A host
  implementation receives buffers in `render_buffer`; it must report a buffer
  rendered and later released using the corresponding WPE calls. The exact
  asynchronous scheduling and buffer lifetime must be respected by a concrete
  presenter.
- `WPEBufferSHM` exposes ARGB8888 pixel data (`GBytes`) and stride. `WPEBufferDMABuf`
  exposes plane fds, offsets, strides, DRM format and modifier. Base `WPEBuffer`
  offers EGL image/pixel imports and rendering/release fences.
- `WPEEvent` constructors cover pointer button/move, scroll, keyboard and touch;
  `wpe_view_event()` dispatches those to the view. Focus is reported through
  `wpe_view_focus_in/out()`.
- WebKit's `<wpe/WebKitWebView.h>` provides `webkit_web_view_new(backend)`,
  `webkit_web_view_load_uri()`, `webkit_web_view_get_wpe_view()` and
  `webkit_web_view_get_display()`. However its `WebKitWebViewBackend` constructor
  is `webkit_web_view_backend_new(struct wpe_view_backend*, ...)`.

## Integration constraint / recommendation

There is a significant API seam in these installed headers: WPE Platform 2
creates GObject `WPEView` objects, while the installed WebKitWebView constructor
still requires a legacy `struct wpe_view_backend` from `<wpe/wpe.h>` (libwpe).
The headers do not expose a direct `WPEView` → `WebKitWebViewBackend` adapter,
nor a documented constructor accepting the platform `WPEView`. Therefore it is
not safe to assume that creating a WPE Platform display/view is sufficient to
create this WebKit view, and a working Qt/WebKit embedding cannot be claimed
from headers alone.

## Current bring-up implementation

The prototype now bridges through `wpebackend-fdo` only at the engine adapter:
`wpe_loader_init("libWPEBackend-fdo-1.0.so")` loads the backend, the exportable
SHM callback copies each `wl_shm_buffer` into a `QImage`, releases the backend
buffer, then queues the owned image into a Qt `QWidget` for painting. Resize is
sent to the backend, and a short Qt timer iterates the default GLib main context
so WebKit's asynchronous work progresses alongside Qt's event loop. Navigation
is issued inside `WebKitView`; the browser window remains Qt-owned.

Initial navigation is deferred one Qt event-loop turn after tab creation. This
allows the window to map and the content widget to receive its final viewport
size before WebKit starts layout; loading at the widget's temporary default
640×480 size caused the initial SHM frame to contain a transparent upper region
until a later page repaint. WebKit/FDO frame completion is also acknowledged
after Qt paints the copied frame, rather than when the frame is merely queued.

This implementation compiled and ran on the current Wayland session. Runtime
logs showed WebKit load events through `WEBKIT_LOAD_FINISHED` and repeated SHM
frame callbacks at the resized 1100×760 content dimensions. The presenter is a
CPU-copy bring-up path, not the intended GPU/DMA-BUF production route.

## Input forwarding

Qt input is translated in `WebKitView` and dispatched on the legacy view
backend with `wpe_view_backend_dispatch_keyboard_event()`,
`wpe_view_backend_dispatch_pointer_event()` and
`wpe_view_backend_dispatch_axis_event()`. WebKit installed itself as the
backend's input client when the view was created, so these calls reach WebKit
directly. Visible/in-window/focused activity states are reported alongside so
the page is not throttled. Text input/IME forwarding is still missing.

One encoding seam is easy to get wrong: `wpe_input_pointer_event.button` uses
WPE's 1-based button index (left = 1, right = 2, middle = 3, back = 4,
forward = 5), not Linux input-event codes. WebKit 2.52.6 maps exactly those
values in `WebEventFactory::createWebMouseEvent()`; sending `BTN_LEFT` (0x110)
and friends matches no button, and the click is silently discarded. Axis
events use the 2D smooth form (`wpe_input_axis_event_type_mask_2d |
wpe_input_axis_event_type_motion_smooth`) with positive Y meaning scroll up.

`webview-pointer-test` exercises move, left/right button and wheel forwarding
end-to-end through a real `WebKitWebView` with synthetic Qt events.

## Presentation

The engine presents frames in one of two ways, chosen at startup:

- **EGL presentation (default).** The UI process creates a surfaceless EGL
  display/context and initializes the FDO renderer with it
  (`wpe_fdo_initialize_for_egl_display()`), then creates the exportable with
  `wpe_view_backend_exportable_fdo_egl_create()`. The web process receives a
  hardware EGL display, so WebKit rasterizes with the GPU. Each exported EGL
  image is imported into a texture, read back into a `QImage` and painted by
  the widget; the last painted frame is retained so repaints do not flash the
  background. The readback is still a GPU→CPU copy: zero-copy GL presentation
  (a `QOpenGLWidget`/QRhi presenter) remains future work.
- **Software SHM presentation (fallback).** When EGL is unavailable or
  `IRIDIUM_DISABLE_GPU_PRESENTATION=1` is set, the engine initializes the FDO
  SHM renderer and forces `LIBGL_ALWAYS_SOFTWARE=1`. The SHM-only renderer
  cannot provide an EGL display to the web process; when Mesa takes the
  hardware path, EGL display creation fails and WebKit aborts the web process
  (`PlatformDisplayLibWPE`). An explicit `LIBGL_ALWAYS_SOFTWARE` setting always
  wins.

The GLib main context is pumped from a Qt timer (default 5 ms,
`IRIDIUM_GLIB_PUMP_MS` overrides it). Buffers are released and frames
acknowledged after Qt has painted them.

## User agent

Pages receive a plain `Iridium/<project version>` user agent (currently
`Iridium/0.0.1`, taken from the CMake project version). It intentionally has no
Mozilla/AppleWebKit/Safari tokens, so sites that sniff for those will treat the
browser as unknown.

Setting `WebKitSettings:user-agent` alone is insufficient: the GLib port's
[`WebPage::platformUserAgent()`](https://github.com/WebKit/WebKit/blob/wpewebkit-2.52.6/Source/WebKit/WebProcess/WebPage/glib/WebPageGLib.cpp)
can replace even a custom agent using its
[`UserAgentQuirks`](https://github.com/WebKit/WebKit/blob/wpewebkit-2.52.6/Source/WebCore/platform/glib/UserAgentQuirks.cpp).
This was reproduced for DuckDuckGo, Google Accounts/Drive/Docs and PayPal, in
both JavaScript and HTTP request headers.

Iridium disables `enable-site-specific-quirks` before navigation to retain its
own identity. This public setting also disables other site-specific engine
workarounds: WPE has no separate public switch for only user-agent quirks.
The choice favours consistent identification over automatic compatibility
spoofing. No page-visible JavaScript override is used.

`webview-useragent` checks the exact agent on `data:` pages and synthetic HTTPS
origins, then uses a loopback HTTP proxy to serve ordinary and quirked hosts
without contacting real websites. It verifies main requests, redirects,
stylesheets, scripts, iframe and dedicated-worker requests, document/frame/worker
`navigator.userAgent`, page and worker `fetch()`, and reloads. These fixtures
check identification, not whether the real websites accept the plain agent.

## Benchmarking

`./build/speedometer-benchmark [iterations]` runs Speedometer 3 through the
real `WebKitView` (headless with the offscreen Qt platform by default) and
prints the score. Measured on the AMD Phoenix development machine with 10
iterations:

| Presentation | Score | Wall time |
| --- | --- | --- |
| Software SHM | 10.5–10.7 | ~53 s |
| EGL (GPU) | 14.0–14.8 | ~35 s |

Skia painting-thread sweeps showed the defaults are already optimal:
`WEBKIT_SKIA_GPU_PAINTING_THREADS=1` matches the default, `0` is worse (12.5),
and forcing `WEBKIT_SKIA_CPU_PAINTING_THREADS` to 2 or 4 lowers the score
(12.0/12.7). The GLib pump interval (1–20 ms) showed no consistent effect under
the current measurement noise.

## Runtime media dependency

WPE WebKit 2.52.6 aborts the web process when it cannot create a GStreamer
audio sink: `MediaPlayerPrivateGStreamer::createAudioSink()` calls
`createPlatformAudioSink()`, which falls back to `autoaudiosink`, and the
failed `RELEASE_ASSERT(audioSink)` terminates the process. Arch's `wpewebkit`
package does not depend on `gst-plugins-good` (which provides `autoaudiosink`),
so any page with audio or video crashes the engine until it is installed:

    sudo pacman -S gst-plugins-good

`WEBKIT_GST_ENABLE_AUDIO_MIXER=1` does not avoid the abort: the internal audio
mixer also falls back to `autoaudiosink`.

For playback of the formats sites actually use, Arch's installed plugin set is
also missing codecs: `gst-plugins-bad` provides the Opus parser needed for
VP9/Opus WebM under MSE, and `gst-libav` provides the H.264 and AAC decoders
used by MP4. Without them `MediaSource.isTypeSupported()` rejects every pair
YouTube offers (WebM VP9 alone is accepted, but WebM Opus and MP4 H.264/AAC
are not) and the player reports that the video cannot be played.

    sudo pacman -S gst-plugins-bad gst-libav

This was verified before installation by pointing `GST_PLUGIN_PATH` at the
extracted plugins: with them, VP9/Opus and H.264/AAC report as supported and a
YouTube watch page loads and advances playback.

GPU video decoding uses VA-API through GStreamer's `va` plugin, which Arch
ships separately as `gst-plugin-va`. Its decoders (`vah264dec`, `vavp9dec`,
`vaav1dec`, `vah265dec`) have rank `primary + 1`, so WebKit picks them
automatically once the package is installed; no code change is needed. The
Vulkan Video decoders from `gst-plugins-bad` are not usable with WebKit's
video sink because they only output `memory:VulkanImage` and no download
element is inserted.

    sudo pacman -S gst-plugin-va

## System color scheme

WebKit only learns about the system appearance through WPE Platform settings
(`WPE_SETTING_DARK_MODE`), which the legacy libwpe embedding path never
receives. Using the WPE Platform API instead is not an option either: loading
`WPEDisplay` makes `isUsingWPEPlatformAPI()` return true and
`webkit_web_view_new(backend)` rejects the legacy backend. Iridium therefore
emulates `prefers-color-scheme` from the Qt color scheme
(`QStyleHints::colorScheme()`, falling back to the application palette):

- a user script injected at document start patches `matchMedia`, so the queries
  pages use to detect the scheme answer with the emulated scheme;
- the media queries of accessible stylesheets are rewritten to match, and
  rewritten back when the scheme changes;
- `color-scheme` is applied to the root element, plus a user style sheet while
  the system is dark.

`WebKitView::setPreferredColorScheme()` overrides the scheme at runtime; the
`webview-theme` test drives it in both directions. Limitations: cross-origin
stylesheets cannot be rewritten, and live scheme changes only update the main
frame (subframes pick up the new scheme when they load next).

## Downloads

WebKit's built-in policy decision downloads responses that are attachments;
the engine handles `WebKitDownload::decide-destination` to save files into the
XDG downloads directory (`QStandardPaths::DownloadLocation`, falling back to
the home directory) and to append " (n)" when the file already exists. The
network session's `download-started` signal announces downloads; each view
tracks only the downloads it started and reports progress, completion and
failure through `WebView::setDownloadStateHandler()`. `cancelDownload()`
cancels an active download.

The sidebar bottom row shows a new-tab button on the left and a downloads
button on the right; the downloads menu lists active transfers with a progress
bar and a cancel button, and finished files with an open button. Closing a tab
cancels the downloads that tab started (a browser-level download manager can
lift this limitation later).

## Tests

`ctest` covers the engine paths that have been brought up so far:

- `wpe-platform-probe` — WPE Platform 2 display/view creation.
- `webview-pointer` — mouse move, left/right button and wheel forwarding.
- `webview-gl` — WebGL context creation, readback and 2D canvas rendering.
- `webview-render` — painted frame colors and orientation through the
  presentation path (EGL readback or SHM fallback).
- `webview-useragent` — the engine reports the Iridium user agent.
- `webview-theme` — `prefers-color-scheme` emulation for `matchMedia` and
  stylesheet media queries.
- `webview-download` — link-triggered file download, destination handling and
  content verification (uses a temporary XDG downloads directory).
- `webview-media` — generates a small WebM (VP8 + Vorbis) with GStreamer and
  verifies playback through a real `WebKitWebView`; skips itself (exit 77) when
  the GStreamer encoder elements are unavailable.

The YouTube/MSE path is not part of `ctest` because it depends on the live
site; it was verified manually (H.264/AAC and VP9/Opus MSE support, VA-API
decoders selected in the append pipelines).

Two of these were written after a defect was found rather than to reach coverage,
and both caught what they were written for. `webview-history` drives the real
`MainWindow`, which is what exposed a shutdown double-free: `addWidget`
reparents a view into the window's stacked widget, so Qt deleted it while
`Browser` still owned it. `settings-window-categories` builds all five
categories and checks that a visit recorded while the window was hidden appears
once it is shown, which is the only thing that distinguishes a real
refresh-on-show from a page that happens to look right on first construction.

## Lifetime/navigation

WebKit owns the web view/backend wrapper and WebKit process state; the host
owns its platform/display objects and presenter. Keep all GLib objects referenced
for as long as the WebKit view or queued frame callbacks need them, and destroy
the web view before its backend/display dependencies. Navigation is via
`webkit_web_view_load_uri()` at the engine adapter, reached from browser-owned
navigation methods—not from Qt UI code. Exact backend teardown semantics depend
on the adapter and remain unverified pending that API investigation.

The legacy adapter ownership is confirmed from WebKit 2.52.6 source: the
`WebKitWebViewBackend` takes ownership of the `wpe_view_backend` and calls its
destroy notify when released. Iridium supplies a destroy notify that destroys
the FDO exportable backend; `WebKitView` must not also call the FDO destroy
function during teardown, or the underlying WPE backend is destroyed twice.
