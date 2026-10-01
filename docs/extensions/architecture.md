# Iridium WebExtensions architecture

Iridium targets the MDN WebExtensions specification, exposed as `browser.*` with
`chrome.*` as a facade. Nothing about it is modelled on Chromium: there is no
CEF, no Qt WebEngine, no Electron, and no Gecko.

The extension *runtime* — manifests, match patterns, content-script worlds,
background contexts, storage, alarms, declarative rules — is WebKit's, compiled
in behind `ENABLE(WK_WEB_EXTENSIONS)`. What Iridium owns is everything around
it: which extensions exist, what they may do, where their data lives, and how
their surfaces appear in Iridium's UI. See
[webkit-integration.md](webkit-integration.md) for why, and for what the
delegation costs.

The specification is MDN's WebExtensions documentation, treated as the public
contract:

<https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions>

## The shape of the thing

```text
                        Extension
                            │
                 ┌──────────┴──────────┐
                 │                     │
             browser.*              chrome.*
                 │                     │
                 │              compatibility
                 │                  adapter
                 └──────────┬──────────┘
                            ▼
                 Iridium WebExtensions
                            │
          ┌─────────────────┼──────────────────┐
          ▼                 ▼                  ▼
     Browser Core    Extension Host         WebKit
          │                                    │
          ▼                                    ▼
   tabs/windows/etc                content/scripts/pages
```

Both namespaces are the same object graph. `chrome.*` is a facade over
`browser.*`, not a second implementation: one registry, one dispatcher, one
event router, one permission check. The facade only changes how a call is
delivered (see [compatibility.md](compatibility.md)), so there is no behaviour
that works under `chrome.*` and not under `browser.*`.

Extensions talk to **browser abstractions**, never to the engine:

```text
browser.tabs.query()
        ↓
ExtensionTabsApi
        ↓
TabManager
```

Not:

```text
browser.tabs.query()
        ↓
WebKitWebView poking
```

The rule is enforced by construction: `src/extensions/api/**` links against
`src/browser` interfaces and does not link WebKit. WebKit types appear only in
`src/engine/**` and in the small set of adapters that implement a browser-core
interface on top of the engine.

## Layers, and what may depend on what

```text
src/
├── app/                  process lifetime, CLI
├── ui/                   Qt chrome: windows, tabs, sidebar, settings
├── browser/              browser core: the services extensions talk to
├── engine/               WebKit adapter, frame presentation, script bridge
├── extensions/           the extension platform
└── platform/             Linux platform services (portal, notifications, ...)
```

`app`, `ui`, `browser`, `engine` and `extensions` exist today;
`platform/` and the subdirectory layout inside `extensions/` are the target and
are tracked in `TODO.md`.

Dependency direction is `ui → browser → extensions → engine`, never upward.
Two rules make that hold:

- A browser service an extension needs does not live in `src/extensions`. If
  `tabs.onUpdated` needs navigation phases, `TabManager` grows the phases;
  `src/extensions` subscribes. An extension-only store of browser state would
  be a second source of truth for state the browser already owns.
- No WebKit type crosses out of `src/engine`. Extension code sees
  `engine::WebView` and JSON, which is also what makes the extension layer
  testable without a display or an engine.

### Browser services the platform maps onto

| API | Browser-core service | Exists today |
| --- | --- | --- |
| `tabs`, `tabGroups` | `TabManager` | tabs yes, groups no |
| `windows` | `WindowManager` (via `MainWindow`) | partial |
| `downloads` | `DownloadManager` | yes |
| `history` | `HistoryStore` | yes |
| `bookmarks` | `BookmarkManager` | no |
| `sessions` | `SessionManager` | no |
| `cookies` | `WebKitCookieManager` (per network session) | reachable |
| `browsingData` | `WebKitWebsiteDataManager` + own stores | reachable |
| `storage` | `ExtensionStorage` | `local` only |
| `webNavigation` | `NavigationService` (from `TabManager`) | no |
| `webRequest`, `declarativeNetRequest` | WebKit's network process, behind the extension flag | supply tab and request state |
| `notifications` | platform notification service | no |
| `theme` | theme layer over the Qt palette | no |
| `menus`, `commands`, `omnibox`, `action`, `pageAction`, `sidebarAction` | UI services the Qt frontend renders | no |

