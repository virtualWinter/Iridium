# WPE WebKit integration for WebExtensions

What the engine in front of us offers, what it does not, and what follows from
each. Everything here was read out of the installed headers
(`/usr/include/wpe-webkit-2.0/wpe`, WPE WebKit 2.52.6) or asserted by
`tests/webkit_extension_probe.cpp`, which fails if any of it changes.

## What the engine refuses to do

### Its own extension types do not work

`WebKitWebExtension` and `WebKitWebExtensionMatchPattern` exist and are declared
in `webkit.h`. Their constructors return `NULL` for every input and leave
`GError` unset, so a caller cannot distinguish a bad path from a build that does
not implement extensions at all.

The reason is a build option, not a missing feature. Upstream WebKit has a
complete WebExtensions implementation behind `ENABLE(WK_WEB_EXTENSIONS)`, and
the GLib/WPE wrapper contains a second definition of every entry point for when
that option is off:

```c
#else // ENABLE(WK_WEB_EXTENSIONS)

WebKitWebExtension* webkit_web_extension_new(const char *extensionPath, GError **error)
{
    return nullptr;
}
...
```

Those are the stubs this build was compiled into — `NULL` with no `GError`,
exactly what the probe observes. The installed WPE 2.52.6 was built with the
option off.

The probe asserts this. It is a negative assertion on purpose: if the option is
ever turned on, the test fails and delegating to the engine becomes a decision
to revisit rather than an accident.

Consequence today: manifests, match patterns and the extension lifecycle are
Iridium's own code, in `src/extensions/`. See the next section for why that may
not stay true.

### Upstream WebKit already implements WebExtensions

This was found by reading Apple's WebKit source after the local headers had been
exhausted, and it changes the picture. `Source/WebKit/UIProcess/Extensions/`
contains, on `main`:

| Upstream file | What it is |
| --- | --- |
| `WebExtensionController.{h,cpp}` | Loads, unloads, enables and disables extensions |
| `WebExtensionContext.{h,cpp}` | The per-extension context: background page, permissions, views |
| `WebExtensionMatchPattern.{h,cpp}` | A real match-pattern implementation |
| `WebExtensionURLSchemeHandler.h` | The extension resource scheme |
| `WebExtensionStorageSQLiteStore.cpp` | `storage` persistence |
| `WebExtensionDeclarativeNetRequestSQLiteStore.cpp` | Declarative rules persistence |
| `WebExtensionContextAPIStorage.cpp`, `…APIAlarms.cpp`, `…APIDeclarativeNetRequest.cpp` | The JavaScript API implementations |
| `WebExtensionMessagePort.h`, `WebExtensionTab.h`, `WebExtensionWindow.h`, `WebExtensionAction.h`, `WebExtensionSidebar.h`, `WebExtensionMenuItem.h`, `WebExtensionCommand.h`, `WebExtensionAlarm.h`, `WebExtensionDynamicScripts.h` | The rest of the object model |
| `wpe/WebExtensionWPE.cpp` | The WPE-specific part of the port |

The WPE port exists. It is a single file because almost everything is
platform-independent, but it is there and it is gated on the same option.

So the three APIs recorded below as `unsupported` are not unsupported by
WebKit. They are unsupported by *this build of* WebKit, and the remedy is a
build flag rather than an engine change.

### The strategic question this raises

Two coherent ways to build this, and they differ in more than effort.

**A. Keep Iridium's own host.** Requires a WPE build with
`WK_WEB_EXTENSIONS=OFF`, which is what is installed. Costs reimplementing what
upstream already has: match patterns, the extension scheme, content-script
worlds, background contexts, storage, alarms, DNR, ports. In exchange, the
extension layer stays ours.

**B. Delegate to WebKit's implementation.** Requires a WPE build with
`WK_WEB_EXTENSIONS=ON`. Costs depending on a non-stock engine build, and
inheriting WebKit's coverage rather than choosing it.

**Decision: B.** Delegating avoids rebuilding a platform upstream already
maintains, and spends Iridium's effort where it is actually differentiated: the
UI, tab and window services, permission decisions, profiles and containers. The
consequence accepted is that extension behaviour lives in a component we compile
rather than one we own, and that coverage becomes something to *measure* rather
than something to write.

### What B actually gives us, from the upstream API

`WebExtensionController` is the embedder-facing entry point. It is plain C++
behind `#if ENABLE(WK_WEB_EXTENSIONS)`, so a WPE build can use it directly rather
than through a Cocoa wrapper. Its interface says a lot about coverage:

