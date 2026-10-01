# Architecture

The intended dependency direction is UI → Browser Core → `engine::WebView` →
WPE WebKit. The Qt application/window are the initial native shell. Browser
state and behavior will remain outside WebKit; WebKit supplies web-platform
execution and rendering only. The engine interface is intentionally narrow and
exists to contain WebKit-specific code, not to promise portability to other
engines.

The first milestone remains an Iridium-owned Qt window with web content in a
resizable content region. WPE's native toplevel must not be mistaken for such an
embedded content region.

`Browser` owns the active tab/view collection and tab creation, removal, ordering,
and navigation requests. It also keeps a small pool of closed views and hands
one back on the next `newTab`, because building and tearing down a WebKit view
per tab retains several megabytes per page load inside the engine that are never
returned. Closed views are navigated to `about:blank` so the document's memory
goes back to the web process, and their UI handlers are cleared on close so a
pooled view cannot call back into a window that may no longer exist.
The extensions layer sits beside Browser Core and is engine-independent. WebKit
exposes `WebKitWebExtension` and `WebKitWebExtensionMatchPattern`, but in the
installed WPE 2.52 build their constructors return NULL for every input and
leave `GError` unset, and no API exists to install an extension into a web
context. Iridium therefore parses manifests, matches URLs and injects content
scripts itself.

Extensions are discovered from the XDG paths in `ExtensionPaths`: the user
directory `XDG_DATA_HOME/iridium/extensions` first, then any
`XDG_DATA_DIRS/*/iridium/extensions`, so a user install shadows a system one.
Per-profile state, including `storage.local`, lives under
`XDG_DATA_HOME/iridium/profiles/<profile>`. See "Profiles" below.

## Profiles

A profile is a directory holding everything a browsing session accumulates:
extensions enablement and granted permissions, extension storage, settings, and
history. `ProfileManager` enumerates them and remembers the current one in
`XDG_DATA_HOME/iridium/settings.ini`, which sits outside the profiles so it can
be read before one is chosen. A directory without `profile.json` is not offered,
so a stray folder cannot become a profile, and the directory name is
authoritative — it is what every path is built from, so a mismatched field in
the file must not redirect it.

`SettingsStore` holds a `QSettings` behind a pointer rather than by value,
because the file is reopened on a switch. Reads and writes go through helpers
that return defaults when no profile has been chosen, so the store is safe to
touch before startup finishes.

Switching profiles is applied by restarting rather than in place. Open tabs, the
views behind them, and the extension state they were built against all belong to
the profile that created them, and a WebKit web context is process-global, so
there is no way to swap the data without rebuilding the tabs. The profiles pane
says so rather than appearing to switch and leaving the old session showing.

Two ownership rules fall out of this and are enforced rather than documented
only. A view's widget is reparented into the window's stacked widget by
`addWidget`, so Qt would delete it along with the window while `Browser` still
owns it; `MainWindow::releaseTabs` hands the views back before either destructor
runs. And a pooled view outlives the window that displayed it, so `recycle`
unparents it — `removeWidget` only takes it out of the stack, which would leave
Qt to delete a view the pool still holds.

## History

`HistoryStore` is SQLite rather than a flat file because history grows without
bound and needs substring search and time-range queries; a JSON file would be
read and rewritten in full for every visit. One row per URL, with a visit count
and the last visit time, so a page visited repeatedly is one entry the way a
browser history shows it.

A visit is recorded on navigation, when the URL is known but the title usually
is not, and the title is filled in by the second `recordVisit` when it arrives.
That is why an empty title has to bind as an empty string rather than as SQL
`NULL`: it is the common case, not an edge case, and the column is `NOT NULL`.
Ties in `last_visit` are broken by rowid, which SQLite assigns in insertion
order, because two visits within the same millisecond would otherwise be
indistinguishable and pruning could discard the entries added last.

History records nothing from internal pages (`about:`, `iridium-extension:`,
`data:`, `blob:` and friends), which is enforced in `MainWindow` rather than in
the store. Growth is bounded two ways: a retention sweep by age and a cap on
total entries, so the table cannot grow without limit.

`ExtensionHost` attaches one `ScriptBridge` per view and owns the
`WebKitUserScript` injected at document start, the same mechanism the color
scheme emulation already uses. Content scripts are delivered by injecting that
one script, so each view has a single source rather than one per extension.
Because several extensions can share a frame, the shim installs `browser` once
and the injector declares the running extension ahead of each of its files
(`window.__iridiumBeginExtension`), so `runtime.id`, `getManifest()` and
storage calls stay attributed to the right caller.

Unavailable methods reject their promise with a message naming the method and
the reason. Resolving to `undefined` would let an extension read silence as
success. Background pages and service workers are not implemented because
nothing in this WebKit build hosts them; `runtime.sendMessage` and friends
reject rather than pretend.

Two details the transport depends on. A handler signals a refusal by returning
`{"message": ...}`; the API layer re-wraps that as `{"error": ...}`, and the
injected shim converts an `error` on the reply back into a rejected promise. The
native side cannot reject a promise from outside JS, so without that step a
refusal would resolve and an extension would read it as success.

Permissions are enforced in the API layer rather than merely described in
settings. Every `tabs`, `windows`, `storage` and `i18n` handler is registered
with the permission it needs; the calling extension's id travels in the message
and is checked against the manifest's required permissions plus the optional ones
the user granted. Only optional permissions can be granted, so the settings UI
cannot widen an extension's reach beyond what its manifest asked for.

Extensions have a run order, which is what injection iterates, so two
extensions' content scripts always run in the same relative order and a
settings change to it takes effect on already-open tabs. Ids that no longer
exist are skipped rather than allowed to hide the remaining ones.

The settings window is a `QDialog` in `src/ui/settings`, created on first use
and reused, with `ExtensionsPage` driven entirely from the registry so a change
made anywhere stays consistent. An extension's id is its directory name, matching
what `ExtensionInstaller` installs and removes by; deriving it from the manifest
display name instead made a freshly installed extension impossible to enable or
remove. System-installed extensions appear in the list but cannot be removed,
since only the user directory is writable. `SettingsStore` is the single
persisted source for preferences, and the colour-scheme override is derived from
the stored scheme rather than kept alongside it, so the two cannot disagree.

`MainWindow` presents that state using a full-sidebar
layout inspired by Zen Browser: tabs, navigation/address controls, and window
controls live in the sidebar, next to stacked content pages; the
copied window-decoration controls live in the UI layer. The zero-tabs case
displays a Qt-only empty state and has no engine view.

`RightSidebar` is the right-hand panel, owned by `MainWindow` and built as a
plain view: its sections are fed from the services that already own the state
(the history store, the tab list, the extension registry), and its actions come
back out as signals that `MainWindow` wires to the same handlers the left
sidebar already uses. It follows the tab list through the tab model rather than
through calls in `newTab()` and `closeTab()`, so a tab opened by any path
updates it and a panel cannot become a second source of truth for tab state.
That is what lets `tests/right_sidebar_test.cpp` exercise it as a widget with no
window, engine or profile. It is also where an extension's `sidebarAction` view
is meant to render once the extension host lands.
