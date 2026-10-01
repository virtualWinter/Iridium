# Extension security model

Extensions run other people's code. Everything below follows from taking that
seriously rather than treating it as a convenience.

## Threat model

An extension is untrusted code with a user-visible surface and a documented API.
The things that must not happen:

1. An extension reads a page it has no permission for.
2. An extension reads another extension's data, files or resources.
3. A page reads an extension's internals, or spoofs one.
4. An extension reaches outside its own directory on disk.
5. An extension executes arbitrary commands through the browser.
6. A compromised content process gains authority it did not have.
7. An unsupported API silently behaves as though it were supported.

## Trust boundaries

```text
                        ┌──────────────────────────────────────┐
                        │  Browser process (trusted)           │
                        │  registry, dispatcher, permissions,  │
                        │  storage, message bus, event router │
                        └───────────────┬──────────────────────┘
                                        │  validated, schema-checked IPC
        ┌───────────────────────────────┼───────────────────────────────┐
        │                               │                               │
┌───────▼────────┐            ┌─────────▼─────────┐          ┌────────▼────────┐
│ Extension      │            │ Extension page /  │          │ Content script  │
│ world          │            │ background /      │          │ world           │
│ (isolated)     │            │ worker            │          │ (isolated)      │
└───────┬────────┘            └───────────────────┘          └────────┬────────┘
        │                            │                             │
        │                     ┌──────▼──────────────────────────────▼──────┐
        └────────────────────►│  Page world (untrusted, shares the DOM)    │
                             └────────────────────────────────────────────┘
```

Four boundaries, each with a specific control:

| Boundary | Control |
| --- | --- |
| Extension ↔ browser process | Schema-validated IPC; the browser never trusts a claimed identity |
| Content script ↔ page | Separate script worlds; shared DOM, separate globals |
| Extension ↔ extension | Per-extension origin, storage namespace and file root |
| Browser ↔ native process | Validated host manifests; fixed argv; length-prefixed framing |

## 1. Content scripts are isolated from the page, and from each other

A content script must not be able to see page JavaScript, and the page must not
be able to see `browser`. This is implemented with WebKit script worlds: one
world per extension per frame. The DOM is shared, deliberately, because the
specification's content-script model is "same page, different JavaScript
environment".

The current implementation is a violation of this, not an approximation of it:
scripts are injected into the page's own world, so a page script can read and
overwrite `window.browser`, and a content script sees page globals. This is the
single most important item in the migration plan, and it is recorded as such in
[api-coverage.md](api-coverage.md) rather than described as a limitation of the
engine.

Two further rules once worlds are in place:

- A world name is derived from the extension id and never from page input.
- The message channel is registered per world, so one extension's channel cannot
  be reached from another extension's world.

`world: "MAIN"` is an explicit, declared escape hatch in MV3. It is honoured
only when the manifest asks for it, and an extension using it has declared that
it does not want isolation.

## 2. Permissions are checked at the boundary, from trusted identity

The extension id used for a permission check must come from the transport, not
from the payload. In a content script, the id is established when the script is
injected and attached to the channel, so page script cannot choose it.

At the dispatcher:

1. Is the method exposed in this context at all?
2. Is the extension enabled?
3. Does it hold the required API permission?
4. For host-scoped members, does it hold the host permission for this URL?
5. Is the caller's manifest version allowed to have this method?

A failure at any step is a rejected promise naming the reason. Not `undefined`,
not a silent no-op: an extension that cannot tell a denial from a success will
behave incorrectly, and the specification's own error messages are part of the
contract.

Optional permissions are granted by the user and revocable at any time. A grant
is stored per profile per extension, and revocation takes effect immediately —
including for a running MV3 worker, which is told to drop the permission rather
than continuing to use it until it next asks.

`activeTab` is granted by user interaction with a specific tab and expires. It is
not stored, because it is not the user's to keep.

## 3. Extensions cannot reach each other

- **Storage** is keyed by extension id, and the store refuses any request whose
  id does not match the caller's.
- **Origins** are per extension. An extension page's origin is
  `moz-extension://<uuid>/` with a uuid that belongs to that extension and no
  other, so `localStorage`, `indexedDB` and `caches` are naturally partitioned.
- **Resources** are served by a handler that resolves a path against the
  requesting extension's own root. See below.
- **`web_accessible_resources`** is an explicit list. Without it, an extension's
  resources are not fetchable by a page, and MV3 narrows this further with
  `matches` and `extension_ids`.

## 4. Resource paths cannot escape the extension directory

An extension URL handler receives an arbitrary path from the network stack. It
must be treated as hostile input.

- Resolve the path against the extension root and verify the result is still
  inside it, after normalisation. `..`, percent-encoding, symlinks and absolute
  paths are all rejected, not sanitised.
- Reject NUL bytes and anything that is not a regular file.
- Serve only files that exist in the extension package. There is no fallback to
  the filesystem.
- A `moz-extension://` request whose uuid is unknown, or whose path is outside
  that extension, fails with not-found. It does not fall through to another
  extension or to the filesystem.
- `web_accessible_resources` gating is applied before the file is opened, not
  after it has been read into memory.

The existing registry test covers a `../` traversal attempt; it is the shape of
check the resource handler needs, and the handler must have its own.

## 5. Native messaging cannot become arbitrary execution