| Upstream member | What it implies |
| --- | --- |
| `load` / `unload` / `unloadAll` / `dispatchDidLoad` | Extension lifecycle, including background teardown |
| `extensionContext(const WebExtension&)` | One context per extension: background page, permissions, views |
| `resourceLoadDidSendRequest`, `...DidPerformHTTPRedirection`, `...DidReceiveChallenge`, `...DidReceiveResponse`, `...DidCompleteWithError` | The `webRequest` lifecycle exists on the UI-process side |
| `handleContentRuleListNotification`, `resourceLoadWasBlockedByContentRuleList` | DNR runs on WebKit's `ContentRuleList` engine, inside the network process |
| `ENABLE(DNR_ON_RULE_MATCHED_DEBUG)` | `declarativeNetRequest.onRuleMatchedDebug` is a separate, optional flag |
| `inspectorWillOpen` / `inspectorWillClose` under `ENABLE(INSPECTOR_EXTENSIONS)` | `devtools.*` needs a *second* flag, and is plumbed to `WebInspectorUIProxy`, a Cocoa type |
| `websiteDataStore(std::optional<PAL::SessionID>)` | Per-session data stores: the mechanism containers need |
| `m_registeredSchemeHandlers` | The extension resource scheme is served by WebKit |
| `addItemsToContextMenu` under `#if PLATFORM(MAC)` | `menus` has no non-Cocoa integration point |

Two of those shape the plan:

- **`menus` will not work on WPE through this API.** Context-menu insertion is
  Mac-only. An extension that adds a menu item will load and then have no menu,
  which is exactly the silent partial support the compatibility database exists
  to catch.
- **`devtools.*` needs `ENABLE(INSPECTOR_EXTENSIONS)` as well**, and its
  plumbing here is Cocoa-shaped. Treat it as unavailable on WPE until a build
  proves otherwise.

Storage, alarms, ports, content-script worlds, the extension scheme, background
contexts, match patterns and DNR are all present and platform-neutral in the
tree.

### What stays Iridium's job

Delegating the JavaScript API does not delegate the browser:

- Loading, enabling and disabling extensions per profile, and the settings UI.
- Deciding permission grants, including optional and host permissions, and
  presenting them to the user.
- Where extension storage lives and how it goes when a profile does.
- Rendering action buttons, popups, sidebars and options pages in Iridium's own
  UI rather than a Safari-shaped one.
- Containers, which map onto `websiteDataStore(sessionId)`.
- Saying what an extension actually got. The compatibility database and the
  generated coverage documents are how that stays honest.

### The request pipeline, from the embedder side

In the installed build there is no route to the request pipeline: no SoupSession
accessor, `decide-policy` covers navigations only, `WebKitURIRequest` has no
header setter, and the shipped injected bundle is a stub. That was the original
reason for recording `webRequest` and `declarativeNetRequest` as unsupported.

It is no longer the whole story. Upstream's `WebExtensionController` receives
`resourceLoadDidSendRequest`, `resourceLoadDidPerformHTTPRedirection`,
`resourceLoadDidReceiveChallenge`, `resourceLoadDidReceiveResponse` and
`resourceLoadDidCompleteWithError`, and handles
`handleContentRuleListNotification` and `resourceLoadWasBlockedByContentRuleList`
for declarative rules. So the interception points exist behind the build flag;
what is not established from a header is whether the extension-facing API built
on them offers *blocking*, or only observation.

Until a build with the flag on is measured against the specification, these two
remain recorded as unavailable, but the reason has changed from "the engine
cannot do this" to "not in this build, and the extension-facing surface is not
yet measured". That is a much shorter list of blockers, and it is a build and
measurement task rather than an engine patch.

For reference, the injected-bundle route remains as it was: WPE's web process
extension mechanism does have request hooks (`willSendRequest`,
`didReceiveResponse`, `didReceiveData`, `didFinishLoading`,
`didFailLoading`, `connectionWillOpen`,
`didReceiveServerRedirectForProvisionalLoadForFrameShared`,
`willSendRedirectedRequestInternal` are all present in the library), but its
ABI header is not installed and the shipped `libWPEInjectedBundle.so` exports
only `WKBundleInitialize`. It is a fallback, not a plan.

So `webRequest` and `declarativeNetRequest` are recorded as **unsupported**, not
approximated. A browser that returned plausible-looking but wrong results from
`onBeforeRequest` would be worse than one that refuses: extensions built on it
would filter traffic incorrectly and the failures would be invisible.

The path to changing that decision, in order of preference:

1. Ship an upstream WebKit patch that exposes a request-interception API, and
   depend on the patch.
2. Implement a web process extension against a vendored, pinned copy of the
   bundle header matching the exact WPE version, with a probe test that fails
   loudly if the ABI does not match.
3. Accept the gap, and say so in the compatibility database.

### No client for the inspector

