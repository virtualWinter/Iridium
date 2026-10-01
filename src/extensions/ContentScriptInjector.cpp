#include "extensions/ContentScriptInjector.hpp"

#include "extensions/JsSource.hpp"

namespace iridium::extensions {


QString ContentScriptInjector::buildScript(const ExtensionRegistry& registry, const QString& url)
{
    const std::vector<ExtensionRegistry::InjectedScript> scripts = registry.scriptsForUrl(url);
    if (scripts.empty())
        return {};

    QString source;
    source.reserve(4096);
    source += QLatin1String("(function() {\n");
    source += QLatin1String("'use strict';\n");

    for (const ExtensionRegistry::InjectedScript& script : scripts) {
        // Declare the running extension before its first file executes, so
        // browser.runtime.id and getManifest() are correct from the first line.
        source += script.identity;
        for (const QString& body : script.sources) {
            // Each file runs in its own scope so top-level declarations in one
            // content script do not collide with another's.
            source += QLatin1String("try {\n(function() {\n");
            source += body;
            source += QLatin1String("\n})();\n} catch (e) {\n"
                "console.error('[iridium] content script failed', e);\n}\n");
        }
    }

    source += QLatin1String("})();\n");
    return source;
}

} // namespace iridium::extensions