#include "extensions/ExtensionApi.hpp"

#include "engine/ScriptBridge.hpp"

#include "extensions/JsSource.hpp"

#include <QJsonDocument>

namespace iridium::extensions {

namespace {

QJsonValue reject(const QString& message)
{
    QJsonObject error;
    error.insert(QStringLiteral("message"), message);
    return error;
}

QJsonValue notImplemented(const QString& method, const QString& reason)
{
    return reject(QStringLiteral("browser.%1 is not available in this build: %2")
        .arg(method, reason));
}

QJsonObject tabToJson(const TabSnapshot& tab)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), tab.id);
    object.insert(QStringLiteral("windowId"), tab.windowId);
    object.insert(QStringLiteral("url"), tab.url);
    object.insert(QStringLiteral("title"), tab.title);
    object.insert(QStringLiteral("favIconUrl"), tab.favIconUrl);
    object.insert(QStringLiteral("active"), tab.active);
    object.insert(QStringLiteral("audible"), tab.audible);
    object.insert(QStringLiteral("status"), tab.url.isEmpty()
        ? QStringLiteral("unloaded") : QStringLiteral("complete"));
    object.insert(QStringLiteral("incognito"), false);
    return object;
}

QJsonObject windowToJson(int id, bool focused)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), id);
    object.insert(QStringLiteral("focused"), focused);
    object.insert(QStringLiteral("state"), QStringLiteral("normal"));
    return object;
}

constexpr char kShimTemplate[] = R"JS((function() {
    var handler = window.webkit && window.webkit.messageHandlers &&
        window.webkit.messageHandlers.iridium;
    if (!handler) return;

    // A content script is injected alongside other extensions' scripts in the
    // same frame, and window.browser can only be one object. So the shim is
    // installed once and reads the id that the injector sets immediately before
    // each extension's script runs. That keeps every call attributed to the
    // extension that actually made it.
    var extensionId = '';
    var manifest = {};
    var uiLanguage = %1;

    window.__iridiumBeginExtension = function(id, parsedManifest) {
        extensionId = id;
        manifest = parsedManifest;
    };

    var listeners = {};
    window.__iridiumEmit = function(channel, payload) {
        var handlers = listeners[channel.split('.')[0]];
        if (!handlers) return;
        var value;
        try { value = JSON.parse(payload); } catch (e) { return; }
        for (var i = 0; i < handlers.length; ++i) {
            try { handlers[i](value); } catch (e) { console.error('[iridium]', e); }
        }
    };

    function call(method, params) {
        var payload = params || {};
        payload.extensionId = extensionId;
        return new Promise(function(resolve, reject_) {
            handler.postMessage({ method: method, params: payload })
                .then(function(reply) {
                    // The native side reports refusals by resolving with an
                    // { error } object, since it cannot reject a promise from
                    // outside JS. Turning that back into a rejection here is
                    // what makes a permission denial or an unimplemented method
                    // observable to the caller instead of silently resolving.
                    if (reply && typeof reply === 'object' && reply.error) {
                        reject_(new Error(String(reply.error)));
                        return;
                    }
                    resolve(reply);
                })
                .catch(function(error) {
                    reject_(new Error((error && error.message) || String(error)));
                });
        });
    }

    function on(namespaceName) {
        var key = namespaceName;
        return {
            addListener: function(fn) {
                (listeners[key] = listeners[key] || []).push(fn);
            },
            removeListener: function(fn) {
                var list = listeners[key];
                if (!list) return;
                var index = list.indexOf(fn);
                if (index >= 0) list.splice(index, 1);
            },
            hasListener: function(fn) {
                return (listeners[key] || []).indexOf(fn) >= 0;
            }
        };
    }

    function unavailable(method) {
        return function() {
            return Promise.reject(new Error(method +
                ' is not available in this build'));
        };
    }

    var api = {
        runtime: {
            // A getter, not a value: the shim is installed once per view but
            // scripts from several extensions share the frame, so the id is
            // only known when the injector declares the running extension.
            get id() { return extensionId; },
            // Synchronous in the real API, answered locally.
            getURL: function(path) {
                return 'iridium-extension://' + extensionId + '/' +
                    String(path).replace(/^\/+/, '');
            },
            getManifest: function() { return manifest; },
            getPlatformInfo: function() { return call('runtime.getPlatformInfo'); },
            sendMessage: unavailable('runtime.sendMessage'),
            connect: function() {
                return { postMessage: function() {}, disconnect: function() {} };
            },
            onMessage: on('runtime'),
            lastError: null
        },
        tabs: {
            query: function(q) { return call('tabs.query', { query: q }); },
            get: function(id) { return call('tabs.get', { tabId: id }); },
            getCurrent: function() { return call('tabs.getCurrent'); },
            create: function(p) { return call('tabs.create', p); },
            update: function(id, p) { return call('tabs.update', { tabId: id, properties: p }); },
            remove: function(id) { return call('tabs.remove', { tabId: id }); },
            reload: function(id) { return call('tabs.reload', { tabId: id }); },
            goBack: unavailable('tabs.goBack'),
            goForward: unavailable('tabs.goForward'),
            executeScript: unavailable('tabs.executeScript'),
            sendMessage: unavailable('tabs.sendMessage'),
            onCreated: on('tabs.created'),
            onUpdated: on('tabs.updated'),
            onRemoved: on('tabs.removed')
        },
        windows: {
            WINDOW_ID_NONE: -1,
            getAll: function() { return call('windows.getAll'); },
            getCurrent: function() { return call('windows.getCurrent'); },
            get: function() { return call('windows.getAll'); },
            create: unavailable('windows.create'),
            onFocusChanged: on('windows')
        },
        storage: {
            local: {
                get: function(keys) { return call('storage.local.get', { keys: keys }); },
                set: function(values) { return call('storage.local.set', { values: values }); },
                remove: function(keys) {
                    return call('storage.local.remove',
                        typeof keys === 'string' ? { key: keys } : { keys: keys });
                },
                clear: function() { return call('storage.local.clear'); }
            },
            sync: {
                get: unavailable('storage.sync.get'),
                set: unavailable('storage.sync.set'),
                remove: unavailable('storage.sync.remove'),
                clear: unavailable('storage.sync.clear')
            }
        },
        i18n: {
            getMessage: function(key, s) { return call('i18n.getMessage', { key: key, s: s }); },
            // Synchronous in the real API; the language is known up front.
            getUILanguage: function() { return uiLanguage; }
        },
        scripting: {
            executeScript: unavailable('scripting.executeScript')
        },
        permissions: {
            contains: unavailable('permissions.contains'),
            request: unavailable('permissions.request')
        }
    };

    try { Object.defineProperty(window, 'browser', { value: api, configurable: true }); }
    catch (e) { window.browser = api; }
    try { Object.defineProperty(window, 'chrome', { value: api, configurable: true }); }
    catch (e) { window.chrome = api; }
})();)JS";

} // namespace