## One API framework, many namespaces

The namespaces are not independent subsystems. They share a framework, and each
namespace contributes only its handlers.

Delegating the JavaScript API does not make that framework unnecessary — it
changes what it is *for*. It is now the conformance harness: the generated
catalog states what the specification requires of each member, and the registry
and dispatcher assert that requirement independently of WebKit. That is how
"WebKit covers 41 of 69 namespaces" becomes a measured claim rather than an
impression, and how a WebKit upgrade that quietly drops something gets noticed.
It is not the shipping path for `browser.*`; WebKit provides that.

The hand-written `src/extensions/ExtensionApi.cpp` shim is what remains of the
old shipping path, and it has to go once WebKit's host is enabled: two
independent definitions of `browser.tabs` in one frame would conflict, and the
page-visible shim would shadow WebKit's isolated-world API.

```text
Extension API
│
├── API registry            which namespaces and methods exist, and for which
│                           manifest versions, contexts and permissions
├── Method dispatcher       validate → check permission → run → serialize
├── Event router            subscribe, filter, deliver, wake a worker
├── Argument validation     generated from the upstream schemas
├── Serialization           generated types in and out
├── Promise handling        one promise per call, one rejection path
├── Callback compatibility  chrome.* delivery mode only
└── Error handling          one error shape across every namespace
```

A namespace handler looks like this, and nothing else is needed to add one:

```text
class TabsApi {
    static void registerWith(ApiRegistry& registry, BrowserServices& services);
    // registers tabs.query, tabs.create, tabs.onUpdated with their permission,
    // context and argument requirements, and maps them onto TabManager
};
```

