# Iridium

Linux-first Qt 6 desktop browser using WPE WebKit as its web engine.

The browser owns its application, its windows, tabs and profiles; the UI talks
to the engine only through the small interface in `src/engine/WebView.hpp`.
Nothing in the browser is built on Chromium, CEF, Qt WebEngine or Electron.

Licensed under the [ISC License](LICENSE).

## Building

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Run it with `./build/iridium`, optionally with a URL or page to open in the
first tab:

```sh
./build/iridium https://example.com
```

### Dependencies

Everything the build needs is listed here. Nothing else is required: there is no
submodule to fetch, no code generator to run by hand and no system component
Iridium downloads at configure time.

**Toolchain**

| Dependency | Why | Arch package |
| --- | --- | --- |
| CMake 3.21 or newer | Build system | `cmake` |
| Ninja | The build generator the commands above use | `ninja` |
| A C++20 compiler | GCC 12+ or Clang 16+ | `gcc` |
| pkg-config | Locates the WPE and GLES libraries | `pkgconf` |

**Qt 6**

| Component | Why | Arch package |
| --- | --- | --- |
| QtCore, QtGui, QtWidgets | The window, sidebar and panels | `qt6-base` |
| QtNetwork | Extension update checks over HTTP | `qt6-base` |
| QtSql | Per-profile history and extension storage | `qt6-base` |
| QtSvg | Icons in the tab list and window decoration | `qt6-svg` |
| QtTest | `QSignalSpy` in the widget tests | `qt6-base` |
| The Wayland platform plugin | Running on a Wayland session | `qt6-wayland` |

**KDE Frameworks 6**

| Component | Why | Arch package |
| --- | --- | --- |
| KF6Svg | SVG rendering for the copied window controls | `ksvg` |

**WPE WebKit and GL**

| Component | Why | Arch package |
| --- | --- | --- |
| WPE WebKit 2.52 | The web engine | `wpewebkit` |
| WPE Platform 2 | Display, view and buffer abstraction | `wpewebkit` |
| The FDO embedding backend | The backend this WebKit's public embedding API still requests; see [docs/wpe-integration.md](docs/wpe-integration.md) | `wpebackend-fdo` |
| libwayland | Wayland server library the backend links | `wayland` |
| EGL, GLESv2, GL, GLX, OpenGL | Hardware-accelerated frame presentation | `mesa`, `libglvnd` |

**Media playback (needed for audio and video)**

| Component | Why | Arch package |
| --- | --- | --- |
| GStreamer 1.0 | The media pipeline WPE uses | `gstreamer` |
| gst-plugins-good | Audio sink. Without it WPE 2.52.6 aborts the web process when a page loads any audio or video | `gst-plugins-good` |
| gst-plugins-bad | The Opus parser MSE playback needs for WebM | `gst-plugins-bad` |
| gst-libav | H.264 and AAC decoders; without them YouTube and similar sites report that the browser cannot play video | `gst-libav` |
| VA-API plugin | GPU video decoding | `gst-plugin-va` |

`wpewebkit` does not depend on any of the GStreamer packages, so they have to be
installed explicitly. On Debian and Fedora the package names differ but the set
is the same.

**Optional, for the documentation tooling**

| Dependency | Why | Arch package |
| --- | --- | --- |
| Python 3 | Runs the two documentation tests | `python` |
| PyYAML | Read by the coverage-database checker | `python-yaml` |

Without these, `extensions-coverage` reports as skipped rather than failed, and
the other 22 tests are unaffected. They are only needed if you want to
regenerate `docs/extensions/` after changing the specification pins.

### Runtime notes

- The Qt SQLite driver must be present at runtime; browsing history and
  extension storage depend on it. If it is missing, browsing still works and
  history reports the problem once on stderr instead of being written.
- Rendering uses the GPU through the FDO backend by default, reading the frame
  back into the Qt widget. Set `IRIDIUM_DISABLE_GPU_PRESENTATION=1` to force
  the software SHM fallback.
- Pages are told `prefers-color-scheme` from the active Qt colour scheme.

## The browser

The window uses a full-sidebar layout inspired by Zen: the tab list, navigation
buttons, address field and window controls sit in the left sidebar while the
page occupies the rest. A right-hand panel sits beside the page, with
collapsible Bookmarks, History, Tabs and Extensions sections; one is open at a
time, and Ctrl+B collapses the panel to its rail or brings it back. History
reads the profile's history store and searches it as you type, Tabs follows the
open tabs, and Extensions toggles an extension's enabled state in place.
Bookmarks reports that it is not implemented, since there is no bookmark store
yet.

Passing a URL opens it in the first tab. Ctrl+T opens another, Ctrl+H opens the
history pane and Ctrl+, opens settings, which manages extensions and profiles.

Profiles are directories under `XDG_DATA_HOME/iridium/profiles/<name>`, each
with its own extensions, extension data, settings and history. Switching
profiles restarts the browser, because open tabs belong to the profile that
created them.

## Extensions

Iridium targets the MDN WebExtensions specification, exposed as `browser.*` with
`chrome.*` as a facade. The extension runtime is WebKit's rather than ours:
upstream WebKit implements WebExtensions behind `ENABLE(WK_WEB_EXTENSIONS)`, and
the installed WPE 2.52.6 was built with that option off. The plan is to enable
it and own the browser around it.

`docs/extensions/` is the reference: the architecture, the execution and security
models, the engine hooks this depends on, and a generated per-API coverage table
generated from pinned upstream definitions. `docs/extensions/README.md` is the
index and the shortest useful summary of where things stand.

## Tests

`ctest` runs 23 tests. The engine-backed ones need a Wayland session or the
offscreen Qt platform and skip themselves when neither is available:

| Test | What it covers |
| --- | --- |
| `wpe-platform-probe`, `webkit-extension-probe` | What this engine build can and cannot do for extensions |
| `webview-pointer`, `webview-gl`, `webview-render`, `webview-useragent` | Input, WebGL, frame presentation, user agent |
| `webview-theme`, `webview-download`, `webview-media`, `webview-history` | Colour scheme, downloads, media playback, history recording |
| `webview-extension` | A content script from a real on-disk extension running in a matching page |
| `extensions-manifest`, `extensions-registry`, `extensions-paths`, `extensions-api`, `extensions-dispatcher` | Manifest parsing, match patterns, install and enable, the API surface, the dispatcher |
| `right-sidebar`, `settings`, `settings-window-categories`, `settings-store` | The panels and settings, as widgets |
| `browser-data` | Profiles and history storage |
| `extensions-coverage`, `extensions-codegen` | The generated specification inventory has not drifted from the documentation |

`./build/speedometer-benchmark [iterations]` runs Speedometer 3 through the
engine, which is separate from `ctest` because it takes minutes.

## Repository layout

```text
src/app/          process lifetime and startup
src/browser/      browser core: tabs, windows, history, profiles
src/engine/       the WPE WebKit adapter, frame presentation, script bridge
src/extensions/   the extension platform and its generated catalog
src/ui/           the Qt window, the sidebars and the settings panes
tools/webext/     the tooling that derives the extension inventory from
                  pinned upstream definitions
tests/            the test suite
docs/             architecture and integration notes
```

The dependency direction is `ui → browser → extensions → engine`. A browser
service an extension needs is added to `src/browser` rather than kept in the
extension layer, so there is only ever one copy of the state.