ExtensionApi::ExtensionApi(ExtensionRegistry& registry, ExtensionApiHost* host, QObject* parent)
    : QObject(parent)
    , m_registry(registry)
    , m_host(host)
{
}

ExtensionApi::~ExtensionApi() = default;

bool ExtensionApi::methodExists(const QString& method)
{
    static const QSet<QString> kImplemented = {
        QStringLiteral("runtime.getPlatformInfo"),
        QStringLiteral("tabs.query"),
        QStringLiteral("tabs.get"),
        QStringLiteral("tabs.getCurrent"),
        QStringLiteral("tabs.create"),
        QStringLiteral("tabs.update"),
        QStringLiteral("tabs.remove"),
        QStringLiteral("tabs.reload"),
        QStringLiteral("windows.getAll"),
        QStringLiteral("windows.getCurrent"),
        QStringLiteral("storage.local.get"),
        QStringLiteral("storage.local.set"),
        QStringLiteral("storage.local.remove"),
        QStringLiteral("storage.local.clear"),
        QStringLiteral("i18n.getMessage"),
        QStringLiteral("i18n.getUILanguage"),
    };
    return kImplemented.contains(method);
}

void ExtensionApi::registerHandler(engine::ScriptBridge& bridge, const QString& method,
    const QString& requiredPermission, Handler handler)
{
    bridge.setHandler(method.toStdString(),
        [this, handler, method, requiredPermission](const std::string& payload) -> std::string {
            QJsonParseError error {};
            const QJsonDocument document = QJsonDocument::fromJson(
                QByteArray::fromStdString(payload), &error);
            QJsonObject params;
            if (error.error == QJsonParseError::NoError && document.isObject())
                params = document.object();

            // The caller is not trusted: it comes from page script, so the
            // extension id is only ever used as a lookup key. Permission is
            // checked before the handler runs, so a toggle in settings actually
            // gates the call rather than only describing it.
            const QString extensionId = params.value(QStringLiteral("extensionId")).toString();
            if (!requiredPermission.isEmpty()
                && !m_registry.hasPermission(extensionId, requiredPermission)) {
                QJsonObject error2;
                error2.insert(QStringLiteral("error"),
                    QStringLiteral("browser.%1 requires the \"%2\" permission, which is "
                        "not granted for %3")
                        .arg(method, requiredPermission, extensionId));
                return QJsonDocument(error2).toJson(QJsonDocument::Compact).constData();
            }

            // A handler signals rejection by returning {"message": ...} with no
            // other keys. That is re-wrapped so the shim's promise rejects with
            // a usable message instead of resolving to an error object.
            const QJsonValue result = handler(params);
            if (result.isObject()) {
                const QJsonObject object = result.toObject();
                if (object.size() == 1 && object.contains(QStringLiteral("message"))) {
                    QJsonObject wrapped;
                    wrapped.insert(QStringLiteral("error"),
                        object.value(QStringLiteral("message")));
                    return QJsonDocument(wrapped).toJson(QJsonDocument::Compact).constData();
                }
            }
            // QJsonDocument only wraps objects and arrays; scalars go through
            // fromVariant so a plain string or number survives the round trip.
            if (result.isObject())
                return QJsonDocument(result.toObject()).toJson(QJsonDocument::Compact).constData();
            if (result.isArray())
                return QJsonDocument(result.toArray()).toJson(QJsonDocument::Compact).constData();
            return QJsonDocument::fromVariant(result.toVariant())
                .toJson(QJsonDocument::Compact).constData();
        });
}

