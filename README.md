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

The package names in the last column are the ones that provide the files the
build actually opens, verified against the packages this project was developed
on. On another distribution the names differ; the Component column gives the
distro-independent identifier to search for.

**Installing on Arch**

```sh
sudo pacman -S --needed \
    cmake ninja gcc pkgconf \
    qt6-base qt6-svg \
    ksvg \
    wpewebkit wpebackend-fdo wayland libglvnd \
    gstreamer gst-plugins-good gst-plugins-bad gst-libav gst-plugin-va
```

Add `python python-yaml` if you also want the documentation tooling to run; see
the last table.

**Installing on another distribution**

The build asks `pkg-config` and CMake for these identifiers, so the packages to
look for are the ones that provide them:

| Identifier the build asks for | Provides |
| --- | --- |
| `wpe-webkit-2.0`, `wpe-platform-2.0` | WPE WebKit and the platform abstraction |
| `wpebackend-fdo-1.0` | The FDO embedding backend |
| `wayland-server` | The Wayland server library the backend links |
| `egl`, `glesv2` | The GL loader's development files |
| `Qt6Widgets`, `Qt6Network`, `Qt6Sql`, `Qt6Test` | Qt 6 base |
| `Qt6Svg` | Qt 6 SVG |
| `KF6Svg` | KDE Frameworks 6 SVG |

**Toolchain**

| Component | Why | Arch package |
| --- | --- | --- |
| CMake 3.21 or newer | The build system | `cmake` |
| Ninja | The build generator the commands above use | `ninja` |
| A C++20 compiler | `CMAKE_CXX_STANDARD 20`; the reference build uses GCC 16 | `gcc` |
| pkg-config | Locates WPE and the GL loader | `pkgconf` |

**Qt 6**

| Component | Why | Arch package |
| --- | --- | --- |
| QtCore, QtGui, QtWidgets | The window, sidebars and panels | `qt6-base` |
| QtNetwork | Extension update checks over HTTP | `qt6-base` |
| QtSql | Per-profile history and extension storage | `qt6-base` |
| QtTest | `QSignalSpy` in the widget tests | `qt6-base` |
| QtSvg | Icons in the tab list and window decoration | `qt6-svg` |

The Wayland platform plugin and the SQLite driver both ship inside `qt6-base`;
they are not separate packages. (`qt6-wayland` is the QtWayland *compositor*
library and is not needed here.)

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
| libwayland | `wayland-server.pc`, needed to build against the backend | `wayland` |
| libglvnd | The EGL, GLESv2, GL, GLX and OpenGL development files the browser links | `libglvnd` |

**Media playback**

Needed at runtime for audio and video. `wpewebkit` does not depend on any of
them, so they must be installed explicitly.

| Component | Why | Arch package |
| --- | --- | --- |
| GStreamer 1.0 | The media pipeline WPE uses | `gstreamer` |
| gst-plugins-good | Audio sink. Without it WPE 2.52.6 aborts the web process when a page loads any audio or video | `gst-plugins-good` |
| gst-plugins-bad | The Opus parser MSE playback needs for WebM | `gst-plugins-bad` |
| gst-libav | H.264 and AAC decoders; without them YouTube and similar sites report that the browser cannot play video | `gst-libav` |
| VA-API plugin | GPU video decoding | `gst-plugin-va` |

**Optional, for the documentation tooling**

| Component | Why | Arch package |
| --- | --- | --- |
| Python 3 | Runs the two documentation tests | `python` |
| PyYAML | Read by the coverage-database checker | `python-yaml` |

Without these, `extensions-coverage` reports as skipped rather than failed, and
the other 22 tests are unaffected. They are only needed if you want to
regenerate `docs/extensions/` after changing the specification pins.

### Runtime notes

- The Qt SQLite driver must be present at runtime; browsing history and
  extension storage depend on it. It ships inside `qt6-base`. If it is missing,
  browsing still works and history reports the problem once on stderr instead of
  being written.
- `mesa` provides the GL driver. It is not needed to build — only `libglvnd` is,
  since that is what supplies the libraries the browser links — but a Wayland
  session needs a driver to present anything.
- Rendering uses the GPU through the FDO backend by default, reading the frame
  back into the Qt widget. Set `IRIDIUM_DISABLE_GPU_PRESENTATION=1` to force
  the software SHM fallback.
- Pages are told `prefers-color-scheme` from the active Qt colour scheme.

## The browser

The window uses a full-sidebar layout inspired by Zen: the tab list, navigation
buttons, address field and window controls sit in a sidebar against the right
edge while the page occupies the rest.

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
| `settings`, `settings-window-categories`, `settings-store` | The settings panes, as widgets |
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