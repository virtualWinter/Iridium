# Extension execution model

Where extension code runs, for how long, and who starts and stops it.

## Contexts

A context is a JavaScript global that extension code runs in, with its own
permission surface and its own lifecycle. The set of contexts an extension gets
is derived from its manifest:

| Context | Created by | Has `window` | Sees page DOM | Notes |
| --- | --- | --- | --- | --- |
| `ContentScriptContext` | `content_scripts`, `scripting.executeScript`, `userScripts` | no | yes | One world per extension per frame |
| `ExtensionPageContext` | popup, `options_ui`, `options_page`, `sidebar_action`, `devtools_page` panel, extension URL | yes | no | Own origin, own CSP |
| `BackgroundPageContext` | MV2 `background.scripts` / `background.page` | yes | no | Persistent or event page |
| `ServiceWorkerContext` | MV3 `background.service_worker` | no | no | Started and stopped by the browser |
| `DevToolsContext` | `devtools_page` | yes | no | Only `devtools.*` and a small allowlist |

Content scripts are the only context that can touch page DOM, and they do it
through a world boundary: WebKit's script world gives them their own globals, so
a page cannot see `browser` and a content script cannot see the page's
JavaScript variables. The DOM is shared, which is the specification's intent —
that is why an isolated world is the mechanism rather than a separate process.

Permissions differ per context, following the schema's `allowedContexts` and
`defaultContexts`:

- `runtime`, `i18n`, `extension`, `storage` are available in content scripts.
- `devtools.*` exists only in a devtools context, and only with the `devtools`
  permission, which the `devtools_page` key grants implicitly.
- `tabs`, `webRequest`, `cookies` and everything else are absent from content
  scripts. Reaching them requires messaging the background context.

## The extension host

Every extension process work happens in `ExtensionHost`, which owns:

- the set of installed extensions and their states;
- one hidden `WebKitWebView` per extension-owned JS context (background page or
  service worker);
- one script bridge per view, carrying that extension's identity;
- the content-script registry, for both manifest-declared and dynamically
  registered scripts;
- the message bus and the port table.

A hidden view is not a tab. It has no entry in `TabManager`, no address bar, no
window, and no way for the user to reach it. It is a `WebKitWebView` created
against the browser's `WebKitWebContext`, sized to nothing, kept alive only as
long as its context's lifetime rules say it should be.

## Lifetimes

### MV2 background page

```text
extension enabled ──> create hidden view ──> load background.html/.js
                                        │
                          (event arrives) │  persistent: already running
                                        │  event page: start on demand
                                        ▼
                                    dispatch
                                        │
                          (idle)        │  persistent: never
                                        │  event page: unload when idle
                                        ▼
                              stop, keep the listener registry
```

`persistent: true` means the view stays alive for the life of the extension.
`persistent: false` is an event page: the view is started when an event arrives,
and torn down when it goes idle. An event page is not unloaded while a port is
open or an extension page is visible, matching the documented behaviour that
views pin the background context.

An event page's teardown must not lose its listeners. That is why listeners live
in the event router in the browser process, not in the page's JavaScript heap.
The page is a place to run the handlers; the router owns the registration.

### MV3 service worker

MV3 is a different lifecycle, not MV2 with a flag. The worker is stopped when it
is idle, and started again when an event arrives for it:

```text
event occurs
     ↓
Event Router
     ↓
worker running?
 ┌───┴────┐
yes       no
 │         │
 │         ▼
 │      start worker
 │         │  replay persisted listeners
 │         │
 └────┬────┘
      ▼
dispatch event
      ↓
keepalive while work is pending
      ↓
idle
      ↓
terminate
```

The hard part is the middle: the browser must know, at the moment an event
fires, whether any extension has a listener for it. A worker that was terminated
cannot be asked. So:

- **Listeners are persisted.** When a worker registers `tabs.onUpdated`, the
  browser process records `(extension, event, filters)` and keeps it across
  worker termination, extension reload and browser restart.
- **Dispatch is replayed into the fresh worker.** On wake-up, the worker is
  re-evaluated from `background.service_worker`, and the router injects the
  persisted registrations before the first event is delivered.
- **The registration must be synchronous.** The specification requires listeners
  to be registered synchronously at the top level of the worker for exactly
  this reason. A listener registered in a `setTimeout` or after an `await` is
  not persisted, and the router must not pretend otherwise.
- **Keepalive is explicit.** `alarms`, `fetch` and an open port extend the
  worker's life; the timeout only starts when nothing is outstanding.
- **Termination is observable.** `runtime.onSuspend` fires when the browser is
  about to stop the worker, and an extension that needs to flush state listens
  for it. This is a real signal, not a best effort.

Every one of these is a browser-process responsibility. The worker itself is
just a hidden view that runs JS; it does not decide when to stop.

### Extension pages

Popup, options, sidebar and any `moz-extension://` URL are views. They are
created on demand, have a real origin, and are destroyed when closed. A
background page may hold `runtime.getBackgroundPage()` for a popup to talk to;
it does not, and must not, hold a reference to a view that can be closed.

## The event router

One router, in the browser process, for every event.

```text
Browser event
      ↓
ExtensionEventRouter
      ↓
for each enabled extension:
      │  is the API exposed in this context?      → context capability table
      │  does the extension hold the permission?  → PermissionManager
      │  do the event's filters match?             → events.Rule filters
      │  is a listener registered?                → persisted registrations
      ▼
deliver (starting a worker or event page if needed)
```

The router is deliberately separate from the code that raises the event.
`TabManager` knows a tab changed; it does not know that `tabs.onUpdated`,
`webNavigation.onCommitted` and `windows.onFocusChanged` are three different
subscriptions over the same fact. That mapping lives in one place, so adding an
event does not mean touching the browser service.