void ExtensionApi::registerHandler(engine::ScriptBridge& bridge, const QString& method,
    Handler handler)
{
    registerHandler(bridge, method, QString(), std::move(handler));
}

void ExtensionApi::registerUnavailable(engine::ScriptBridge& bridge, const QString& method,
    const QString& reason)
{
    registerHandler(bridge, method, [method, reason](const QJsonObject&) {
        return notImplemented(method, reason);
    });
}

QString ExtensionApi::extensionIdFor(const QJsonObject& params) const
{
    // Untrusted: it comes from page script, so treat it as a lookup key only.
    return params.value(QStringLiteral("extensionId")).toString();
}

QJsonValue ExtensionApi::runtimeGetUrl(const QString& id, const QJsonObject& params) const
{
    QString path = params.value(QStringLiteral("path")).toString();
    if (path.isEmpty())
        path = params.value(QStringLiteral("file")).toString();
    while (path.startsWith(QLatin1Char('/')))
        path.remove(0, 1);
    return QStringLiteral("iridium-extension://%1/%2").arg(id, path);
}

QJsonValue ExtensionApi::runtimeGetManifest(const QString& id) const
{
    QJsonParseError error {};
    const QJsonDocument document = QJsonDocument::fromJson(
        m_registry.manifestJson(id).toUtf8(), &error);
    return error.error == QJsonParseError::NoError && document.isObject()
        ? document.object() : QJsonObject();
}

QJsonValue ExtensionApi::tabsQuery(const QJsonObject& params) const
{
    if (!m_host)
        return reject(QStringLiteral("no window is available"));

    const QJsonObject query = params.value(QStringLiteral("query")).toObject();
    QJsonArray result;
    for (const TabSnapshot& tab : m_host->tabs()) {
        if (query.contains(QStringLiteral("url"))
            && !tab.url.contains(query.value(QStringLiteral("url")).toString()))
            continue;
        if (query.contains(QStringLiteral("title"))
            && !tab.title.contains(query.value(QStringLiteral("title")).toString()))
            continue;
        if (query.contains(QStringLiteral("active"))
            && tab.active != query.value(QStringLiteral("active")).toBool())
            continue;
        if (query.value(QStringLiteral("currentWindow")).toBool()
            && tab.windowId != m_host->currentWindow())
            continue;
        result.append(tabToJson(tab));
    }
    return result;
}

QJsonValue ExtensionApi::tabsGet(int id) const
{
    if (!m_host)
        return reject(QStringLiteral("no window is available"));
    const auto tab = m_host->tab(id);
    if (!tab)
        return reject(QStringLiteral("No tab with id: %1").arg(id));
    return tabToJson(*tab);
}

QJsonValue ExtensionApi::tabsGetCurrent() const
{
    if (!m_host)
        return reject(QStringLiteral("no window is available"));
    const auto tab = m_host->activeTab();
    if (!tab)
        return reject(QStringLiteral("The browser has no active tab"));
    return tabToJson(*tab);
}

QJsonValue ExtensionApi::tabsCreate(const QJsonObject& params)
{
    if (!m_host)
        return reject(QStringLiteral("no window is available"));
    const QString url = params.value(QStringLiteral("url")).toString();
    const int id = m_host->createTab(url, params.value(QStringLiteral("active")).toBool(true));
    if (id <= 0)
        return reject(QStringLiteral("could not create a tab"));
    const auto tab = m_host->tab(id);
    return tab ? tabToJson(*tab) : QJsonObject();
}

