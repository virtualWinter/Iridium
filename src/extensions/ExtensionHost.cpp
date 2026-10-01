#include "extensions/ExtensionHost.hpp"

#include "engine/WebView.hpp"
#include "engine/webkit/WebKitScriptBridge.hpp"
#include "extensions/ContentScriptInjector.hpp"
#include "extensions/ExtensionPaths.hpp"

namespace iridium::extensions {

ExtensionHost::ExtensionHost(ExtensionRegistry& registry, ExtensionApiHost* host, QObject* parent)
    : QObject(parent)
    , m_registry(&registry)
    , m_api(std::make_unique<ExtensionApi>(registry, host, this))
{
    connect(&registry, &ExtensionRegistry::changed, this, &ExtensionHost::reload);
}

ExtensionHost::~ExtensionHost() = default;

void ExtensionHost::loadDefaults()
{
    if (!m_registry)
        return;
    for (const QString& path : ExtensionPaths::searchPaths())
        m_registry->loadFrom(path);
}

void ExtensionHost::reload()
{
    if (!m_registry)
        return;
    m_registry->invalidateStorageCache();
    // Re-inject into live views so a newly enabled extension takes effect
    // without the user reloading the page.
    for (auto* view : m_views)
        injectContentScripts(view);
}

void ExtensionHost::attachView(engine::WebView* view)
{
    if (!view || !m_registry || m_views.contains(view))
        return;

    m_views.insert(view);

    // The bridge is per view and shared by every extension injected into it.
    // When it cannot be registered the shim would silently do nothing, so only
    // the content scripts are injected and they run without browser.* APIs.
    auto bridge = std::make_unique<engine::webkit::WebKitScriptBridge>(*view);
    if (bridge->registered())
        m_api->installOn(*view, std::move(bridge));

    injectContentScripts(view);
}

void ExtensionHost::detachView(engine::WebView* view)
{
    if (!view)
        return;
    m_views.remove(view);
    // Drop the bridge first: it holds references into this view's engine.
    m_api->releaseFor(*view);
    view->clearContentScript();
}

void ExtensionHost::refreshView(engine::WebView* view)
{
    injectContentScripts(view);
}

void ExtensionHost::injectContentScripts(engine::WebView* view)
{
    if (!view || !m_registry)
        return;

    // The API shim comes first: content scripts call into it on their first
    // line. The content script bundle declares the running extension's identity
    // ahead of each of its files.
    QString source = m_api->shim();
    if (m_api->hasBridgeFor(*view)) {
        source += ContentScriptInjector::buildScript(*m_registry,
            QString::fromStdString(view->currentUrl()));
    }
    view->setContentScriptSource(source.toStdString());
}

} // namespace iridium::extensions