`runtime.connectNative` and `runtime.sendNativeMessage` start a process. The
rules:

- **The host manifest decides the command.** The extension names an application;
  it does not supply an argument vector. Iridium reads the allowed host from the
  extension's `nativeMessaging` permission plus the host name in the call, looks
  up the registered manifests, and runs the first allowed entry's command with
  no extension-controlled arguments.
- **Host manifests are validated** against the standard locations
  (`~/.config/mozilla/native-messaging-hosts`,
  `/etc/opt/chrome/native-messaging-hosts`, and the Chromium equivalent), for
  ownership and permissions, before anything is executed. A manifest that is
  world-writable, or owned by another user, is refused.
- **The channel is bound to the calling extension.** A port created by
  extension A cannot be used by extension B, and the browser process tracks
  which extension owns which native process.
- **Framing is the standard one**: 4-byte native-endian length prefix followed by
  UTF-8 JSON. A malformed frame, an oversized frame, or JSON that does not match
  the expected shape closes the channel.
- **No shell.** The command is executed directly, never through a shell, so an
  extension cannot smuggle shell metacharacters into an argument.
- Output is bounded. An extension that reads without limit gets a closed channel
  rather than unbounded memory growth in the browser process.

## 6. Argument validation happens before anything else

Every call's arguments are validated against the generated schema before a
handler runs. This is not only a correctness concern:

- An integer `tabId` that is not an integer must not become a truncated or
  wrapped value.
- A URL argument must be parsed and rejected if it is not a URL, before it is
  matched against permissions.
- A `matchPatterns` array must be parsed by the one match-pattern
  implementation, so a pattern that would match everything cannot be smuggled
  past a check that expected a narrow one.
- A callback function must be one of the callbacks the API actually takes, not
  an arbitrary object.
- The `chrome.*` callback form must not be able to smuggle an argument that the
  promise form would reject.

Validation failures are errors, with a message that names the argument. This is
also the difference between a sandbox and a suggestion.

## 7. One match-pattern implementation

There is exactly one URL-matching implementation, used by content scripts, host
permissions, `webRequest` filters, `scripting`, `userScripts`, and by the
permission check itself. Two implementations would mean a pattern that matches
in one place and not the other, which is a security bug, not an inconsistency.

The implementation must handle the specification's grammar exactly, including
the parts that are easy to get wrong:

- `*` in the scheme means http and https only (plus ws/wss where supported), not
  every scheme.
- `<all_urls>` covers the web schemes and `file:`, and must not cover `data:`.
- `*.example.com` matches `example.com` as well as its subdomains.
- The path matches path plus query string; the fragment is not part of the
  match, and a pattern containing `#` matches nothing.
- Ports are part of the host when present.

## 8. Compromise of a content process

A content process is where untrusted page code runs. If it is compromised, the
attacker gets the page's world and its DOM, and must get nothing else.

- Content script worlds live in the same process as the page. This is an
  accepted, deliberate trade: the specification's content-script model requires
  shared DOM, and process-per-extension is a different architecture. What it
  means is that a page cannot *reach* an extension's globals through the world
  boundary, but a sufficiently deep engine compromise could. This is recorded
  rather than claimed away.
- The browser process never trusts anything from the content process: no
  permission decisions, no storage access, no native messaging.
- API calls arriving from a content process carry the identity the browser
  assigned, never an identity the page supplied.
- A crashed content process invalidates the extension contexts in it. Registered
  content scripts are re-injected into the reloaded page. Ports into the dead
  process are disconnected with an error rather than left hanging.

## 9. No silent faking

An unsupported API rejects with a message that says what is missing and why.
It does not resolve to `undefined`, and it does not return an empty array that
looks like a successful query.

This matters beyond politeness. An extension that receives `[]` from
`tabs.query` concludes the user has no tabs; one that receives `undefined` from
a rejected `sendMessage` may hang forever. A browser that quietly returns
plausible wrong data is worse than one that says no, because the extension will
be built and shipped against the fiction.

The same rule applies to the documents: an API is `full` in
[api-coverage.md](api-coverage.md) only when its documented behaviour, events,
permissions, errors and edge cases are implemented and covered by a named test.
A JS namespace existing is not implementation.

## 10. Content security policy

Once extension pages have an origin, the manifest's `content_security_policy`
becomes enforceable:

- MV3 restricts the policy further than MV2 by default (no remote script, `'self'`
  only for extension scripts, no `eval`), and Iridium's floor must be at least as
  strict.
- Content scripts are exempt, because they run in the page's context by design
  and are constrained by the page's own CSP.
- Inline script in an extension page is refused unless the manifest's policy
  allows it. This needs an injection point that respects CSP, which is a
  deliberate reason not to evaluate extension code by string concatenation.

## Review checklist for a new API

Before an API is marked anything above `planned`:

- [ ] Every call is permission-checked at the dispatcher, with a trusted id.
- [ ] Every argument is validated against the generated schema first.
- [ ] Every URL goes through the one match-pattern implementation.
- [ ] The API is absent from contexts that should not have it.
- [ ] Storage access is keyed by extension id and refuses other ids.
- [ ] Nothing in the path reads outside the extension directory.
- [ ] Failure is a rejection with a reason, never a plausible empty value.
- [ ] `chrome.*` behaves identically, differing only in delivery.
- [ ] A test exists and is named in `compatibility.yaml`.