`src/extensions/generated/` (not yet written) holds what the schemas say: C++
types, argument validators, method descriptors, event descriptors, and
compatibility metadata. `src/extensions/api/` holds behaviour. Generated code
never calls into handwritten behaviour and handwritten behaviour never re-declares
a signature; the registry is the only place the two meet. That boundary is what
lets the schemas be replaced (Firefox's today) without touching a single handler.

### Generated from upstream definitions

Handwriting several hundred type definitions is how they drift, so they are
generated. The generator reads pinned upstream data — see
`tools/webext/pins.json`:

- Gecko schema files (`toolkit/components/extensions/schemas`,
  `browser/components/extensions/schemas`) are authoritative for a function's
  parameters, its required permission, its manifest-version bounds and its
  allowed contexts.
- MDN's `browser-compat-data` supplies per-browser support, notes and the
  canonical namespace/member spelling.
- MDN `content` supplies the list of namespaces and manifest keys, which is what
  makes "the list is exhaustive" checkable instead of assumed.

`tools/webext/inventory.py` reduces those to
`docs/extensions/data/*.json`; the generator turns that into C++. Both are
committed so a build needs no network.

The consequence worth stating plainly: **the API surface Iridium targets is
enumerated in data, not in someone's memory.** Today that is 69 namespaces,
796 members and 43 manifest keys. A namespace that exists upstream and has no
status in `compatibility.yaml` fails the build.

## Compatibility tracking

`docs/extensions/compatibility.yaml` is the single source of truth for what is
implemented. `tools/webext/coverage.py` validates it against the inventory and
generates the human-readable tables:

- `docs/extensions/api-coverage.md`
- `docs/extensions/manifest-coverage.md`

There is no second list. The states are `full`, `partial`, `experimental`,
`planned`, `unsupported`, and the rules are enforced rather than hoped for:

- `full` requires at least one named CTest target. An API whose JS namespace
  exists is not an implemented API.
- `partial` requires `missing`, `notes` or `why`.
- `unsupported` requires `why`.
- A name in `missing` that the specification does not have is an error, so a
  typo cannot quietly reduce the reported gap.

## Execution contexts

Contexts are explicit, and they are not interchangeable. See
[execution-model.md](execution-model.md) for the lifecycle; the security
consequences are in [security-model.md](security-model.md).

```text
ExtensionContext
├── ContentScriptContext      per extension, per frame, isolated world
├── ExtensionPageContext      popup, options, sidebar, extension pages
├── BackgroundPageContext     MV2 background page, persistent or event page
├── ServiceWorkerContext      MV3 background service worker
└── DevToolsContext           devtools_page and panels
```

An extension's set of contexts is derived from its manifest, not assumed. A
`content_scripts` block produces `ContentScriptContext`s; a `background.scripts`
entry produces a `BackgroundPageContext`; a `background.service_worker` entry
produces a `ServiceWorkerContext`; a `devtools_page` produces a
`DevToolsContext`.

## Where the engine boundary really is

The engine is used for what it is good at: running the DOM, running JavaScript
in worlds, fetching resources, and painting. The browser owns everything else.
Concretely, Iridium uses:

- `WebKitUserScript` for world name, injection time and frame scope.
- `WebKitScriptWorld` for content-script isolation.
- Per-world script message handlers with a reply for the API channel.
- A registered URI scheme for extension resources.
- `WebKitNetworkSession` per storage partition for containers.
- `WebKitCookieManager`, `WebKitWebsiteDataManager`, `WebKitFindController`,
  `WebKitNetworkProxySettings` where they exist.

The one thing it cannot do for us today is extensions at all: the installed
build has them compiled out. Everything else is in the tree behind build flags.
[webkit-integration.md](webkit-integration.md) records which flags, what each
unlocks, and where the upstream implementation stops short of the specification.

## Who does what

| Specification requirement | Where it comes from | What Iridium still does |
| --- | --- | --- |
| Manifest parsing and localization | WebKit (`WebKitWebExtension`) | Decides what is installed and enabled |
| Match patterns | WebKit (`WebKitWebExtensionMatchPattern`) | Grants permissions, using the same grammar |
| Isolated content-script worlds | WebKit, one named world per extension | Supplies the tab and frame identity |
| Extension resources and pages | WebKit (`WebExtensionURLSchemeHandler`) | Opens popups, options and sidebars in Iridium's UI |
| Background context and MV3 workers | WebKit (`WebExtensionContext`) | Nothing but lifetime decisions |
| `storage` | WebKit (`WebExtensionStorageSQLiteStore`) | Chooses the directory; removes it with the profile |
| `alarms`, ports, messaging | WebKit | Supplies tab and window state |
| Blocking `webRequest` | WebKit controller hooks, not yet measured | Decides what a blocking extension may see |
| `declarativeNetRequest` | WebKit `ContentRuleList` engine | Chooses which static rulesets are enabled |
| `menus` | No non-Cocoa integration point | Renders what the platform allows, or reports the gap |
| `devtools.*` | Needs `ENABLE(INSPECTOR_EXTENSIONS)` as well | Stretch goal |
| Containers | `websiteDataStore(sessionId)` | Owns the container-to-tab mapping |

## Reading order

1. [execution-model.md](execution-model.md) — contexts, lifetimes, the event
   router, messaging and ports.
2. [security-model.md](security-model.md) — what is trusted, what is not, and
   where each check happens.
3. [api-coverage.md](api-coverage.md) — what exists, and what actually works.
4. [manifest-coverage.md](manifest-coverage.md) — the same for `manifest.json`.
5. [compatibility.md](compatibility.md) — the `chrome.*` facade and the
   deliberate divergences.
6. [webkit-integration.md](webkit-integration.md) — the engine hooks this
   depends on, and the ones that do not exist.