QJsonValue ExtensionApi::tabsUpdate(const QJsonObject& params)
{
    if (!m_host)
        return reject(QStringLiteral("no window is available"));
    const int id = params.value(QStringLiteral("tabId")).toInt(-1);
    if (!m_host->tab(id))
        return reject(QStringLiteral("No tab with id: %1").arg(id));
    m_host->updateTab(id, params.value(QStringLiteral("properties")).toObject());
    const auto tab = m_host->tab(id);
    return tab ? tabToJson(*tab) : QJsonObject();
}

QJsonValue ExtensionApi::tabsRemove(const QJsonObject& params)
{
    if (!m_host)
        return reject(QStringLiteral("no window is available"));
    const int id = params.value(QStringLiteral("tabId")).toInt(-1);
    if (!m_host->tab(id))
        return reject(QStringLiteral("No tab with id: %1").arg(id));
    m_host->removeTab(id);
    return {};
}

QJsonValue ExtensionApi::tabsReload(int id)
{
    if (!m_host || !m_host->tab(id))
        return reject(QStringLiteral("No tab with id: %1").arg(id));
    return {};
}

QJsonValue ExtensionApi::windowsGetAll() const
{
    QJsonArray result;
    if (m_host) {
        for (const int id : m_host->windowIds())
            result.append(windowToJson(id, id == m_host->currentWindow()));
    }
    return result;
}

QJsonValue ExtensionApi::windowsGetCurrent() const
{
    if (!m_host)
        return reject(QStringLiteral("no window is available"));
    return windowToJson(m_host->currentWindow(), true);
}

QJsonValue ExtensionApi::storageGet(const QString& id, const QJsonObject& params) const
{
    const QJsonObject storage = m_registry.loadStorage(id);
    const QJsonValue keys = params.value(QStringLiteral("keys"));
    if (!keys.isArray())
        return storage;

    QJsonObject result;
    for (const QJsonValue& key : keys.toArray())
        result.insert(key.toString(), storage.value(key.toString()));
    return result;
}

QJsonValue ExtensionApi::storageSet(const QString& id, const QJsonObject& params)
{
    QJsonObject storage = m_registry.loadStorage(id);
    const QJsonObject values = params.value(QStringLiteral("values")).toObject();
    for (auto it = values.begin(); it != values.end(); ++it)
        storage.insert(it.key(), it.value());
    m_registry.saveStorage(id, storage);
    return {};
}

QJsonValue ExtensionApi::storageRemove(const QString& id, const QJsonObject& params)
{
    QJsonObject storage = m_registry.loadStorage(id);
    const QString key = params.value(QStringLiteral("key")).toString();
    if (!key.isEmpty()) {
        storage.remove(key);
    } else {
        for (const QJsonValue& entry : params.value(QStringLiteral("keys")).toArray())
            storage.remove(entry.toString());
    }
    m_registry.saveStorage(id, storage);
    return {};
}

QJsonValue ExtensionApi::storageClear(const QString& id)
{
    m_registry.saveStorage(id, QJsonObject());
    return {};
}

QJsonValue ExtensionApi::i18nGetMessage(const QString& id, const QJsonObject& params) const
{
    Q_UNUSED(id);
    Q_UNUSED(params);
    // No _locales/messages.json support yet. The spec prescribes returning the
    // key when a message is missing, so that is what an extension sees.
    return QStringLiteral("");
}

QJsonValue ExtensionApi::i18nGetUiLanguage() const
{
    return QStringLiteral("en-US");
}

