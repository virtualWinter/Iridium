#pragma once

#include "engine/ScriptBridge.hpp"
#include "extensions/ExtensionApi.hpp"
#include "extensions/ExtensionRegistry.hpp"

#include <QObject>
#include <QSet>

#include <memory>
#include <vector>

namespace iridium::engine {
class WebView;
}

namespace iridium::extensions {

// Ties the registry, the API surface and the engine together.
//
// Owns one script bridge per attached view. Re-injection happens on
// navigation and whenever the enabled set changes, so toggling an extension
// takes effect without reloading the page.
class ExtensionHost final : public QObject {
    Q_OBJECT
public:
    // `host` supplies live tab and window state to the API layer.
    ExtensionHost(ExtensionRegistry& registry, ExtensionApiHost* host, QObject* parent = nullptr);
    ~ExtensionHost() override;

    ExtensionRegistry* registry() { return m_registry; }
    ExtensionApi* api() { return m_api.get(); }

    // Scans the XDG search paths. Safe to call once at startup.
    void loadDefaults();

    // Starts delivering extension scripts to `view`, and keeps them updated.
    void attachView(engine::WebView* view);
    // Stops delivering to `view` and clears anything already injected.
    void detachView(engine::WebView* view);
    // Re-injects for `view`, e.g. after a navigation.
    void refreshView(engine::WebView* view);

// Asks the API host to update tab state and reports success. Used by the API
    // layer when an extension calls tabs.create/update/remove.
private:
    void reload();
    void injectContentScripts(engine::WebView* view);

    ExtensionRegistry* m_registry { nullptr };
    std::unique_ptr<ExtensionApi> m_api;
    // Membership is all that matters; the API owns one bridge per attached view.
    QSet<engine::WebView*> m_views;
};

} // namespace iridium::extensions