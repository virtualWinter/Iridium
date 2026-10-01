# Iridium TODO

## Bootstrap
- [x] Initialize CMake workspace
- [x] Create Qt application
- [x] Create main browser window
- [x] Verify Qt application starts on Wayland

## WebKit
- [x] Investigate WPE Platform 2 API
- [x] Document WPE lifecycle
- [x] Verify WPE Platform 2 display/view can be initialized at runtime
- [x] Initialize WPE WebKit through the installed legacy backend ABI
- [x] Create WebKit view
- [x] Load https://example.com (load finished and SHM frames observed)

## Rendering
- [x] Determine WPE frame delivery mechanism from installed headers
- [x] Implement initial SHM presentation path (copy into Qt-owned widget)
- [x] Handle resize correctly
- [x] Implement frame release/acknowledgement
- [x] Hardware rasterization via EGL presentation (EGL image readback)
- [x] Speedometer 3 benchmark harness
- [ ] Investigate WPEBufferDMABuf
- [ ] Design DMA-BUF → Qt integration
- [ ] Implement production GPU presentation path (zero-copy GL)
- [ ] Measure frame-copy behavior

## Input
- [x] Mouse movement
- [x] Mouse buttons
- [x] Scrolling
- [ ] Keyboard
- [ ] modifiers
- [ ] focus
- [ ] IME/text input

## Basic browsing
- [x] URL navigation
- [x] back
- [x] forward
- [x] reload
- [ ] stop
- [x] title updates
- [x] favicon
- [ ] navigation events

## Browser UI
- [x] Tabs (new, select, close, reorder; browser-owned tab/view list; no-tabs state has no renderer)
- [x] Sidebar (Zen-style left sidebar: window controls, navigation buttons, address bar and tab list; new-tab and downloads buttons in the bottom row)
- [x] Downloads (XDG downloads directory, unique file names, progress/cancel/open menu)
- [x] System color scheme (prefers-color-scheme)
- [x] Right sidebar (panel host with collapsible Bookmarks / History / Tabs /
      Extensions sections, one open at a time, collapsible to a rail, Ctrl+B to
      toggle; History reads the profile store, Tabs follows the tab list, and
      Extensions toggles enable state in place. Bookmarks says it is not
      implemented rather than showing an empty list. Tested as a widget with no
      browser, engine or profile: `right-sidebar`)
- [ ] Right sidebar: extension panels via `browser.sidebarAction`, once the
      extension host lands. The panel host is the place it renders
- [x] Profiles (named directories under XDG data; per-profile extensions, extension storage, settings and history; create/switch/delete in settings; switching restarts because open tabs belong to the profile that created them)
- [x] History (per-profile SQLite store; records on navigation and title, skips internal pages; search, open, forget, clear and age-based deletion; retention + count pruning)
- [ ] Permissions

## Media
- [x] GStreamer runtime dependencies documented (audio sink, Opus parser, H.264/AAC decoders)
- [x] GPU video decoding (VA-API)
- [x] MSE playback (YouTube: H.264/AAC, VP9/Opus)
- [x] Playback regression test (webview-media)
- [ ] Fullscreen video
- [ ] Encrypted media (DRM)

## Extensions (WebExtensions platform)

Full design, inventory and status: `docs/extensions/`. Read
`docs/extensions/architecture.md` first; it is the map for everything below.

WebKit's `WebKitWebExtension` and `WebKitWebExtensionMatchPattern` are
non-functional in the installed WPE 2.52 build: their constructors return NULL
for every input and leave `GError` unset, and there is no
`webkit_web_context_add_extension`. The extension host is therefore implemented
in `src/extensions/` rather than delegated to the engine. `tests/webkit_extension_probe.cpp`
asserts this, so an engine upgrade that changes it fails the test instead of
invalidating the design quietly.

Two further quirks in this build are worked around in
`src/engine/webkit/WebKitScriptBridge.cpp`: the signal is spelled
`script-message-with-reply-received` (not the documented
`script-message-received-with-reply`), and its arguments arrive value-first.

### Status of the platform today

69 namespaces, 796 members and 43 manifest keys in the specification surface.
Five namespaces are reachable from extension JavaScript today, all `partial`,
through the hand-written shim that is about to be replaced:
`docs/extensions/api-coverage.md` is generated and is the authority; nothing here
restates a status.

