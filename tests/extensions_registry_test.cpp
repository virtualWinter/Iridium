// Covers registry behaviour against real on-disk extension fixtures: scanning,
// enable/disable with dependencies, per-URL script selection, exclude_matches,
// and that a manifest cannot read files outside its own directory.

#include "extensions/ContentScriptInjector.hpp"
#include "extensions/ExtensionRegistry.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>
#include <string>

using iridium::extensions::ContentScriptInjector;
using iridium::extensions::ExtensionRegistry;

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

bool writeFile(const QString& path, const QByteArray& contents)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    return file.write(contents) == contents.size();
}

void makeExtension(const QString& root, const QString& name, const QByteArray& manifest,
    const QByteArray& script = QByteArray())
{
    writeFile(root + QLatin1Char('/') + name + QStringLiteral("/manifest.json"), manifest);
    if (!script.isEmpty())
        writeFile(root + QLatin1Char('/') + name + QStringLiteral("/content.js"), script);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir profile;
    QTemporaryDir root;
    if (!profile.isValid() || !root.isValid()) {
        std::printf("FAIL: could not create temp dirs\n");
        return 1;
    }

    const QString extRoot = root.path();
    makeExtension(extRoot, QStringLiteral("alpha"),
        R"({"manifest_version":3,"name":"Alpha","version":"1.0",
            "permissions":["storage"],
            "content_scripts":[{"matches":["*://example.com/*"],"js":["content.js"]}]})",
        "window.__alpha = true;");
    makeExtension(extRoot, QStringLiteral("beta"),
        R"({"manifest_version":2,"name":"Beta","version":"1.0",
            "content_scripts":[{"matches":["<all_urls>"],
                                "exclude_matches":["*://skip.test/*"],
                                "js":["content.js"]}]})",
        "window.__beta = true;");
    makeExtension(extRoot, QStringLiteral("gamma"),
        R"({"manifest_version":2,"name":"Gamma","version":"1.0",
            "dependencies":["alpha"],
            "content_scripts":[{"matches":["*://example.com/*"],"js":["content.js"]}]})",
        "window.__gamma = true;");
    makeExtension(extRoot, QStringLiteral("broken"),
        R"({"manifest_version":99,"name":"Broken","version":"1.0"})");

    ExtensionRegistry registry(profile.path());
    registry.loadFrom(extRoot);

    check(registry.extensions().size() == 3, "three valid extensions loaded, got "
        + std::to_string(registry.extensions().size()));
    check(registry.loadErrors().contains("broken"), "broken extension reported");
    check(registry.find(QStringLiteral("alpha")) != nullptr, "alpha found by id");
    check(registry.find(QStringLiteral("nope")) == nullptr, "unknown id returns null");

    // Nothing is enabled until asked.
    check(registry.scriptsForUrl(QStringLiteral("https://example.com/")).empty(),
        "no scripts before enabling");

    check(registry.setEnabled(QStringLiteral("alpha"), true), "enable alpha");
    check(registry.isEnabled(QStringLiteral("alpha")), "alpha is enabled");

    auto forExample = registry.scriptsForUrl(QStringLiteral("https://example.com/page"));
    check(forExample.size() == 1, "one script for example.com");
    if (!forExample.empty())
        check(forExample.front().extensionId == "alpha", "script attributed to alpha");

    check(registry.scriptsForUrl(QStringLiteral("https://other.test/")).empty(),
        "alpha does not match other hosts");

    // Enablement persists to the profile.
    {
        ExtensionRegistry reloaded(profile.path());
        check(reloaded.isEnabled(QStringLiteral("alpha")), "enabled state persisted");
    }

    // A dependency pulls its dependency in.
    check(registry.setEnabled(QStringLiteral("gamma"), true), "enable gamma");
    check(registry.isEnabled(QStringLiteral("alpha")), "dependency auto-enabled");
    check(registry.scriptsForUrl(QStringLiteral("https://example.com/")).size() == 2,
        "dependency's scripts also apply");

    // exclude_matches wins over matches: beta matches <all_urls> but must not
    // apply on the excluded host.
    check(registry.setEnabled(QStringLiteral("beta"), true), "enable beta");
    const auto forSkip = registry.scriptsForUrl(QStringLiteral("https://skip.test/x"));
    bool sawBeta = false;
    for (const auto& entry : forSkip) {
        if (entry.extensionId == "beta")
            sawBeta = true;
    }
    check(!sawBeta, "exclude_matches removes beta on skip.test");

    // Generated script is wrapped and contains the extension source.
    const QString source = ContentScriptInjector::buildScript(registry,
        QStringLiteral("https://example.com/"));
    check(source.contains("window.__alpha"), "alpha source present");
    check(source.contains("window.__gamma"), "gamma source present");
    check(source.contains("'use strict'"), "generated script is strict mode");
    check(!ContentScriptInjector::buildScript(registry, QStringLiteral("about:blank"))
        .contains("__alpha"), "no script for unmatched url");

    

    // A manifest must not reach outside its own directory.
    makeExtension(extRoot, QStringLiteral("escape"),
        R"({"manifest_version":2,"name":"Escape","version":"1.0",
            "content_scripts":[{"matches":["<all_urls>"],"js":["../alpha/manifest.json"]}]})");
    ExtensionRegistry escapeRegistry(profile.path() + QStringLiteral("/2"));
    escapeRegistry.loadFrom(extRoot);
    auto* escape = escapeRegistry.find(QStringLiteral("escape"));
    check(escape != nullptr, "escape extension parsed");
    if (escape) {
        check(escape->readResource(QStringLiteral("../alpha/manifest.json")).isEmpty(),
            "path traversal outside the extension directory is refused");
        check(escape->readResource(QStringLiteral("does-not-exist.js")).isEmpty(),
            "missing file reads as empty");
    }

    if (g_failures == 0)
        std::printf("PASS: extensions registry and script injection\n");
    else
        std::printf("%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
