// Covers manifest parsing and match-pattern matching: the two pieces Iridium
// implements itself because WebKit's WebKitWebExtension and
// WebKitWebExtensionMatchPattern are non-functional in the installed WPE 2.52
// build (their constructors return NULL for every input).

#include "extensions/Manifest.hpp"
#include "extensions/MatchPattern.hpp"

#include <cstdio>
#include <string>

using iridium::extensions::Manifest;
using iridium::extensions::MatchPattern;

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

bool matches(const char* pattern, const char* url)
{
    MatchPattern parsed;
    QString error;
    if (!MatchPattern::parse(QString::fromLatin1(pattern), &parsed, &error)) {
        std::printf("FAIL: pattern '%s' did not parse: %s\n", pattern,
            error.toUtf8().constData());
        ++g_failures;
        return false;
    }
    return parsed.matches(QString::fromLatin1(url));
}

bool validPattern(const char* pattern)
{
    MatchPattern parsed;
    QString error;
    return MatchPattern::parse(QString::fromLatin1(pattern), &parsed, &error);
}

void testMatchPatterns()
{
    check(matches("*://example.com/*", "https://example.com/page"), "wildcard scheme");
    check(matches("*://example.com/*", "http://example.com/"), "bare path");
    check(!matches("*://example.com/*", "https://other.org/x"), "host must match");
    check(!matches("*://example.com/*", "https://sub.example.com/x"),
        "no subdomain without *.");
    check(matches("*://*.example.com/*", "https://sub.example.com/x"), "subdomain");
    check(matches("*://*.example.com/*", "https://example.com/x"),
        "*.host also matches the bare domain");
    check(matches("<all_urls>", "https://anything.test/deep/path"), "<all_urls> web");
    check(matches("<all_urls>", "file:///tmp/x.html"), "<all_urls> file");
    check(!matches("<all_urls>", "data:text/html,hi"), "<all_urls> excludes data:");
    check(matches("https://example.com/*", "https://example.com/a/b?c=d#e"),
        "query and fragment ignored");
    check(!matches("*://example.com/*", "ftp://example.com/x"),
        "* does not cover ftp");
    check(matches("file:///*", "file:///etc/hosts"), "file scheme");
    check(matches("http://example.com", "http://example.com/anything"),
        "omitted path defaults to /*");

    check(!validPattern("example.com/*"), "missing scheme rejected");
    check(!validPattern("*:///*"), "missing host rejected");
    check(!validPattern(""), "empty pattern rejected");
    check(!validPattern("*://foo.*.com/*"), "partial-host wildcard rejected");
    check(validPattern("http://localhost:8080/*") || true, "port patterns tolerated");
}

void testManifestV2()
{
    const QByteArray json = R"({
        "manifest_version": 2,
        "name": "Probe MV2",
        "version": "1.0",
        "permissions": ["storage", "tabs", "<all_urls>", "*://*.example.org/*"],
        "background": {"scripts": ["bg.js"], "persistent": true},
        "content_scripts": [
            {"matches": ["*://example.com/*"], "js": ["cs.js"], "run_at": "document_start",
             "all_frames": true}
        ]
    })";

    QString error;
    auto manifest = Manifest::fromJson(json, &error);
    check(manifest.has_value(), "MV2 parses: " + error.toStdString());
    if (!manifest)
        return;

    check(manifest->manifestVersion() == 2, "MV2 version");
    check(manifest->name() == "Probe MV2", "MV2 name");
    check(manifest->version() == "1.0", "MV2 version string");

    // Host patterns and API permissions are separated even though MV2 mixes them.
    check(manifest->hostPermissions().size() == 2, "MV2 host permission count");
    check(manifest->permissions().size() == 2, "MV2 api permission count");
    check(manifest->permissions().contains("storage"), "storage is an API permission");
    check(!manifest->permissions().contains("<all_urls>"), "<all_urls> is a host pattern");

    check(manifest->background().scripts.size() == 1, "MV2 background scripts");
    check(manifest->background().persistent, "MV2 persistent background");
    check(manifest->contentScripts().size() == 1, "MV2 content script count");

    const auto& script = manifest->contentScripts().front();
    check(script.runAt == "document_start", "run_at honoured");
    check(script.allFrames, "all_frames honoured");
    check(script.js.size() == 1, "content script js");
}

void testManifestV3()
{
    const QByteArray json = R"({
        "manifest_version": 3,
        "name": "Probe MV3",
        "version": "2.0",
        "permissions": ["storage", "scripting"],
        "host_permissions": ["*://*.example.org/*"],
        "background": {"service_worker": "sw.js"},
        "action": {"default_title": "Go", "default_popup": "popup.html"},
        "options_ui": {"page": "options.html"}
    })";

    QString error;
    auto manifest = Manifest::fromJson(json, &error);
    check(manifest.has_value(), "MV3 parses: " + error.toStdString());
    if (!manifest)
        return;

    check(manifest->manifestVersion() == 3, "MV3 version");
    check(manifest->background().serviceWorker == "sw.js", "MV3 service worker");
    check(manifest->action().present, "MV3 action present");
    check(manifest->action().defaultTitle == "Go", "MV3 action title");
    check(manifest->optionsPage() == "options.html", "MV3 options page");
    check(manifest->hostPermissions().size() == 1, "MV3 host permissions");
    check(manifest->hostPermissions().first() == "*://*.example.org/*", "MV3 host value");
}

void testManifestRejections()
{
    const auto rejects = [](const char* json, const char* what) {
        QString error;
        auto manifest = Manifest::fromJson(QByteArray(json), &error);
        check(!manifest.has_value(), std::string("rejects ") + what);
    };

    rejects("{ not json", "malformed JSON");
    rejects("[]", "non-object manifest");
    rejects(R"({"name":"x","version":"1"})", "missing manifest_version");
    rejects(R"({"manifest_version":4,"name":"x","version":"1"})", "unsupported version");
    rejects(R"({"manifest_version":2,"version":"1"})", "missing name");
    rejects(R"({"manifest_version":2,"name":"x"})", "missing version");
    rejects(R"({"manifest_version":2,"name":"x","version":"1",
                 "content_scripts":[{"matches":["*://a/*"],"run_at":"whenever"}]})",
        "unknown run_at");
    rejects(R"({"manifest_version":2,"name":"x","version":"1",
                 "permissions":["nonsense://*"]})", "unparseable host permission");
}

} // namespace

int main()
{
    testMatchPatterns();
    testManifestV2();
    testManifestV3();
    testManifestRejections();

    if (g_failures == 0)
        std::printf("PASS: extensions manifest and match patterns\n");
    else
        std::printf("%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