The largest current gap is not a missing API: content scripts run in the page's
world instead of an isolated one, which is a security-model violation rather
than a limitation. WebKit's host fixes this as a side effect of being enabled.

### Strategy

The extension runtime is WebKit's, not ours. Upstream WebKit has a complete
WebExtensions implementation — controller, contexts, match patterns, the resource
scheme, storage, alarms, declarative rules, ports, per-extension content-script
worlds — behind `ENABLE(WK_WEB_EXTENSIONS)`, with a WPE port of its own. The
installed WPE 2.52.6 was compiled with that option **off**, which is why
`webkit_web_extension_new()` returns NULL with no `GError` set: the GLib wrapper
has a second definition of every entry point for exactly that case.

So the first task is the engine build, not our API layer. Iridium's own work is
the browser around it: what is installed, what it is permitted, where its data
lives, and how its surfaces look in Iridium's UI. Reasoning and the upstream API
surface are in `docs/extensions/webkit-integration.md`; the decision is recorded
under "Decision: B" there.

What this changes about the phases below: the API namespaces are no longer
written here, they are *measured* here. `compatibility.yaml` plus the generated
catalog and dispatcher are the conformance harness that turns "WebKit supports
X" into a claim with a test behind it.

### Phase 0 — specification and inventory (done)

- [x] Inventory of the API surface, generated from pinned upstream definitions
      (MDN `browser-compat-data`, MDN `content`, Gecko schemas) rather than
      from memory: 69 namespaces, 796 members, 43 manifest keys, 54 permissions
- [x] `tools/webext/fetch.py` + `pins.json` (pinned commits, refreshable)
- [x] `tools/webext/inventory.py` → `docs/extensions/data/*.json`
- [x] `docs/extensions/compatibility.yaml` as the single source of truth for
      status, with rules the tooling enforces (`full` needs a named test,
      `unsupported` needs a reason, `missing` names must be real members)
- [x] `tools/webext/coverage.py` → generated `api-coverage.md` and
      `manifest-coverage.md`, wired into CTest as `extensions-coverage`
- [x] Engine capability probe (`webkit-extension-probe`) pinning the facts the
      architecture depends on
- [x] `docs/extensions/architecture.md`, `execution-model.md`,
      `security-model.md`, `compatibility.md`, `webkit-integration.md`

### Phase 1 — the engine build (blocks everything)

Nothing below can be finished until this is, because without it extensions do
not run at all.

- [ ] Build or obtain WPE with `ENABLE_WK_WEB_EXTENSIONS=ON`, pinned to a known
      upstream commit, and record it the way `tools/webext/pins.json` records the
      specification sources
- [ ] Confirm the runtime effect: `webkit_web_extension_new()` returns a real
      object with `GError` set on failure. `webkit-extension-probe` flips from
      asserting the stubs to asserting the working API, which is the signal that
      this phase is done
- [ ] Decide `ENABLE_INSPECTOR_EXTENSIONS`: it is a separate flag and gates
      `devtools.*`. Assume off; measure before enabling
- [ ] Decide `ENABLE_DNR_ON_RULE_MATCHED_DEBUG`: separate again, and only
      affects `declarativeNetRequest.onRuleMatchedDebug`
- [ ] Report which WPE GTK/GLib entry points the option exposes
      (`WebKitWebExtension`, `WebKitWebExtensionContext`,
      `WebKitWebExtensionController`) so the integration is written against the
      real surface rather than the 2.52 headers
- [ ] Establish how the build is reproduced: a PKGBUILD or equivalent, pinned, so
      a machine can be brought up from nothing

### Phase 2 — embedder integration

- [ ] Extension lifecycle: discover, load, enable, disable, uninstall, per
      profile, with WebKit's controller as the actor
- [ ] Remove the hand-written `browser.*` shim (`ExtensionApi.cpp`) before
      enabling WebKit's host: two definitions of `browser.tabs` in one frame
      conflict, and the page-visible shim would shadow the isolated-world API
- [ ] Permission decisions: grant API and host permissions, optional permissions
      on request, `activeTab`, and revocation that reaches a running extension
- [ ] Storage location per profile, and removal of extension data when a profile
      or an extension goes
- [ ] Extension UI in Iridium's own shell: action button, popup, options page,
      sidebar, `devtools_page`. Iridium's layout is not Safari's, so these are
      surfaces we render, not ones we inherit
- [ ] `chrome.*` compatibility: confirm WebKit provides it, and add only what it
      does not