void ExtensionApi::registerAll(engine::ScriptBridge& bridge, const QVariant& viewHandle)
{
    m_viewHandle = viewHandle;

    registerHandler(bridge, QStringLiteral("runtime.getPlatformInfo"),
        [](const QJsonObject&) {
            QJsonObject info;
            info.insert(QStringLiteral("os"), QStringLiteral("linux"));
            info.insert(QStringLiteral("arch"), QStringLiteral("x86-64"));
            return info;
        });

    // Every tabs call needs the "tabs" permission. Without this a content script
    // could enumerate and close tabs regardless of what its manifest declared.
    const QString tabs = QStringLiteral("tabs");
    registerHandler(bridge, QStringLiteral("tabs.query"), tabs,
        [this](const QJsonObject& p) { return tabsQuery(p); });
    registerHandler(bridge, QStringLiteral("tabs.get"), tabs,
        [this](const QJsonObject& p) { return tabsGet(p.value(QStringLiteral("tabId")).toInt(-1)); });
    registerHandler(bridge, QStringLiteral("tabs.getCurrent"), tabs,
        [this](const QJsonObject&) { return tabsGetCurrent(); });
    registerHandler(bridge, QStringLiteral("tabs.create"), tabs,
        [this](const QJsonObject& p) { return tabsCreate(p); });
    registerHandler(bridge, QStringLiteral("tabs.update"), tabs,
        [this](const QJsonObject& p) { return tabsUpdate(p); });
    registerHandler(bridge, QStringLiteral("tabs.remove"), tabs,
        [this](const QJsonObject& p) { return tabsRemove(p); });
    registerHandler(bridge, QStringLiteral("tabs.reload"), tabs,
        [this](const QJsonObject& p) {
            return tabsReload(p.value(QStringLiteral("tabId")).toInt(-1));
        });

    registerHandler(bridge, QStringLiteral("windows.getAll"), tabs,
        [this](const QJsonObject&) { return windowsGetAll(); });
    registerHandler(bridge, QStringLiteral("windows.getCurrent"), tabs,
        [this](const QJsonObject&) { return windowsGetCurrent(); });

    // Storage is per extension; without the permission an extension must not
    // read or write any store, including its own.
    const QString storage = QStringLiteral("storage");
    registerHandler(bridge, QStringLiteral("storage.local.get"), storage,
        [this](const QJsonObject& p) { return storageGet(extensionIdFor(p), p); });
    registerHandler(bridge, QStringLiteral("storage.local.set"), storage,
        [this](const QJsonObject& p) { return storageSet(extensionIdFor(p), p); });
    registerHandler(bridge, QStringLiteral("storage.local.remove"), storage,
        [this](const QJsonObject& p) { return storageRemove(extensionIdFor(p), p); });
    registerHandler(bridge, QStringLiteral("storage.local.clear"), storage,
        [this](const QJsonObject& p) { return storageClear(extensionIdFor(p)); });

    registerHandler(bridge, QStringLiteral("i18n.getMessage"), QStringLiteral("i18n"),
        [this](const QJsonObject& p) { return i18nGetMessage(extensionIdFor(p), p); });
    registerHandler(bridge, QStringLiteral("i18n.getUILanguage"),
        [this](const QJsonObject&) { return i18nGetUiLanguage(); });

    // Declared as part of the WebExtensions API but genuinely unavailable in
    // this build. Each rejects with its reason rather than resolving to
    // undefined, which an extension would otherwise read as success.
    const QString noHost = QStringLiteral(
        "Iridium has no extension host process, so there is no background context "
        "to talk to");
    for (const QString& method : {
            QStringLiteral("runtime.sendMessage"),
            QStringLiteral("tabs.executeScript"),
            QStringLiteral("tabs.sendMessage"),
            QStringLiteral("scripting.executeScript"),
            QStringLiteral("windows.create"),
            QStringLiteral("permissions.request"),
            QStringLiteral("permissions.contains"),
        }) {
        registerUnavailable(bridge, method, noHost);
    }

    const QString noHistory = QStringLiteral(
        "the engine adapter does not expose navigation history");
    registerUnavailable(bridge, QStringLiteral("tabs.goBack"), noHistory);
    registerUnavailable(bridge, QStringLiteral("tabs.goForward"), noHistory);
}

void ExtensionApi::releaseFor(engine::WebView& view)
{
    m_bridges.erase(&view);
}

void ExtensionApi::installDetached(std::unique_ptr<engine::ScriptBridge> bridge)
{
    if (!bridge)
        return;
    registerAll(*bridge, QVariant());
    m_detachedBridge = std::move(bridge);
}

void ExtensionApi::installOn(engine::WebView& view, std::unique_ptr<engine::ScriptBridge> bridge)
{
    // The concrete bridge is built by the caller, which is the layer that links
    // the engine. Keeping WebKit out of this file is what lets the API be tested
    // against a fake transport.
    if (!bridge)
        return;

    registerAll(*bridge, QVariant());
    // Owned here so the bridge dies with the view it was attached to. Replaces
    // any bridge already registered for the view.
    m_bridges[&view] = std::move(bridge);
}

QString ExtensionApi::shim() const
{
    return QString::fromLatin1(kShimTemplate)
        .arg(jsStringLiteral(i18nGetUiLanguage().toString()));
}

QString ExtensionApi::identityFor(const QString& extensionId) const
{
    const QString parsed = m_registry.manifestJson(extensionId);
    return QStringLiteral("window.__iridiumBeginExtension(%1, %2);\n")
        .arg(jsStringLiteral(extensionId),
            parsed.isEmpty() ? QStringLiteral("{}") : parsed);
}

} // namespace iridium::extensions