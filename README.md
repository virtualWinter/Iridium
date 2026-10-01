### This project is vibecoded!!!

# Iridium

Linux-first, Qt 6 desktop browser prototype using WPE WebKit as its web engine.
The browser owns its application and browser state; UI code talks to the engine
through the small interface in `src/engine/WebView.hpp`.

## Build

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Run with `./build/iridium`. WPE 2.52.6 headers and the Qt Wayland platform plugin
are needed on the development machine, along with the Qt 6 SQLite driver plugin
(`libqsqlite`) at runtime, which browsing history depends on. If the plugin is
missing, browsing still works and history reports once on stderr instead of
being written. Media playback additionally requires
GStreamer's `gst-plugins-good` (audio sink), `gst-plugins-bad` (Opus parser for
WebM/MSE), `gst-libav` (H.264/AAC decoders) and `gst-plugin-va` (GPU decoding);
the `wpewebkit` package does not depend on them. Without `gst-plugins-good` WPE
2.52.6 aborts the web process when a page loads audio or video, and without the
codec packages YouTube and similar MSE sites report that the browser cannot
play the video. Pages also receive the system color scheme
(`prefers-color-scheme`), emulated from the Qt color scheme. Rendering uses the
GPU by default (WPE FDO EGL presentation with a readback into the Qt widget);
`IRIDIUM_DISABLE_GPU_PRESENTATION=1` forces the software SHM fallback.
`./build/speedometer-benchmark [iterations]` runs Speedometer 3 through the
engine for performance work. The engine embeds WebKit through
the installed legacy WPE FDO backend because WebKit's public embedding API
still requests that backend ABI; see `docs/wpe-integration.md` for the boundary
and known limitations. The window uses a full-sidebar browser layout inspired
by Zen: tab list, navigation buttons, address field, and client-side KDE-style
window controls sit in the left sidebar, while the page occupies the remaining
window. Colors come from the active Qt system palette. Closing the final tab
leaves a no-tabs page with no WebKit view; use Ctrl+T or the + button to create
another. The sidebar bottom row holds the new-tab button on the left, a history
button and a downloads button on the right; downloads are saved to the XDG
downloads directory and can be opened or cancelled from that menu.

Browsing history and profiles are per-profile: each profile keeps its own
extensions, extension data, settings and history under
`XDG_DATA_HOME/iridium/profiles/<name>`. Ctrl+H opens the history pane, which
searches, reopens, forgets and clears entries; Ctrl+, opens settings, which
manages extensions and profiles. Switching profiles restarts the browser, because
open tabs belong to the profile that created them.

## Extensions

Iridium implements the WebExtensions platform itself, targeting the MDN
WebExtensions specification with `browser.*` as the primary namespace and
`chrome.*` as a facade over the same implementation. The platform design, its
execution and security models, the engine hooks it depends on, and a generated
per-API coverage table are in `docs/extensions/`. `docs/extensions/README.md` is
the index; `docs/extensions/api-coverage.md` says what actually works today.

Pass a URL to open it in the first tab, for example `./build/iridium https://arsn.cc`.

A right-hand panel sits beside the page, with collapsible Bookmarks, History,
Tabs and Extensions sections; one is open at a time, and Ctrl+B collapses the
panel to its rail or brings it back. History reads the profile's history store
and searches it as you type, Tabs follows the open tabs, and Extensions toggles
an extension's enabled state in place. Bookmarks reports that it is not
implemented, since there is no bookmark store yet. The panel is where an
extension's `browser.sidebarAction` view will render.
