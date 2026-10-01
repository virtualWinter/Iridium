# Iridium WebExtensions

The platform, its specification surface, and how much of it works.

Start with [architecture.md](architecture.md). The rest are reference material
and, where marked, generated.

## Documents

| Document | What it is |
| --- | --- |
| [architecture.md](architecture.md) | Layers, the API framework, the service mappings, what is generated |
| [execution-model.md](execution-model.md) | Contexts, MV2 and MV3 lifetimes, the event router, messaging, content scripts, storage |
| [security-model.md](security-model.md) | Trust boundaries, permissions, path traversal, native messaging, content-process compromise |
| [compatibility.md](compatibility.md) | The `chrome.*` facade, deliberate divergences, the real-extension test strategy |
| [webkit-integration.md](webkit-integration.md) | The engine hooks this depends on, and the three that do not exist |
| [api-coverage.md](api-coverage.md) | Generated: every namespace, what is implemented, what is missing |
| [manifest-coverage.md](manifest-coverage.md) | Generated: every `manifest.json` key, and the permission classes |

## Data

| File | What it is |
| --- | --- |
| [compatibility.yaml](compatibility.yaml) | Hand-written. The single source of truth for status |
| `data/api-inventory.json` | Generated. The API surface, from pinned upstream definitions |
| `data/manifest-inventory.json` | Generated. Manifest keys and permissions |
| [../tools/webext/README.md](../../tools/webext/README.md) | How to refresh the inventory and the coverage documents |

## The short version

The target is the complete applicable MDN WebExtensions surface, exposed as
`browser.*` with `chrome.*` as a facade. MV2 and MV3 are both first-class, with
separate execution and lifetime models.

**The runtime is WebKit's.** Upstream WebKit implements WebExtensions behind
`ENABLE(WK_WEB_EXTENSIONS)` and ships a WPE port; the installed WPE 2.52.6 was
built with it off, which is why `webkit_web_extension_new()` returns null. The
plan is to enable it and own the browser around it — see
[webkit-integration.md](webkit-integration.md) for the decision and the evidence.

What exists today is the prototype: manifest parsing, match patterns, install
and enable, content script injection, and a subset of `runtime`, `tabs`,
`windows`, `storage.local` and `i18n` through a hand-written shim that the
delegation replaces. All five are `partial`, and
[api-coverage.md](api-coverage.md) says exactly what each one is missing.

Because the JavaScript API comes from WebKit, the work here becomes *measuring*
it rather than writing it: the generated catalog and the dispatcher assert what
the specification requires of each member, and `compatibility.yaml` records what
the engine actually delivers.

Two things are known to fall short of the specification on WPE and are recorded
rather than approximated: `menus` (upstream's context-menu integration is
`#if PLATFORM(MAC)`) and `devtools.*` (needs a second build flag, and its
plumbing is Cocoa-shaped).