The only public inspector entry point is `webkit_web_view_toggle_inspector`,
which opens a UI. The remote inspector does exist in the binaries: the string
`WEBKIT_INSPECTOR_SERVER` appears in `libWPEWebKit-2.0.so`, and there is a full
`RemoteInspectorClient` implementation and an inspector HTTP server string. But
no public API hands the embedder a socket path or a connection, and the
web process is launched by the library rather than by us.

So `devtools.*` is recorded as unavailable, with a probe task rather than a
design: if the web process honours `WEBKIT_INSPECTOR_SERVER` when it inherits the
environment, a client speaking WebKit's remote inspector protocol could be built
from the browser process. That is an experiment, and until it works the honest
answer is unsupported. The intended shape, if it works:

```text
browser.devtools.*        extension-facing
          ↓
Iridium Inspector API     our own abstraction over a protocol client
          ↓
WebKit remote inspector   protocol client in the browser process
          ↓
extension's devtools page its own WebKit view, via the inspector's own channel
```

Upstream's extension tree does carry `inspectorWillOpen(WebInspectorUIProxy&,
WebPageProxy&)` under `ENABLE(INSPECTOR_EXTENSIONS)`, so there is a second build
flag to flip and an existing hook to land on. But `WebInspectorUIProxy` is a
Cocoa type and that flag is separate from the one that enables extensions, so
`devtools.*` remains the least likely of the three to work on WPE. Treat it as a
stretch goal and measure it last.

## What the engine does offer

| Need | Mechanism | Notes |
| --- | --- | --- |
| Isolated content scripts | `WebKitScriptWorld`, `webkit_user_script_new_for_world` | One world per extension per frame; globals are separate, DOM is shared |
| Per-extension API channel | `webkit_user_content_manager_register_script_message_handler_with_reply(name, world_name)` | Reply gives a promise; per world keeps extensions apart |
| Injection point | `WebKitUserScript` | Document start or document end only |
| Frame scope | `WebKitUserContentInjectedFrames` | Top frame or all frames |
| Extension resources | `webkit_web_context_register_uri_scheme` | Plus `WebKitSecurityManager` for local/secure/CORS flags |
| Request metadata in the scheme handler | `WebKitURISchemeRequest` | Method, headers, body; `finish` / `finish_with_response` / `finish_error` |
| Background contexts | `WebKitWebView` | A hidden view is a background page or worker host |
| Cookies | `WebKitNetworkSession::get_cookie_manager` | Per session, so containers work |
| Site data | `WebKitNetworkSession::get_website_data_manager` | For `browsingData` |
| Proxy | `WebKitNetworkSession::set_proxy_settings` | Per session |
| Find in page | `WebKitFindController` | Per view |
| Storage partitions | `webkit_network_session_new(data_directory)` | The mechanism behind containers |
| Custom themes | `webkit_web_view_get_theme_color` | Read-only |

The probe asserts the first six rows and the cookie/website-data/proxy rows.

## Consequences for the design

### Extension resources: `moz-extension://<uuid>/`

A custom URI scheme gives us a real origin per extension, which is what makes
`localStorage`, `indexedDB` and `caches` partitioned for free, and what makes
`web_accessible_resources` enforceable.

```text
moz-extension://<uuid>/popup.html
                     │
                     ▼
       WebKitURISchemeRequest
                     │
        ┌────────────┴────────────┐
        │  does the uuid exist?   │──no──▶ 404
        │  is the path inside the │──no──▶ 404 (traversal attempt)
        │  extension root?        │
        │  is it web accessible   │──no──▶ 403
        │  when the referrer is   │
        │  a page?                │
        └────────────┬────────────┘
                     ▼
              ExtensionResourceServer reads the file
```

Two things to verify with a probe before relying on this, because the headers do
not promise them:

- Whether `WebKitURISchemeRequest::get_path()` receives the path with or
  without the authority, for a scheme that has one.
- Whether registering a scheme as local and secure is enough to give it an
  origin distinct per authority, which is what `moz-extension://<uuid>/` needs.

Both are answerable in a test that loads one page and asks for its origin. If the
answer is that per-authority origins do not work for custom schemes, the
fallback is a distinct scheme per extension
(`moz-extension-<n>://…`), which the probe should also check before it is
needed.

The current build already returns `iridium-extension://<id>/<path>` from
`runtime.getURL`, but that string is not backed by a scheme handler, so it is a
name rather than a URL. It must be replaced before any extension page exists.

### Content scripts: worlds, not the page world

Target shape:

```text
page world                     content-script world (one per extension)
┌──────────────────┐          ┌──────────────────────────────┐
│ window.foo = 1   │          │ window.foo === undefined      │
│ browser absent   │          │ browser.* present             │
│ Array.prototype  │          │ Array.prototype (own copy)    │
│   shared with ↓  │          │   invisible to the page       │
│ the page         │          │                               │
│         shared DOM (document, elements, storage)              │
└──────────────────┘          └──────────────────────────────┘
```