Filtering happens before delivery, never inside the listener, because a filter
that runs inside the listener still wakes a stopped MV3 worker. Filter forms
come from the specification's `events.Rule`:

- `tabs`, `extraInfoSpec` — the event declares what it can filter on, and the
  router checks the requested set before registering.
- `webNavigation`, `url` filters — matched against the URL of the frame.
- `webRequest`, `urls` — matched against the request URL; only relevant if the
  network surface exists.

## Messaging

Foundational, so it comes early.

### One-shot messages

```text
Content Script                Extension Page / Background / Worker
      │                                    │
      │  runtime.sendMessage(message)      │
      │───────────────────────────────────>│
      │                                    │  router: target extension(s)
      │                                    │  permission + context check
      │                                    ▼
      │                          onMessage listeners
      │                                    │  first listener that returns
      │                                    │  a promise or true owns the reply
      │  <───────────────────────────────────│
```

`runtime.sendMessage` with no receiver targets the extension's own other
contexts. `runtime.sendMessage` with an extension id, and `tabs.sendMessage`, are
the same mechanism with a resolved target. `tabs.sendMessage` to a frame is the
only way to reach a specific `ContentScriptContext`.

The response rule is the specification's, and it is easy to get wrong: the first
listener that returns a Promise or `true` owns the reply, and returning a
non-Promise, non-`true` value does not. There is no "last listener wins" and no
"first listener always wins". A listener that returns nothing lets the next one
answer.

### Ports

`runtime.connect()` and `tabs.connect()` produce a `Port`:

```text
Port
├── name
├── sender            who the other end is
├── postMessage()     one message, no reply
├── disconnect()      close this end
├── onMessage
├── onDisconnect
└── onDisconnect fired on the other end too
```

A port is a long-lived bidirectional channel held by the message bus, keyed by
an id, with both ends addressable. It keeps an MV2 event page alive (the
specification says so explicitly) and it keeps an MV3 worker alive while open.

Ports are not routed through UI objects. The bus is in `ExtensionHost`, and both
ends are addressed by port id, so a popup closing cannot take a background port
with it.

## Content scripts

Content scripts come from two places, and both end up in the same registry:

- **Manifest-declared**: `content_scripts`, persistent across restarts.
- **Dynamically registered**: `scripting.registerContentScripts` (MV3),
  `contentScripts.register` (MV2, Firefox), `userScripts` where available. Not
  persisted unless the API says so.

The full feature set that has to work:

| Feature | Behaviour required |
| --- | --- |
| `matches` | Inject where the URL matches |
| `exclude_matches` | Never inject there, even if `matches` hit |
| `include_globs` / `exclude_globs` | Filter `matches` further, in that order |
| `js` / `css` | Run in declaration order; CSS inserted as a stylesheet |
| `run_at` | `document_start`, `document_end`, `document_idle` |
| `all_frames` | Every frame, not just the top one |
| `match_about_blank` / `match_origin_as_fallback` | `about:blank`, `about:srcdoc` and `data:` frames that inherit an origin |
| `world` | `ISOLATED` (default) or `MAIN` |

Two of these need engine support this build does not have, and are planned
rather than assumed:

- **`document_idle`** has no engine equivalent: `WebKitUserScript` injects at
  document start or document end only. The plan is document-end injection plus a
  page-side idle callback, which reproduces the documented ordering. That has to
  be tested as such, because the difference from a native hook shows up as
  scripts running too early.
- **`match_origin_as_fallback`** needs the *creator* origin of a frame, not its
  URL. WebKit does not expose it, so the origin has to be threaded through
  injection: the browser process knows the initiator, and the frame is
  identified by the pair. This is a genuine limitation, not a detail.

Content scripts from different extensions must not see each other. Each
extension gets its own world name per frame, so `browser` in one extension's
content script is not the same object as another's. Scripts within one extension
share a world, as the specification requires.

## Storage

Four areas with four different backings, and the difference matters:

| Area | Backing | Persistence |
| --- | --- | --- |
| `storage.local` | Per-extension SQLite or JSON store in the profile | Yes |
| `storage.session` | In-memory, per browser session | No |
| `storage.sync` | A sync abstraction | Yes, across profiles that sync |
| `storage.managed` | Read-only administrator policy | Policy-controlled |

`storage.sync` must not be a permanent alias for `storage.local`. The
specification gives it a quota (per item, per total, and per minute of write
operations) and makes writes that exceed it fail. A fake sync that only pretends
about persistence but not about quota produces an extension that works on one
machine and breaks on a second. If no sync service is configured, the honest
behaviour is a documented failure mode, not silent aliasing.

Every area is keyed by extension id, and the store refuses to read or write
another extension's data even when asked with a different id.

## Permission checking

The check happens at the dispatch boundary, on every call, with the calling
context's identity as the key — never with anything the caller supplied about
itself.

```text
JS call
  ↓
Dispatcher: is this method exposed in this context?   → context capability table
        ↓
      is the extension enabled?
        ↓
      does it hold the required API permission?
        ↓
      for host-scoped members: does it hold the host permission?
        ↓
      validate the arguments
        ↓
      run the handler
```

Nothing is granted implicitly. Optional permissions start denied and are granted
only through `permissions.request` with a user-visible prompt, or the settings
UI. A permission that is not granted produces a rejected promise with a message
naming the permission, never a silent `undefined` — an extension that cannot
distinguish "denied" from "succeeded" is an extension that will misbehave.

`activeTab` is a grant, not a permission: it is held only for a tab the user has
interacted with, and it is dropped when navigation leaves the origin.

See [security-model.md](security-model.md) for the trust boundaries these
checks sit on.