- [ ] Containers on `websiteDataStore(sessionId)`: cookie, storage and cache
      partitioning per container, with a real `cookies.getAllCookieStores`

### Phase 3 — conformance harness

The work begun for the old path, repurposed: this is how "WebKit supports X"
becomes a measured claim.

- [x] Code generator: Gecko schemas → `src/extensions/generated/ApiCatalog.*`
      (namespaces, members, parameters, permissions, context masks, MV bounds)
- [x] `ApiRegistry` reading that catalog, `ApiDispatcher` enforcing the order the
      security model requires, and tests for both (`extensions-dispatcher`)
- [x] The generator is checked in CI (`extensions-codegen --check`)
- [ ] Coverage measurement: run real extensions against a WebKit-enabled build
      and record each member's status in `compatibility.yaml`
- [ ] Extend the generator to emit C++ types and validators, once a member is
      known to need one
- [ ] Test fixtures under `tests/extensions/`, then the unmodified real-world
      set under `tests/extensions/compatibility/`

### Phase 4 — gaps WebKit does not close

Only what measurement shows is missing. Known candidates:

- [ ] `menus`: upstream's context-menu integration is `#if PLATFORM(MAC)`, so
      this may need our own implementation or an upstream patch
- [ ] `devtools.*`: needs `ENABLE_INSPECTOR_EXTENSIONS`, and the plumbing is
      Cocoa-shaped
- [ ] Blocking `webRequest`: the controller receives the lifecycle events;
      whether the extension-facing API offers blocking is unmeasured
- [ ] `webNavigation` phases, if WebKit's events do not cover them
- [ ] Anything else the conformance run reports as missing

### Phase 5 — browser APIs still ours

Where WebKit's extension host does not reach browser services Iridium owns, the
service is added in `src/browser` and bridged — not duplicated in the extension
layer. `history`, `bookmarks` and `sessions` are the likely cases.

### Phase 6 — MV3

MV3's service-worker lifecycle is WebKit's (`WebExtensionContext`), so this is
verification rather than construction:

- [ ] Confirm MV2 background pages *and* event pages both run, and that a
      non-persistent background page is torn down when idle and restarted on the
      next event
- [ ] Confirm MV3 service workers are terminated when idle and that a persisted
      listener wakes them again. This is the part most likely to be partial, and
      it is the reason `runtime.onSuspend` matters
- [ ] Confirm listeners registered asynchronously do *not* persist, matching the
      specification rather than the implementation's convenience
- [ ] `optional_host_permissions` and the MV3 permission split

### Phase 7 — platform and advanced

Only what measurement shows as missing. Candidates already identified:

- [ ] `clipboard`, `idle`, `dns`, `captivePortal`, `publicSuffix` — platform
      layer work behind an API WebKit may not fully cover
- [ ] `notifications` — desktop notifications, if WebKit's are insufficient
- [ ] Native messaging, if WebKit's implementation does not cover Iridium's host
      manifest locations
- [ ] `i18n` `_locales` behaviour, if WebKit's localization is incomplete for
      the messages extensions actually use
- [ ] `contentScriptGlobalScope` (`cloneInto`, `exportFunction`) has no WebKit
      primitive; a plain object copy differs where it matters, so this needs a
      decision rather than an implementation

### Phase 8 — compatibility

- [ ] `chrome.*` facade: confirm what WebKit provides, add only the gap
- [ ] Real-world extension suite under `tests/extensions/compatibility/`,
      unmodified: content-script-only, MV2 background, MV3 worker, content
      blocker, userscript manager, password-manager-style, download manager,
      tab/session manager, theme, sidebar, devtools, native messaging
- [ ] Every failure recorded in `compatibility.yaml` against the API
      responsible, naming the extension that failed
- [ ] Automatic extension updates (still only version comparison; no update
      server)

## What is deliberately not being built

So that these do not quietly creep back in:

- A hand-written `browser.*` implementation of the 69 namespaces. WebKit
  provides them; two implementations in one frame conflict.
- A hand-written match-pattern, content-script injection or extension resource
  scheme. WebKit provides all three; ours exists only until it is enabled, and
  `src/extensions/MatchPattern.cpp` becomes dead code at that point.
- A rules engine in the browser process for `declarativeNetRequest`. WebKit's
  `ContentRuleList` engine already runs in the network process, where a DNR
  engine has to be.