Each extension gets a world name derived from its id. The message channel is
registered against that same world name, so a page cannot reach an extension's
channel and one extension cannot reach another's.

This is Apple's documented recommendation, not just a reasonable choice.
`WKContentWorld.h` in WebKit says so directly:

> "If you are writing a general purpose web browser that supports JavaScript
> extensions, you would use a different client WKContentWorld for each
> extension."

It also documents two consequences worth designing around, because both are
true of `webkit_script_world_new_with_name` as well:

- **A world is a namespace, not a store.** "If you store a variable in
  JavaScript in the scope of a particular WKContentWorld while viewing a
  particular web page document, after navigating to a new document that variable
  will be gone." So a content script's state does not survive navigation, and
  anything that must survive belongs in the browser process.
- **A world belongs to one view.** State in a world in one `WKWebView` does not
  exist in the same world in another. So a per-extension world name is a
  per-(extension, view) identity, and cross-view state cannot live in the world
  either.

The prototype isolation matters more than it first appears: because each world
has its own copies of the built-in prototypes, a page that redefines
`document.querySelector` before a content script runs cannot affect what that
content script sees. Content scripts do not inherit page-side tampering.

Content scripts run in the page world today (`webkit_user_script_new`, not
`webkit_user_script_new_for_world`). That is the largest single correctness and
security gap in the current implementation, and it is recorded as such rather
than as an engine limitation, because the mechanism to fix it is present.

### `run_at` and the missing `document_idle`

`WebKitUserScriptInjectionTime` has two values: document start and document
end. There is no document idle.

The plan for `document_idle` is document-end injection plus a page-side idle
callback, which reproduces the documented ordering (after DOM content loaded, at
a low-priority idle moment) without an engine hook. That is a real
implementation of the guarantee rather than an approximation of it, and it needs
a test that a `document_idle` script observes a complete DOM, because that is
exactly the property a naive implementation gets wrong.

`match_origin_as_fallback` needs the origin of a frame's creator, which WebKit
does not expose. The origin has to be threaded through injection from the
browser process, where the initiator is known.

### Background contexts are hidden views

A background page and an MV3 service worker are each a hidden `WebKitWebView`
against the browser's context, with the extension's own origin and its own
lifetime rules. No tab, no window, no address bar, no user-reachable surface.
MV3 lifecycle management lives in the browser process (see
[execution-model.md](execution-model.md)); the view only runs JavaScript.

### Containers are separate network sessions

`webkit_network_session_new(data_directory)` per container gives each container
its own cookie jar, cache and website data, which is what the specification means
by a contextual identity. Adding a container tab means choosing a session for the
view, not attaching a label to a tab.

## What must be probed before Phase 4

Network and DevTools work depends on answers that the headers do not give. Each
is cheap to answer and cheap to be wrong about.

| Question | Why it matters | Method |
| --- | --- | --- |
| Does the web process honour `WEBKIT_INSPECTOR_SERVER` from the inherited environment, and is the socket reachable from the browser process? | Decides whether `devtools.*` is possible at all | Start a view, look for the socket, connect |
| Is there a network-process bundle loader distinct from the web-process one? | Decides whether `webRequest` has any route | Inspect the shipped loader path and the library's strings |
| Does `decide-policy` fire for subresource loads in any form? | Would give observation without a full interceptor | Load a page with a blocked subresource, watch the signal |
| Do custom schemes get per-authority origins? | Decides `moz-extension://<uuid>/` versus one scheme per extension | Load two pages, compare `location.origin` |
| Does `WebKitURISchemeRequest::get_path()` include the authority? | Decides the resource server's path parsing | Answer one request, print what arrives |
| Is `document_end` injection before or after `DOMContentLoaded` listeners? | Decides the `document_idle` emulation's accuracy | Compare a script's `readyState` at injection |
| Does a `UserScript` world see the same `document.cookie` as the page? | Whether cookies are readable from a content script | Compare across worlds |

Each of these becomes a `tests/webview_*_test.cpp` case when it is answered. A
yes/no with no test behind it is a note, not a design.

## What this means for the compatibility database

Three API groups are unsupported for engine reasons, and their entries say so:

- `webRequest` — no per-request hook, no header setter, no bundle ABI.
- `declarativeNetRequest` — the rules engine would have nothing to apply itself
  to.
- `devtools.*` — no client API for the inspector.

`contentScriptGlobalScope` is planned but its Firefox semantics
(`cloneInto`, `exportFunction`) have no WebKit primitive; a plain object copy
would differ in exactly the way that matters for passing a live DOM object, so
it needs a decision rather than an implementation.