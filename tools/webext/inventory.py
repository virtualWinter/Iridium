#!/usr/bin/env python3
"""Reduce the pinned upstream definitions to a machine-readable inventory.

Two files are produced, both committed, both consumed by docs and by the code
generator:

  docs/extensions/data/api-inventory.json       namespaces and their members
  docs/extensions/data/manifest-inventory.json  manifest.json keys

Nothing here decides what Iridium implements. It records what the specification
says exists, and which browser supports it, so the compatibility database can be
compared against the specification instead of against a list someone typed.

Inputs (see fetch.py):
  <cache>/bcd/api/*.json          MDN compatibility data per namespace
  <cache>/bcd/manifest/*.json     MDN compatibility data per manifest key
  <cache>/mdn_content/.../api/    MDN's own namespace list
  <cache>/gecko/*.json            Gecko schemas: parameters, permissions, MV

Usage:
    tools/webext/inventory.py [--cache DIR] [--out DIR]
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any, Iterator

ROOT = Path(__file__).resolve().parents[2]

# Gecko schema files allow // comments before the JSON body.
_COMMENT = re.compile(r"(?m)^\s*//.*$")


def log(message: str) -> None:
    print(f"[webext] {message}", flush=True)


def load_json(path: Path) -> Any:
    text = _COMMENT.sub("", path.read_text(encoding="utf-8"))
    try:
        return json.loads(text)
    except json.JSONDecodeError as error:
        raise SystemExit(f"{path}: {error}") from error


# ---------------------------------------------------------------- MDN content

def mdn_namespaces(content_root: Path) -> dict[str, dict[str, Any]]:
    """MDN's authoritative list of API namespaces.

    A namespace is a directory under MDN's API section. Reading the list off
    the filesystem is what makes "do not assume this list is exhaustive"
    checkable: a namespace MDN documents but nobody implemented shows up here
    as unimplemented rather than quietly disappearing.
    """
    api_dir = content_root / "files/en-us/mozilla/add-ons/webextensions/api"
    if not api_dir.is_dir():
        raise SystemExit(f"missing MDN content at {api_dir}")
    namespaces: dict[str, dict[str, Any]] = {}
    for child in sorted(api_dir.iterdir()):
        if not child.is_dir() or not (child / "index.md").is_file():
            continue
        # MDN's directories are lowercased ("browseraction", "declarativenetrequest"),
        # so the directory name cannot be the namespace name. BCD and the Gecko
        # schemas both spell it exactly; MDN is matched against them, and only a
        # namespace that appears in neither keeps a derived name, flagged so a
        # reader can see it was reconstructed rather than read.
        namespaces[child.name.replace("_", "")] = {
            "dir": child.name,
            "mdn_url": f"https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/{child.name}",
            "mdn_dir": child.name,
        }
    return namespaces


def mdn_manifest_keys(content_root: Path) -> dict[str, dict[str, Any]]:
    """Manifest keys as listed on MDN's manifest.json landing page.

    The page carries its list as markdown links, which is a better source than
    the set of directories: it is the curated list, including keys that only
    exist on other browsers.
    """
    index = content_root / "files/en-us/mozilla/add-ons/webextensions/manifest.json/index.md"
    if not index.is_file():
        raise SystemExit(f"missing MDN manifest index at {index}")
    text = index.read_text(encoding="utf-8")
    keys: dict[str, dict[str, Any]] = {}
    for match in re.finditer(r"^- \[([a-z0-9_]+)\]\(([^)]+)\)(.*)$", text, re.M):
        name, url, rest = match.group(1), match.group(2), match.group(3)
        note = ""
        note_match = re.search(r"\((Manifest V\d[^)]*|[^)]*Firefox[^)]*|[^)]*only[^)]*)\)\s*$", rest)
        if note_match:
            note = note_match.group(1)
        keys[name] = {
            "mdn_url": "https://developer.mozilla.org/en-US/docs/" + url.lstrip("/").replace("en-US/docs/", ""),
            "note": note,
        }
    return keys


# ------------------------------------------------------------------ MDN BCD

def bcd_support(block: dict[str, Any], browser: str) -> dict[str, Any] | None:
    """Collapse a BCD support block to one browser's answer.

    BCD models support as an array of statements; the last one without
    version_removed is the current state. "mirror" means "same as the browser
    this entry mirrors", which for our purposes is 'ask that browser'.
    """
    support = block.get("support")
    if not isinstance(support, dict):
        return None
    value = support.get(browser)
    if value is None:
        return None
    if value == "mirror":
        return {"mirrors": True}
    statements = value if isinstance(value, list) else [value]
    if not statements:
        return None
    current = None
    for statement in statements:
        if not isinstance(statement, dict):
            continue
        if "version_removed" in statement:
            current = None
            continue
        current = statement
    if current is None:
        return {"supported": False}
    # version_added can be the boolean false, which BCD uses for "not supported"
    # rather than "supported in version false".
    if current.get("version_added") is False and "partial_implementation" not in current:
        return {"supported": False}
    notes = current.get("notes")
    if isinstance(notes, str):
        notes = [notes]
    return {
        "supported": True,
        "version_added": current.get("version_added", True),
        "partial": bool(current.get("partial_implementation")),
        "notes": notes or [],
        "flags": current.get("flags") or [],
        "impl_url": current.get("impl_url"),
    }


def bcd_members(root: Path) -> dict[str, dict[str, Any]]:
    """Per-namespace members with their per-browser support."""
    result: dict[str, dict[str, Any]] = {}
    for path in sorted((root / "bcd/webextensions/api").glob("*.json")):
        data = load_json(path)
        api = data["webextensions"]["api"]
        for namespace, body in api.items():
            entry = result.setdefault(namespace, {"namespace": namespace, "members": {}})
            namespace_compat = body.get("__compat") if isinstance(body, dict) else None
            if isinstance(namespace_compat, dict):
                entry["mdn_url"] = namespace_compat.get("mdn_url")
                entry["support"] = {
                    browser: bcd_support(namespace_compat, browser)
                    for browser in ("firefox", "chrome", "safari")
                }
            if not isinstance(body, dict):
                continue
            for name, member in body.items():
                if name == "__compat" or not isinstance(member, dict):
                    continue
                compat = member.get("__compat")
                if not isinstance(compat, dict):
                    # A nested type (dictionary, enum, ...) rather than a
                    # callable. Still part of the surface, so keep the name.
                    entry["members"].setdefault(name, {"kind": "type"})
                    continue
                sub_members = bcd_sub_members(member)
                entry["members"][name] = {
                    "kind": member_kind(compat, name),
                    "mdn_url": compat.get("mdn_url"),
                    "support": {
                        browser: bcd_support(compat, browser)
                        for browser in ("firefox", "chrome", "safari")
                    },
                }
                if sub_members:
                    # storage.local.get, tabs.Tab.url and friends. These are
                    # real members of the surface and have to appear somewhere,
                    # or storage.local.getBytesInUse would look like it does not
                    # exist.
                    entry["members"][name]["members"] = sub_members
    return result


def bcd_sub_members(member: dict[str, Any]) -> dict[str, dict[str, Any]]:
    """Members of a member, one level deep.

    BCD nests wherever the JavaScript does: storage.local is a property whose
    value is a StorageArea with methods, and a documented type such as
    tabs.Tab carries its fields the same way. One level is enough for the
    WebExtensions surface and keeps the inventory readable.
    """
    sub: dict[str, dict[str, Any]] = {}
    for name, value in member.items():
        if name == "__compat" or not isinstance(value, dict):
            continue
        compat = value.get("__compat")
        if not isinstance(compat, dict):
            continue
        sub[name] = {
            "kind": member_kind(compat, name),
            "mdn_url": compat.get("mdn_url"),
            "support": {
                browser: bcd_support(compat, browser)
                for browser in ("firefox", "chrome", "safari")
            },
        }
    return sub


def member_kind(compat: dict[str, Any], name: str) -> str:
    """Classify a member as method, event, property or type.

    BCD does not always record a type, and it is wrong to guess from the URL
    alone: MDN documents types (MessageSender, OnInstalledReason) on the same
    URL shape as methods. The WebExtensions naming convention is reliable,
    though: types and constants are PascalCase, callable members and events are
    camelCase. That is checked first, then BCD's own declaration, then the URL.
    """
    declared = compat.get("type")
    if isinstance(declared, str):
        if declared in ("function", "method"):
            return "method"
        if declared == "event":
            return "event"
        if declared in ("property", "attribute"):
            return "property"
        if declared in ("dictionary", "array", "enum", "type_alias", "callback", "object"):
            return "type"
    if _looks_constant(name):
        return "property"
    if name[:1].isupper():
        # PascalCase is a type in this API (Tab, MessageSender, UrlFilter).
        return "type"
    if "/Events/" in (compat.get("mdn_url") or ""):
        return "event"
    # Everything else that is camelCase is callable: a method, or an event whose
    # URL BCD does not record. Guessing "method" is the safe error here, because
    # a misclassified method still shows up in the coverage table while a
    # misclassified one disappears from it.
    return "method"


def _looks_constant(name: str) -> bool:
    """SCREAMING_CASE names are constants, not types."""
    stripped = name.replace("_", "")
    return stripped.isupper() and any(c.isalpha() for c in stripped)


def bcd_manifest(root: Path) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    for path in sorted((root / "bcd/webextensions/manifest").glob("*.json")):
        data = load_json(path)
        manifest = data["webextensions"]["manifest"]
        for key, body in manifest.items():
            entry = result.setdefault(key, {"key": key, "properties": {}})
            if not isinstance(body, dict):
                continue
            for name, value in body.items():
                compat = value.get("__compat") if isinstance(value, dict) else None
                if not isinstance(compat, dict):
                    continue
                entry["properties"][name] = {
                    "mdn_url": compat.get("mdn_url"),
                    "support": {
                        browser: bcd_support(compat, browser)
                        for browser in ("firefox", "chrome", "safari")
                    },
                }
            top = body.get("__compat")
            if isinstance(top, dict):
                entry["mdn_url"] = top.get("mdn_url")
                entry["support"] = {
                    browser: bcd_support(top, browser)
                    for browser in ("firefox", "chrome", "safari")
                }
    return result


# ---------------------------------------------------------------- Gecko JSON

def geeko_namespaces(root: Path) -> dict[str, dict[str, Any]]:
    """Namespaces, members, permissions and MV bounds from the Gecko schemas.

    This is the layer that BCD cannot give us: for each function we learn its
    parameters, and for each namespace the permission it needs and the manifest
    versions it is valid in. That is what the generated C++ validators and the
    permission checks are built from.
    """
    schemas = root / "gecko"
    if not schemas.is_dir():
        raise SystemExit(f"missing Gecko schemas at {schemas}")

    result: dict[str, dict[str, Any]] = {}
    for path in sorted(schemas.glob("*.json")):
        entries = load_json(path)
        if isinstance(entries, dict):
            entries = [entries]
        for schema in entries:
            namespace = schema.get("namespace")
            if not namespace or namespace == "manifest":
                # 'manifest' holds permission enum extensions, which the
                # manifest inventory reads separately.
                continue
            entry = result.setdefault(namespace, {"namespace": namespace})
            if schema.get("unsupported"):
                entry["unsupported"] = True
            if schema.get("description"):
                entry["description"] = re.sub(r"<[^>]+>", "", schema["description"]).strip()
            entry["permissions"] = schema.get("permissions")
            entry["allowed_contexts"] = schema.get("allowedContexts")
            entry["default_contexts"] = schema.get("defaultContexts")
            if schema.get("min_manifest_version") is not None:
                entry["min_manifest_version"] = schema["min_manifest_version"]
            if schema.get("max_manifest_version") is not None:
                entry["max_manifest_version"] = schema["max_manifest_version"]

            for group, kind in (("functions", "method"), ("events", "event"),
                                ("properties", "property")):
                for item in schema.get(group, []):
                    # A property may be a bare string: the schema uses that
                    # form for a read-only constant such as TAB_ID_NONE.
                    if isinstance(item, str):
                        entry.setdefault("members", {})[item] = {"kind": kind, "constant": True}
                        continue
                    name = item.get("name")
                    if not name:
                        continue
                    member = entry.setdefault("members", {}).setdefault(name, {})
                    member["kind"] = kind
                    # Several namespaces (tabs above all) state the permission on
                    # each member rather than on the namespace, because some
                    # members need more than the namespace does: tabs.captureTab
                    # requires <all_urls>, and tabs.hide requires tabHide. A
                    # member's own permission wins over its namespace's.
                    if item.get("permissions"):
                        member["permissions"] = item["permissions"]
                    if kind == "method":
                        if item.get("async"):
                            member["async"] = item["async"]
                        params = [describe_parameter(p) for p in item.get("parameters", [])]
                        if params:
                            member["parameters"] = params
                    elif kind == "event":
                        params = [describe_parameter(p) for p in item.get("parameters", [])]
                        if params:
                            member["parameters"] = params

            for type_entry in schema.get("types", []):
                name = type_entry.get("id") or type_entry.get("name")
                if not name:
                    continue
                types = entry.setdefault("types", {})
                types[name] = {
                    "type": type_entry.get("type", "object"),
                    "description": strip_html(type_entry.get("description")),
                }
    return result


def describe_parameter(param: dict[str, Any]) -> dict[str, Any]:
    """Flatten a Gecko parameter into something a validator can consume."""
    out: dict[str, Any] = {}
    name = param.get("name")
    if name:
        out["name"] = name
    kind = param.get("type")
    if kind:
        out["type"] = kind
    ref = param.get("$ref")
    if ref:
        out["ref"] = ref
    choices = param.get("choices")
    if choices:
        out["choices"] = [describe_parameter(choice) for choice in choices]
    if "optional" in param:
        out["optional"] = bool(param["optional"])
    if param.get("minimum") is not None:
        out["minimum"] = param["minimum"]
    if param.get("maximum") is not None:
        out["maximum"] = param["maximum"]
    if param.get("enum"):
        out["enum"] = param["enum"]
    if param.get("additionalProperties") is not None:
        out["additional_properties"] = True
    if param.get("maxLength") is not None:
        out["max_length"] = param["maxLength"]
    if param.get("minLength") is not None:
        out["min_length"] = param["minLength"]
    if param.get("pattern"):
        out["pattern"] = param["pattern"]
    if param.get("deprecated"):
        out["deprecated"] = True
    return out


def strip_html(text: str | None) -> str | None:
    if not text:
        return None
    return re.sub(r"\s+", " ", re.sub(r"<[^>]+>", "", text)).strip()


def geeko_permissions(root: Path) -> dict[str, Any]:
    """Every permission string a manifest may contain, and how it is granted.

    The Gecko schemas split permissions across an inheritance chain:
    'Permission' refers to 'PermissionNoPrompt', which refers to
    'OptionalPermissionNoPrompt' and 'PermissionPrivileged'; other schema files
    add to them with "$extend". Reading only the $extend entries misses most of
    the list (storage, alarms, notifications and idle live in manifest.json as
    base definitions), so the chain is resolved here.

    Two things come out of it that the permission system needs and MDN's prose
    only implies:

      all     every permission identifier, resolved through the chain
      classes how each one may be requested, which is what decides whether it
              belongs in "permissions", "optional_permissions" or neither
    """
    result: dict[str, list[str]] = {}
    for path in sorted((root / "gecko").glob("*.json")):
        entries = load_json(path)
        if isinstance(entries, dict):
            entries = [entries]
        for schema in entries:
            if schema.get("namespace") != "manifest":
                continue
            for type_entry in schema.get("types", []):
                target = type_entry.get("$extend")
                if not target:
                    continue
                choices = type_entry.get("choices", [])
                if choices:
                    # Kept as raw choices, not flattened names: a choice may be a
                    # $ref into another permission type, which only resolves
                    # once the whole graph has been collected.
                    result.setdefault(target, []).extend(choices)

    # Base definitions (id rather than $extend), which the extends build on.
    base: dict[str, dict[str, Any]] = {}
    for path in sorted((root / "gecko").glob("*.json")):
        entries = load_json(path)
        if isinstance(entries, dict):
            entries = [entries]
        for schema in entries:
            if schema.get("namespace") != "manifest":
                continue
            for type_entry in schema.get("types", []):
                name = type_entry.get("id")
                if name and "Permission" in name:
                    base.setdefault(name, type_entry)

    # Resolve the chain by repeatedly folding $ref choices into their target.
    resolved: dict[str, list[str]] = {}

    def expand(type_name: str, seen: frozenset[str] = frozenset()) -> list[str]:
        if type_name in seen:  # a malformed schema should not loop forever
            return []
        seen = seen | {type_name}
        names: list[str] = []
        sources: list[list[dict[str, Any]]] = []
        if type_name in base:
            sources.append(base[type_name].get("choices", []))
        if type_name in result:
            sources.append(result[type_name])
        for choices in sources:
            for choice in choices:
                ref = choice.get("$ref")
                if ref:
                    names.extend(expand(ref, seen))
                elif choice.get("type") == "string":
                    if "enum" in choice:
                        names.extend(choice["enum"])
                    elif choice.get("pattern"):
                        # A permission family admitted by pattern, e.g.
                        # "^experiments(\.\w+)+$". The pattern is kept verbatim
                        # so the permission check can apply it rather than
                        # inventing a name for it.
                        names.append(f"pattern:{choice['pattern']}")
                    else:
                        names.append("<free-form>")
        # Preserve order, drop duplicates.
        seen_names: list[str] = []
        for name in names:
            if name not in seen_names:
                seen_names.append(name)
        return seen_names

    for type_name in ("Permission", "OptionalPermission", "PermissionPrivileged"):
        names = expand(type_name)
        if names:
            resolved[type_name] = names

    every: list[str] = []
    for names in resolved.values():
        every.extend(n for n in names if n not in every)

    # Flatten the per-target choice lists for readability in the JSON.
    extends: dict[str, list[str]] = {}
    for target, choices in sorted(result.items()):
        flat: list[str] = []
        for choice in choices:
            if choice.get("type") == "string":
                flat.extend(choice.get("enum") or [f"pattern:{choice['pattern']}"
                                                   if choice.get("pattern") else "<free-form>"])
            elif choice.get("$ref"):
                flat.append(f"$ref:{choice['$ref']}")
        extends[target] = flat

    return {
        "classes": resolved,
        "all": every,
        "extends": extends,
    }


# ------------------------------------------------------------------- combine

def canonical_namespace_key(name: str) -> str:
    """Fold a namespace name so MDN's directories line up with the schema names.

    'browseraction', 'browser_action' and 'browserAction' all fold to the same
    key. This is only ever used for matching, never for display: the emitted
    name is always the spelling from BCD or the Gecko schemas.
    """
    return re.sub(r"[^a-z0-9]", "", name.lower())


def resolve_mdn_names(mdn: dict[str, dict], canonical: list[str]) -> dict[str, dict]:
    """Map each MDN namespace directory onto its correctly-spelled name."""
    by_key: dict[str, list[str]] = {}
    for name in canonical:
        by_key.setdefault(canonical_namespace_key(name), []).append(name)
    resolved: dict[str, dict] = {}
    for entry in mdn.values():
        candidates = by_key.get(canonical_namespace_key(entry["dir"]), [])
        if len(candidates) == 1:
            resolved[candidates[0]] = entry
        elif not candidates:
            # Documented by MDN but absent from both machine-readable sources.
            # Keep it, with a name derived from the directory, so it shows up as
            # unimplemented instead of vanishing.
            resolved[entry["dir"]] = entry
    return resolved


def merge_namespace_lists(mdn: dict, bcd: dict, gecko: dict) -> Iterator[dict[str, Any]]:
    """Union of the three namespace views, with provenance for each name.

    Keeping the provenance is what lets a reviewer answer "why is this here?"
    without re-running the fetcher: a name in all three is unambiguous, a name
    only in MDN is undocumented-elsewhere, and a name only in the Gecko
    schemas is a Firefox implementation detail Iridium may choose to expose.
    """
    names = sorted(set(mdn) | set(bcd) | set(gecko))
    for name in names:
        bcd_entry = bcd.get(name, {})
        gecko_entry = gecko.get(name, {})
        members: dict[str, Any] = {}

        for member_name, member in gecko_entry.get("members", {}).items():
            members.setdefault(member_name, {}).update(member)
            members[member_name]["in_gecko"] = True
        for member_name, member in bcd_entry.get("members", {}).items():
            target = members.setdefault(member_name, {})
            target.update({k: v for k, v in member.items() if k != "kind"})
            target.setdefault("kind", member.get("kind", "method"))
            target["in_bcd"] = True
            support = member.get("support") or {}
            if any(support.get(browser, {}).get("supported") for browser in support):
                target["documented"] = True

        # BCD nests a sub-feature for every named parameter as well as for every
        # real member, so webRequest.onBeforeRequest would report a member called
        # "details". The Gecko parameter list says which names are arguments.
        for member_name, member in members.items():
            arguments = {p.get("name") for p in (member.get("parameters") or [])}
            if arguments and "members" in member:
                member["members"] = {name: sub for name, sub in member["members"].items()
                                     if name not in arguments}
        for member_name in gecko_entry.get("types", {}):
            members.setdefault(member_name, {"kind": "type", "in_gecko": True})

        entry: dict[str, Any] = {
            "namespace": name,
            "in_mdn": name in mdn,
            "mdn_dir": (mdn.get(name) or {}).get("mdn_dir"),
            "in_bcd": name in bcd,
            "in_gecko": name in gecko,
            "description": gecko_entry.get("description") or strip_html(
                (mdn.get(name) or {}).get("summary")),
            "permissions": gecko_entry.get("permissions"),
            "allowed_contexts": gecko_entry.get("allowed_contexts"),
            "default_contexts": gecko_entry.get("default_contexts"),
            "support": bcd_entry.get("support"),
            "mdn_url": bcd_entry.get("mdn_url") or (mdn.get(name) or {}).get("mdn_url"),
            "members": members,
        }
        if gecko_entry.get("unsupported"):
            entry["unsupported_upstream"] = True
        if "min_manifest_version" in gecko_entry:
            entry["min_manifest_version"] = gecko_entry["min_manifest_version"]
        if "max_manifest_version" in gecko_entry:
            entry["max_manifest_version"] = gecko_entry["max_manifest_version"]
        yield entry


def count_members(namespaces: list[dict[str, Any]]) -> dict[str, int]:
    kinds: dict[str, int] = {}
    for namespace in namespaces:
        for member in namespace["members"].values():
            kind = member.get("kind", "type")
            kinds[kind] = kinds.get(kind, 0) + 1
    return dict(sorted(kinds.items()))


def write_json(path: Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2, sort_keys=True)
        handle.write("\n")
    log(f"wrote {path.relative_to(ROOT)} ({path.stat().st_size // 1024} KiB)")


def merge_manifest_keys(mdn: dict[str, dict], bcd: dict[str, dict]) -> list[dict[str, Any]]:
    """Union of MDN's manifest key list and BCD's manifest compatibility data."""
    keys = []
    for name in sorted(set(mdn) | set(bcd)):
        mdn_entry = mdn.get(name, {})
        bcd_entry = bcd.get(name, {})
        keys.append({
            "key": name,
            "in_mdn": name in mdn,
            "in_bcd": name in bcd,
            "note": mdn_entry.get("note", ""),
            "mdn_url": bcd_entry.get("mdn_url") or mdn_entry.get("mdn_url"),
            "support": bcd_entry.get("support"),
            "properties": sorted(bcd_entry.get("properties", {})),
        })
    return keys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", default=str(ROOT / ".cache" / "webext-upstream"))
    parser.add_argument("--out", default=str(ROOT / "docs" / "extensions" / "data"))
    args = parser.parse_args()

    cache = Path(args.cache).resolve()
    out = Path(args.out).resolve()

    sources_path = cache / "sources.json"
    sources = load_json(sources_path) if sources_path.is_file() else {}
    if not sources:
        log("warning: no sources.json in the cache; inventory will not record provenance")

    mdn_raw = mdn_namespaces(cache / "mdn_content")
    mdn_keys = mdn_manifest_keys(cache / "mdn_content")
    bcd_api = bcd_members(cache)
    bcd_manifest_keys = bcd_manifest(cache)
    gecko = geeko_namespaces(cache)
    # MDN's directories are lowercased, so resolve them against the schema
    # spellings before merging or every camelCase namespace splits in two.
    mdn = resolve_mdn_names(mdn_raw, sorted(set(bcd_api) | set(gecko)))

    namespaces = list(merge_namespace_lists(mdn, bcd_api, gecko))

    api_inventory = {
        "_generated_by": "tools/webext/inventory.py",
        "_sources": sources,
        "_counts": {
            "namespaces": len(namespaces),
            "members": sum(len(n["members"]) for n in namespaces),
            "by_kind": count_members(namespaces),
        },
        "namespaces": namespaces,
    }
    write_json(out / "api-inventory.json", api_inventory)

    manifest_inventory = {
        "_generated_by": "tools/webext/inventory.py",
        "_sources": sources,
        "permissions": geeko_permissions(cache),
        "keys": merge_manifest_keys(mdn_keys, bcd_manifest_keys),
    }
    write_json(out / "manifest-inventory.json", manifest_inventory)

    log(f"namespaces: {len(namespaces)}, "
        f"members: {api_inventory['_counts']['members']}, "
        f"manifest keys: {len(manifest_inventory['keys'])}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
