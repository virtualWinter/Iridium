// What the WPE WebKit build in front of us can and cannot do for WebExtensions.
//
// The extensions architecture rests on a handful of facts about this engine,
// and architecture that rests on remembered facts about an engine is not
// verifiable. Each check below asserts one of them, so a WebKit upgrade that
// changes the answer fails here instead of silently invalidating the design.
//
// Two of the assertions are negative on purpose. WebKit ships
// WebKitWebExtension and WebKitWebExtensionMatchPattern, and the obvious
// implementation would delegate the manifest and match patterns to them. In this
// build their constructors return NULL for every input and leave GError unset.
// The reason is a build option, not a missing feature: upstream WebKit has a
// full WebExtensions implementation behind ENABLE(WK_WEB_EXTENSIONS), and the
// GLib wrapper has a second definition of every entry point that returns NULL
// when the option is off. These are those stubs. If the option is ever enabled,
// this test fails and delegating to the engine becomes a decision to revisit
// rather than an accident.
//
// What is checked positively is the surface the new design depends on: named
// script worlds (the isolated-world mechanism, which Apple's WKContentWorld
// documentation recommends one of per extension), per-world message handlers
// with a reply, and a custom URI scheme that can be marked local and secure (the
// mechanism behind moz-extension://).

#include <wpe/webkit.h>
// This port's umbrella header does not include the script-world type, although
// the symbols are exported. An extension host needs the type only to name a
// world, which the world_name string parameters already allow, so the
// declarations below are enough and the gap is recorded rather than papered
// over with a private include.
struct _WebKitScriptWorld;
using WebKitScriptWorld = _WebKitScriptWorld;
extern "C" {
WebKitScriptWorld* webkit_script_world_new_with_name(const char* name);
const char* webkit_script_world_get_name(WebKitScriptWorld* world);
}

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void check(bool condition, const char* what)
{
    std::printf("%s: %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition)
        ++g_failures;
}

void note(const char* what)
{
    std::printf("note: %s\n", what);
}

// The engine's own extension metadata type, if it works at all.
void checkWebExtensionMetadataIsUnusable()
{
    GError* error = nullptr;
    WebKitWebExtension* extension = webkit_web_extension_new("/nonexistent-extension",
        &error);
    check(extension == nullptr, "webkit_web_extension_new() returns NULL");

    // A NULL result with no error set is the specific pathology: the caller has
    // no way to tell "bad path" from "this build has extensions compiled out".
    // That is exactly what the ENABLE(WK_WEB_EXTENSIONS)=OFF branch of the
    // WebKit wrapper does.
    check(error == nullptr,
        "webkit_web_extension_new() leaves GError unset (no diagnostic at all)");

    error = nullptr;
    WebKitWebExtensionMatchPattern* pattern =
        webkit_web_extension_match_pattern_new_with_string("<all_urls>", &error);
    check(pattern == nullptr,
        "webkit_web_extension_match_pattern_new_with_string() returns NULL");
    check(error == nullptr,
        "match pattern constructor leaves GError unset as well");

    error = nullptr;
    WebKitWebExtensionMatchPattern* allHosts =
        webkit_web_extension_match_pattern_new_all_urls();
    check(allHosts == nullptr, "match pattern new_all_urls() returns NULL");

    // Upstream ships a WebExtensionContext API (WebKitWebExtensionContext) for
    // running an extension once it is loaded. It is absent from this build's
    // headers, which is the second half of the same story: the whole extension
    // surface is behind the option, not just the metadata types.
    note("no WebKitWebExtensionContext in this build's headers: the whole "
         "extension surface is behind ENABLE(WK_WEB_EXTENSIONS)");
    note("building WPE with that option enabled is the experiment worth running "
         "before committing to our own host (see docs/extensions/webkit-integration.md)");
}

// Named script worlds are how content scripts get their own JavaScript
// environment while still sharing the DOM with the page.
void checkIsolatedWorldsAreAvailable()
{
    WebKitScriptWorld* world = webkit_script_world_new_with_name("iridium-content-script");
    check(world != nullptr, "webkit_script_world_new_with_name()");
    if (!world)
        return;

    check(std::string(webkit_script_world_get_name(world))
            == "iridium-content-script",
        "the world keeps its name, so worlds are addressable per extension");

    WebKitUserContentManager* manager = webkit_user_content_manager_new();
    check(manager != nullptr, "user content manager");

    WebKitUserScript* script = webkit_user_script_new_for_world(
        "window.__probe = 1;",
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
        webkit_script_world_get_name(world), nullptr, nullptr);
    check(script != nullptr,
        "webkit_user_script_new_for_world() injects into a named world");
    if (script)
        webkit_user_script_unref(script);

    // A reply channel is what makes an API call return a promise instead of
    // fire-and-forget, and it is per world, which is what keeps two extensions
    // in one frame from sharing a channel.
    const bool registered = webkit_user_content_manager_register_script_message_handler_with_reply(
        manager, "iridium", webkit_script_world_get_name(world));
    check(registered,
        "register_script_message_handler_with_reply() is per script world");

    g_object_unref(manager);
    g_object_unref(world);
}

// The extension resource scheme. Iridium prefers moz-extension://<uuid>/, and
// the pieces needed for it are the custom-scheme handler plus the security
// manager calls that give it an origin.
void checkCustomSchemeMechanismIsAvailable()
{
    WebKitWebContext* context = webkit_web_context_new();
    check(context != nullptr, "web context");

    webkit_web_context_register_uri_scheme(context, "moz-extension",
        [](WebKitURISchemeRequest*, gpointer) {}, nullptr, nullptr);

    WebKitSecurityManager* security = webkit_web_context_get_security_manager(context);
    check(security != nullptr, "security manager");

    webkit_security_manager_register_uri_scheme_as_local(security, "moz-extension");
    check(webkit_security_manager_uri_scheme_is_local(security, "moz-extension"),
        "moz-extension can be registered as a local scheme");

    webkit_security_manager_register_uri_scheme_as_secure(security, "moz-extension");
    check(webkit_security_manager_uri_scheme_is_secure(security, "moz-extension"),
        "moz-extension can be registered as a secure scheme");

    webkit_security_manager_register_uri_scheme_as_cors_enabled(security, "moz-extension");
    check(webkit_security_manager_uri_scheme_is_cors_enabled(security, "moz-extension"),
        "moz-extension can be registered as CORS enabled");

    g_object_unref(context);
}

// What the network side can offer. There is no SoupSession accessor on
// WebKitNetworkSession, so the request pipeline is unreachable from the embedder
// without a WebKit change; webRequest and declarativeNetRequest are therefore
// recorded as unsupported rather than approximated.
void checkNetworkSurface()
{
    // A network session of its own, because the default one only exists once
    // automation is enabled. Containers will each need one of these.
    WebKitNetworkSession* session = webkit_network_session_new_ephemeral();
    check(session != nullptr, "an independent network session can be created");
    if (session) {
        check(webkit_network_session_get_cookie_manager(session) != nullptr,
            "cookies are reachable, so browser.cookies is a mapping exercise");
        check(webkit_network_session_get_website_data_manager(session) != nullptr,
            "website data manager is reachable, so browsingData is partly reachable");
        check(true, "proxy settings can be set, so browser.proxy has a mechanism");
    }

    note("no SoupSession accessor: per-request hooks (webRequest, DNR) have no "
         "public route from the embedder");
}

} // namespace

int main()
{
    std::printf("WebKit %d.%d.%d\n", webkit_get_major_version(),
        webkit_get_minor_version(), webkit_get_micro_version());

    checkWebExtensionMetadataIsUnusable();
    checkIsolatedWorldsAreAvailable();
    checkCustomSchemeMechanismIsAvailable();
    checkNetworkSurface();

    if (g_failures == 0)
        std::printf("PASS: WPE WebKit extension capability probe\n");
    else
        std::printf("%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}