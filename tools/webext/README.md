# WebExtensions specification tooling

The extension API surface is derived from upstream definitions, not from
anyone's memory of the APIs. Three upstream sources are pinned, fetched,
reduced to a machine-readable inventory, and checked against a hand-written
record of what Iridium implements.

```text
mdn/browser-compat-data   ─┐
mdn/content                ─┼─► fetch.py ─► inventory.py ─► docs/extensions/data/*.json
Gecko WebExtension schemas ┘                        │
                                                    ▼
                              compatibility.yaml ──► coverage.py ─► api-coverage.md
                              (hand-written)                    └─► manifest-coverage.md
```

## Files

| File | Role |
| --- | --- |
| `pins.json` | The three upstream repositories, pinned to exact commits |
| `fetch.py` | Sparse-clones the pinned commits into `.cache/webext-upstream` |
| `inventory.py` | Reduces them to `docs/extensions/data/*.json` |
| `coverage.py` | Validates `compatibility.yaml` against the inventory, generates the coverage documents |

## Refreshing

```sh
tools/webext/fetch.py --refresh   # re-fetch the pinned commits
tools/webext/inventory.py         # regenerate the inventory
tools/webext/coverage.py          # regenerate the coverage documents
```

`fetch.py` needs network access. `inventory.py` and `coverage.py` do not, which
is why the reduced inventory is committed: a build, a documentation review and
the `extensions-coverage` test all work offline.

`coverage.py` needs PyYAML.

## To bump a pin

1. Update the `ref` in `pins.json` to a commit that exists upstream.
2. Run the three commands above.
3. Read the diff to `docs/extensions/data/*.json`. New namespaces and members
   appearing here is the point: it is how a new API upstream is noticed.
4. If a new namespace or manifest key appeared, `coverage.py` will fail until
   `compatibility.yaml` has an entry for it. That is intended.

## The rules the tooling enforces

A status in `compatibility.yaml` is a claim, and these are the checks that make
it one:

- Every namespace and manifest key in the inventory must have an entry. A new
  upstream API fails the build until someone decides its status.
- `full` requires at least one named CTest target, and that target must actually
  be registered in `CMakeLists.txt`. An API whose JS namespace exists is not an
  implemented API.
- `unsupported` requires a `why` naming the engine or platform limit.
- `partial` requires `missing`, `notes` or `why`.
- A name in `missing` must be a real member of the inventory, including nested
  ones such as `storage.local.getBytesInUse`. A typo cannot silently reduce the
  reported gap.
- `full` and `experimental` may not list `missing`.

`coverage.py --check` runs as the `extensions-coverage` CTest test, so the
committed documents cannot drift from the database.

## Reading the inventory

`docs/extensions/data/api-inventory.json` is organised per namespace, with a
`members` map. Each member records where it came from (`in_bcd`, `in_gecko`),
its kind (method, event, property, type), per-browser support, and — where the
Gecko schemas describe it — its parameters. A namespace also records the
permission it requires, the contexts it is allowed in, and its manifest-version
bounds.

That is what the C++ generator will consume, so the generated validators and
descriptors come from the same data the documentation is generated from.