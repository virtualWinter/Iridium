#pragma once

#include "extensions/ExtensionRegistry.hpp"

#include <QString>
#include <QStringList>

#include <vector>

namespace iridium::extensions {

// Injects enabled extensions' content scripts into a page.
//
// This is the bridge to the engine. WebKit's own WebExtension host is not
// available in the installed WPE build, so scripts are delivered the same way
// the color scheme emulation already works: a WebKitUserScript installed on the
// view's user content manager at document start.
class ContentScriptInjector final {
public:
    // Builds the single script to inject for `url`, or an empty string when no
    // extension applies. Sources are concatenated in registry order; each is
    // wrapped in an IIFE so one script cannot leak globals into the next.
    static QString buildScript(const ExtensionRegistry& registry, const QString& url);

    };

} // namespace iridium::extensions
