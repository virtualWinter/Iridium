# Compatibility

## The two namespaces

Iridium implements one API surface and exposes it under two names.

```text
browser.*   promises, the specification's own namespace
chrome.*    callbacks (and promises where the browser provides them)
```

`chrome.*` is a delivery adapter, not a second implementation. One registry, one
dispatcher, one permission check, one event router. Anything that works under
`chrome.*` works under `browser.*`, and the only observable difference is how
the result is delivered.

This matters more than it used to. MDN now records that Chrome 148+ exposes
`browser.*` itself, with complete promise coverage by 152, and that the
`browser` namespace is unavailable only to DevTools pages in Chrome 148. So the
long-term direction is one namespace everywhere, and a `chrome.*` adapter that
is a thin veneer is the correct shape to converge on.

## Delivery rules

Under `browser.*`, an asynchronous method returns a promise and rejects with an
`Error`. Under `chrome.*`, the same method additionally accepts a trailing
callback, and reports failure in the documented Chrome way.

```js
// promise form
const tabs = await browser.tabs.query({ active: true });

// callback form, same implementation underneath
chrome.tabs.query({ active: true }, (tabs) => { ... });
```

The rules the adapter follows:

- A method that is asynchronous gets a promise. If a callback was passed, the
  promise's settlement drives the callback instead, and the promise itself is
  not returned to the caller in the way Chrome does it. Exactly one mechanism
  delivers the result; there is no path where both fire.
- Rejection becomes `chrome.runtime.lastError` for the callback, with
  `console.error` unless the callback handles it, which is Chrome's documented
  behaviour and an extension that ignores it should still see the error.
- A synchronous method stays synchronous. `runtime.getURL` does not become a
  promise because the namespace did.
- `chrome.runtime.lastError` is cleared when the callback returns, and reading
  it inside the callback is the only supported time.
- `runtime.onMessage` under `chrome.*` uses the callback/`return true` shape:
  a listener returns `true` to keep the channel open for an asynchronous reply.
  Under `browser.*` it returns a Promise. The router accepts both.
- `browser.*` never sets `lastError`; Chrome-only semantics stay in the
  `chrome.*` facade.

## Deliberate divergences from Firefox

These are decisions, not gaps. Each is recorded here because a divergence should
be a choice someone made.

| Area | Firefox | Iridium | Why |
| --- | --- | --- | --- |
| Extension scheme | `moz-extension://<uuid>/` | Same | The uuid is generated per extension and is not derived from the extension id |
| Extension id | `addon:id` for a signed add-on, or a UUID | Directory name, or `browser_specific_settings.gecko.id` | Iridium installs unpacked directories and verifies no signature |
| Signature verification | AMO-signed, or temporary with an id | None | No add-on signing infrastructure; installing is a local act |
| Private browsing | `incognito: spanning` and `not_allowed` | Not implemented | Iridium has no private mode yet |
| `contentScriptGlobalScope` (`cloneInto`, `exportFunction`) | Provided | Planned, semantics to be decided | WebKit has no equivalent primitive; a plain object copy would differ where it matters |
| `sidebarAction` | Firefox-specific | Planned, hosted inside Iridium's sidebar layout | Iridium's UI is its own design; an extension sidebar does not replace it |
| `theme` | Mozilla theme format | Planned | Iridium's palette is the Qt system palette |
| `devtools.*` | Firefox DevTools | Unsupported | No client to the WebKit inspector |
| `browser_specific_settings` | Read and enforced | `gecko.id` only | Nothing else in the key has meaning here |
| `protocol_handlers` | Supported | Planned | Needs protocol registration in browser core |
| `externally_connectable` | Not supported | Unsupported | Same, plus the security decision is unmade |
| `webRequest` blocking | Supported (MV2) | Unsupported | No per-request hook in the public engine API |

## Manifest differences

- **`manifest_version` 2 and 3** are both accepted. MV2 is first-class: no
  feature is removed because Chromium deprecated it.
- **`web_accessible_resources`** has both shapes: the MV2 string array, and the
  MV3 array of objects with `resources`, `matches`, `extension_ids` and
  `use_dynamic_url`.
- **`content_security_policy`** has both shapes: the MV2 string, and the MV3
  object form with separate `extension_pages` and `sandbox` values.
- **`background`** has MV2 `scripts`/`page`/`persistent` and MV3
  `service_worker`, kept as distinct model objects rather than one struct with
  optional fields, because the lifetimes differ.
- **`action` vs `browser_action`/`page_action`** are modelled separately, and the
  manifest version decides which are legal. Chrome has begun accepting both keys
  in MV3; Iridium follows the specification rather than Chrome's drift, and
  reports the disagreement rather than hiding it.

## Real-world extension compatibility

The goal is that extensions written for Firefox or Chrome run unmodified. The
test strategy has two layers, and the first is the one that counts:

1. **Small extensions written for this repository**, under `tests/extensions/`,
   one per behaviour that is easy to get wrong. They assert specification
   behaviour, not Iridium's preferences: promise semantics, error identity,
   `chrome.*` delivery, isolated worlds, message and port semantics, storage
   areas, MV2 and MV3 lifecycles.
2. **Real extensions**, unmodified, as compatibility evidence. A representative
   set is maintained in `tests/extensions/compatibility/`:

   | Kind | What it exercises |
   | --- | --- |
   | content-script-only | Injection, DOM access, isolation |
   | MV2 background | Background page, persistent and event pages |
   | MV3 service worker | Worker lifecycle, wake-up, event persistence |
   | content blocker | `declarativeNetRequest` or `webRequest` |
   | userscript manager | `userScripts`, dynamic registration |
   | password-manager-style | Storage, `storage.managed`, host permissions |
   | download manager | `downloads`, `runtime.onMessage` |
   | tab/session manager | `tabs`, `sessions`, `tabGroups` |
   | theme | `theme`, `manifest.theme` |
   | sidebar | `sidebarAction`, `devtools_page` |
   | native messaging | `runtime.connectNative` and `sendNativeMessage` |

Extensions in the second set are never modified to make them pass. A failure is
a compatibility finding, recorded in `compatibility.yaml` against the API
responsible, with the failing extension named.

Some real extensions will not run, and that is expected. What is not acceptable
is failing silently: an extension that loads and then errors in the background
has told the user nothing.

## How a status changes

A status in `compatibility.yaml` changes when evidence changes, not when
intention changes:

```text
planned  ──▶  partial  ──▶  full
              needs: handlers registered, permission checks in place,
                     validation from the generated schema, and a test

full     ──▶  partial  ──▶  experimental
              a documented deviation appears, or a test regresses

any      ──▶  unsupported
              only with a `why` naming the engine or platform limit
```

`coverage.py` enforces the rules that matter: `full` needs a named test,
`unsupported` needs a reason, a `missing` entry must be a real member, and every
namespace and manifest key upstream must have an entry. So a status cannot drift
away from the code without the build noticing.