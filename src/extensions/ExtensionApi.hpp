#pragma once

#include "extensions/ExtensionRegistry.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace iridium::engine {
// Declared here rather than included: the API layer only needs pointers to a
// view, and pulling the engine header in would make this untestable without it.
class ScriptBridge;
class WebView;
}

namespace iridium::extensions {

// Tabs and windows as the API layer needs to see them. The UI owns the real
// state, so the API calls back into a host implementation rather than
// touching widgets itself.
struct TabSnapshot {
    int id { 0 };
    int windowId { 0 };
    QString url;
    QString title;
    QString favIconUrl;
    bool active { false };
    bool audible { false };
    int index { 0 };
    int width { 0 };
    int height { 0 };
};

class ExtensionApiHost {
public:
    virtual ~ExtensionApiHost() = default;

    virtual std::vector<TabSnapshot> tabs() const = 0;
    virtual std::optional<TabSnapshot> activeTab() const = 0;
    virtual std::optional<TabSnapshot> tab(int id) const = 0;
    virtual int createTab(const QString& url, bool active) = 0;
    virtual void removeTab(int id) = 0;
    virtual void updateTab(int id, const QJsonObject& properties) = 0;
    virtual void activateTab(int id) = 0;
    virtual std::vector<int> windowIds() const = 0;
    virtual int currentWindow() const = 0;
};

// Implements the browser.* namespaces an extension can call.
//
// The surface is deliberately explicit about what this build cannot do: an
// unavailable method rejects its promise with a message naming the reason,
// rather than resolving to undefined and letting an extension misread that as
// success.
class ExtensionApi final : public QObject {
    Q_OBJECT
public:
    ExtensionApi(ExtensionRegistry& registry, ExtensionApiHost* host, QObject* parent = nullptr);
    ~ExtensionApi() override;

    // Registers every implemented method on `bridge` and records `view` as the tab
    // calls are attributed to. The bridge is shared by all extensions injected
    // into one view, so this runs once per view; the per-extension identity
    // travels in each message instead.
    //
    // The bridge is built by the caller because only the engine layer knows which
    // transport to use, which is what keeps this class testable without WebKit.
    // The API layer takes ownership, so a bridge dies with the view it serves.
    void installOn(engine::WebView& view, std::unique_ptr<engine::ScriptBridge> bridge);

    // For a context that is not a tab, such as an options page, which has no
    // view to attribute calls to. The API layer owns the bridge either way.
    void installDetached(std::unique_ptr<engine::ScriptBridge> bridge);

    // Releases the bridge for `view`, e.g. when it is being destroyed.
    void releaseFor(engine::WebView& view);
    bool hasBridgeFor(engine::WebView& view) const { return m_bridges.count(&view) > 0; }

    // The shim source, installed once per view. It defines browser.* and the
    // entry point ContentScriptInjector uses to declare the running extension.
    QString shim() const;

    // Installs the per-extension identity that the injector emits ahead of each
    // extension's own scripts.
    QString identityFor(const QString& extensionId) const;

    static bool methodExists(const QString& method);

private:
    // Each handler receives the params object and returns a JSON value.
    using Handler = std::function<QJsonValue(const QJsonObject& params)>;

    void registerAll(engine::ScriptBridge& bridge, const QVariant& viewHandle);
    // `requiredPermission` gates the call: when set, the calling extension must
    // hold it or the promise rejects. Empty means the call needs no permission.
    void registerHandler(engine::ScriptBridge& bridge, const QString& method,
        const QString& requiredPermission, Handler handler);
    void registerHandler(engine::ScriptBridge& bridge, const QString& method, Handler handler);
    void registerUnavailable(engine::ScriptBridge& bridge, const QString& method,
        const QString& reason);

    // Runtime identity. The extension id comes from the calling script, not
    // from server-side state, because one view can host scripts from several
    // extensions.
    QString extensionIdFor(const QJsonObject& params) const;

    // runtime
    QJsonValue runtimeGetUrl(const QString& id, const QJsonObject& params) const;
    QJsonValue runtimeGetManifest(const QString& id) const;

    // tabs
    QJsonValue tabsQuery(const QJsonObject& params) const;
    QJsonValue tabsGet(int id) const;
    QJsonValue tabsGetCurrent() const;
    QJsonValue tabsCreate(const QJsonObject& params);
    QJsonValue tabsUpdate(const QJsonObject& params);
    QJsonValue tabsRemove(const QJsonObject& params);
    QJsonValue tabsReload(int id);
    QJsonValue tabsGoBack(int id);
    QJsonValue tabsGoForward(int id);

    // windows
    QJsonValue windowsGetAll() const;
    QJsonValue windowsGetCurrent() const;

    // storage.local
    QJsonValue storageGet(const QString& id, const QJsonObject& params) const;
    QJsonValue storageSet(const QString& id, const QJsonObject& params);
    QJsonValue storageRemove(const QString& id, const QJsonObject& params);
    QJsonValue storageClear(const QString& id);

    // i18n
    QJsonValue i18nGetMessage(const QString& id, const QJsonObject& params) const;
    QJsonValue i18nGetUiLanguage() const;

    // scripting
    QJsonValue scriptingExecuteScript(const QString& id, const QJsonObject& params);

    ExtensionRegistry& m_registry;
    ExtensionApiHost* m_host { nullptr };
    // The view this API serves, used to attribute calls to a tab.
    QVariant m_viewHandle;
    // One bridge per attached view; the bridge dispatches every method below.
    std::map<engine::WebView*, std::unique_ptr<engine::ScriptBridge>> m_bridges;
    // Held for a context with no view, e.g. an options page.
    std::unique_ptr<engine::ScriptBridge> m_detachedBridge;
};

} // namespace iridium::extensions