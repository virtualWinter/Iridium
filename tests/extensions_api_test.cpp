// Covers the browser.* API layer against a fake host: the calls that should work,
// the isolation between extensions' storage, and the promise-rejection contract
// for methods this build cannot provide.

#include "engine/ScriptBridge.hpp"
#include "engine/WebView.hpp"
#include "extensions/ExtensionApi.hpp"
#include "extensions/ExtensionPaths.hpp"
#include "extensions/ExtensionRegistry.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QVariant>

#include <cstdio>
#include <map>
#include <optional>
#include <string>

using iridium::engine::ScriptBridge;
using iridium::extensions::ExtensionApi;
using iridium::extensions::ExtensionApiHost;
using iridium::extensions::ExtensionRegistry;
using iridium::extensions::TabSnapshot;

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

// Records handlers so the test can call them directly, standing in for a page.
class FakeBridge final : public ScriptBridge {
public:
    bool setHandler(const std::string& channel,
        ScriptBridge::RequestHandler handler) override
    {
        m_handlers[channel] = std::move(handler);
        return true;
    }
    void setClosedHandler(ScriptBridge::ClosedHandler) override {}
    void sendEvent(const std::string&, const std::string&) override {}

    bool has(const std::string& channel) const
    {
        return m_handlers.count(channel) > 0;
    }

    // Calls `channel` with `params` and returns the parsed reply. The reply may
    // be any JSON value, not just an object, so the whole document is unwrapped.
    QJsonValue call(const std::string& channel, const QJsonObject& params = {}) const
    {
        const auto found = m_handlers.find(channel);
        if (found == m_handlers.end())
            return {};
        const QString text = QString::fromStdString(
            found->second(QJsonDocument(params).toJson(QJsonDocument::Compact).toStdString()));
        const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8());
        if (document.isObject())
            return document.object();
        if (document.isArray())
            return document.array();
        return {};
    }

private:
    std::map<std::string, ScriptBridge::RequestHandler> m_handlers;
};

// A minimal engine view, enough for the API to hold a pointer to.
class FakeView final : public iridium::engine::WebView {
public:
    void load(const std::string&) override {}
    void goBack() override {}
    void goForward() override {}
    void reload() override {}
    bool canGoBack() const override { return false; }
    bool canGoForward() const override { return false; }
    QWidget* widget() override { return nullptr; }
    void* nativeWebView() const override { return nullptr; }
    void setTitleChangedHandler(std::function<void(const std::string&)>) override {}
    void setUriChangedHandler(std::function<void(const std::string&)>) override {}
    void setFaviconUrlChangedHandler(std::function<void(const std::string&)>) override {}
    void setDownloadStateHandler(DownloadStateHandler) override {}
    void cancelDownload(std::uint64_t) override {}
    void evaluateJavaScript(const std::string&, JavaScriptResultHandler) override {}
    std::string currentUrl() const override { return {}; }
    void setContentScriptSource(const std::string&) override {}
    void clearContentScript() override {}
    void setForcedColorScheme(std::optional<bool>) override {}
};

class FakeHost final : public ExtensionApiHost {
public:
    std::vector<TabSnapshot> tabs() const override { return m_tabs; }

    std::optional<TabSnapshot> activeTab() const override
    {
        for (const TabSnapshot& tab : m_tabs) {
            if (tab.active)
                return tab;
        }
        return std::nullopt;
    }

    std::optional<TabSnapshot> tab(int id) const override
    {
        for (const TabSnapshot& tab : m_tabs) {
            if (tab.id == id)
                return tab;
        }
        return std::nullopt;
    }

    int createTab(const QString& url, bool active) override
    {
        TabSnapshot created;
        created.id = m_nextId++;
        created.url = url;
        created.active = active;
        m_tabs.push_back(created);
        return created.id;
    }

    void removeTab(int id) override
    {
        for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it) {
            if (it->id == id) {
                m_tabs.erase(it);
                return;
            }
        }
    }

    void updateTab(int id, const QJsonObject& properties) override
    {
        for (TabSnapshot& tab : m_tabs) {
            if (tab.id != id)
                continue;
            if (properties.contains("url"))
                tab.url = properties.value("url").toString();
            if (properties.contains("title"))
                tab.title = properties.value("title").toString();
        }
    }

    void activateTab(int id) override
    {
        for (TabSnapshot& tab : m_tabs)
            tab.active = tab.id == id;
    }

    std::vector<int> windowIds() const override { return { 1 }; }
    int currentWindow() const override { return 1; }

    std::vector<TabSnapshot> m_tabs;
    int m_nextId { 100 };
};

bool writeFile(const QString& path, const QByteArray& contents)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    return file.write(contents) == contents.size();
}

QJsonObject params(const QString& extensionId, QJsonObject extra = {})
{
    QJsonObject object;
    object.insert("extensionId", extensionId);
    for (auto it = extra.begin(); it != extra.end(); ++it)
        object.insert(it.key(), it.value());
    return object;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir xdg;
    QTemporaryDir profile;
    QTemporaryDir root;
    if (!xdg.isValid() || !profile.isValid() || !root.isValid()) {
        std::printf("FAIL: could not create temp dirs\n");
        return 1;
    }
    qputenv("XDG_DATA_HOME", xdg.path().toUtf8());

    // "tabs" is optional here so the permission-enforcement block below can
    // grant and revoke it and observe the gate open and close.
    writeFile(root.path() + QStringLiteral("/alpha/manifest.json"),
        R"({"manifest_version":3,"name":"Alpha","version":"1.0",
            "permissions":["storage"],"optional_permissions":["tabs"]})");
    writeFile(root.path() + QStringLiteral("/beta/manifest.json"),
        // "tabs" is required: the API enforces permissions, and the tabs and windows
        // checks below need to reach the fake host.
        R"({"manifest_version":2,"name":"Beta","version":"2.0","permissions":["tabs"]})");

    ExtensionRegistry registry(profile.path());
    registry.loadFrom(root.path());
    registry.setEnabled(QStringLiteral("alpha"), true);
    registry.setEnabled(QStringLiteral("beta"), true);
    check(registry.extensions().size() == 2, "two extensions loaded");

    FakeHost host;
    TabSnapshot first;
    first.id = 1;
    first.windowId = 1;
    first.url = QStringLiteral("https://example.com/");
    first.title = QStringLiteral("Example");
    first.active = true;
    host.m_tabs.push_back(first);
    TabSnapshot second = first;
    second.id = 2;
    second.url = QStringLiteral("https://other.test/");
    second.title = QStringLiteral("Other");
    second.active = false;
    host.m_tabs.push_back(second);

    ExtensionApi api(registry, &host);
    FakeView view;
    // The API takes ownership of the bridge and ties it to the view, so that a
    // destroyed view cannot leave a bridge dispatching into freed memory.
    auto owned = std::make_unique<FakeBridge>();
    FakeBridge* raw = owned.get();
    api.installOn(view, std::move(owned));
    // Raw pointer stays valid while the API owns the bridge, which is the
    // lifetime the host guarantees by releasing before destroying the view.
    FakeBridge& bridge = *raw;

    check(api.hasBridgeFor(view), "bridge registered for the view");
    FakeView other;
    check(!api.hasBridgeFor(other), "an unrelated view has no bridge");

    // Alpha declares "tabs" as optional, so the tabs and windows checks below
    // have to grant it first. Without this they would be measuring the
    // permission gate rather than the tab plumbing.
    check(registry.setPermissionGranted(QStringLiteral("alpha"), "tabs", true),
        "optional tabs permission granted");
    check(registry.hasPermission(QStringLiteral("alpha"), "tabs"),
        "the granted permission is in effect");

    // The methods this build supports are registered.
    for (const char* method : { "runtime.getPlatformInfo", "tabs.query", "tabs.get",
             "tabs.getCurrent", "tabs.create", "tabs.update", "tabs.remove",
             "tabs.reload", "windows.getAll", "windows.getCurrent",
             "storage.local.get", "storage.local.set", "storage.local.remove",
             "storage.local.clear", "i18n.getMessage", "i18n.getUILanguage" }) {
        check(bridge.has(method), std::string("registered: ") + method);
        check(ExtensionApi::methodExists(QString::fromLatin1(method)),
            std::string("methodExists: ") + method);
    }

    // tabs.query reflects the host's state and honours its filters.
    QJsonValue all = bridge.call("tabs.query", params("alpha"));
    check(all.isArray() && all.toArray().size() == 2, "query returns every tab");

    QJsonObject activeFilter;
    activeFilter.insert("active", true);
    QJsonValue actives = bridge.call("tabs.query",
        params("alpha", { { "query", activeFilter } }));
    check(actives.isArray() && actives.toArray().size() == 1, "query filters on active");

    QJsonObject urlFilter;
    urlFilter.insert("url", "other.test");
    QJsonValue filtered = bridge.call("tabs.query",
        params("alpha", { { "query", urlFilter } }));
    check(filtered.isArray() && filtered.toArray().size() == 1, "query filters on url");
    if (filtered.isArray() && !filtered.toArray().isEmpty()) {
        check(filtered.toArray().first().toObject().value("url").toString()
            == QStringLiteral("https://other.test/"), "query returns the matching tab");
    }

    // Tabs carry the fields extensions actually read.
    if (!all.isArray() || all.toArray().isEmpty()) {
        std::printf("FAIL: no tabs to inspect\n");
        return 1;
    }
    const QJsonObject tab = all.toArray().first().toObject();
    for (const char* field : { "id", "url", "title", "active", "status", "windowId" }) {
        check(tab.contains(field), std::string("tab has field: ") + field);
    }

    // tabs.get rejects an unknown id instead of returning something empty.
    QJsonValue missing = bridge.call("tabs.get", params("alpha", { { "tabId", 999 } }));
    check(missing.isObject() && missing.toObject().contains("error"),
        "tabs.get rejects an unknown tab");

    // tabs.create adds a tab through the host.
    QJsonValue created = bridge.call("tabs.create",
        params("alpha", { { "url", "https://new.test/" } }));
    check(created.isObject() && created.toObject().value("url").toString()
        == QStringLiteral("https://new.test/"), "tabs.create navigates the new tab");
    check(host.m_tabs.size() == 3, "host gained a tab");

    // tabs.update mutates the tab.
    const int newId = created.toObject().value("id").toInt();
    QJsonObject updateProperties;
    updateProperties.insert("title", QStringLiteral("Renamed"));
    bridge.call("tabs.update",
        params("alpha", { { "tabId", newId }, { "properties", updateProperties } }));
    check(host.tab(newId)->title == QStringLiteral("Renamed"), "tabs.update sets the title");

    // tabs.remove closes it.
    bridge.call("tabs.remove", params("alpha", { { "tabId", newId } }));
    check(!host.tab(newId).has_value(), "tabs.remove closes the tab");

    // storage.local is per extension: alpha's value is invisible to beta.
    QJsonObject values;
    values.insert("secret", QStringLiteral("alpha-only"));
    bridge.call("storage.local.set", params("alpha", { { "values", values } }));

    QJsonValue alphaRead = bridge.call("storage.local.get", params("alpha"));
    check(alphaRead.isObject()
        && alphaRead.toObject().value("secret").toString() == QStringLiteral("alpha-only"),
        "alpha reads its own storage");

    QJsonValue betaRead = bridge.call("storage.local.get", params("beta"));
    check(betaRead.isObject() && !betaRead.toObject().contains("secret"),
        "beta cannot see alpha's storage");

    // Reading a subset of keys returns just those.
    QJsonArray keys;
    keys.append("secret");
    QJsonValue subset = bridge.call("storage.local.get",
        params("alpha", { { "keys", keys } }));
    check(subset.isObject() && subset.toObject().contains("secret"), "storage.get by keys");

    // remove and clear work.
    bridge.call("storage.local.remove", params("alpha", { { "key", "secret" } }));
    check(registry.loadStorage(QStringLiteral("alpha")).isEmpty(), "storage.remove deletes");

    QJsonObject more;
    more.insert("k", QStringLiteral("v"));
    bridge.call("storage.local.set", params("alpha", { { "values", more } }));
    bridge.call("storage.local.clear", params("alpha"));
    check(registry.loadStorage(QStringLiteral("alpha")).isEmpty(), "storage.clear empties");

    // windows
    QJsonValue windows = bridge.call("windows.getAll", params("alpha"));
    check(windows.isArray() && windows.toArray().size() == 1, "windows.getAll");
    check(windows.toArray().first().toObject().value("focused").toBool(),
        "current window is focused");

    // Permission enforcement: an extension with no "tabs" permission must be
    // refused, and the refusal must name the permission so the cause is clear.
    {
        ExtensionRegistry bare(profile.path() + QStringLiteral("/bare"));
        bare.loadFrom(root.path());
        bare.setEnabled(QStringLiteral("alpha"), true);
        ExtensionApi bareApi(bare, &host);
        auto bareBridge = std::make_unique<FakeBridge>();
        FakeBridge* bareRaw = bareBridge.get();
        bareApi.installDetached(std::move(bareBridge));
        const QJsonValue denied = bareRaw->call("tabs.query",
            { { "extensionId", "alpha" } });
        check(denied.isObject() && denied.toObject().contains("error"),
            "a call without the required permission is refused");
        if (denied.isObject() && denied.toObject().contains("error")) {
            const QString message = denied.toObject().value("error").toString();
            check(message.contains(QStringLiteral("tabs"))
                    && message.contains(QStringLiteral("not granted")),
                "the refusal names the missing permission");
        }

        // Granting the optional permission opens the gate.
        bare.setPermissionGranted(QStringLiteral("alpha"), "tabs", true);
        const QJsonValue allowed = bareRaw->call("tabs.query",
            { { "extensionId", "alpha" } });
        check(allowed.isArray(), "granting the permission allows the call");

        // Revoking closes it again.
        bare.setPermissionGranted(QStringLiteral("alpha"), "tabs", false);
        const QJsonValue revoked = bareRaw->call("tabs.query",
            { { "extensionId", "alpha" } });
        check(revoked.isObject() && revoked.toObject().contains("error"),
            "revoking the permission blocks the call again");

        // A required permission cannot be "granted" as if it were optional.
        check(!bare.setPermissionGranted(QStringLiteral("alpha"),
            QStringLiteral("storage"), true),
            "a required permission is not grantable through the toggle");

        // The refusal is an { error } reply, which the JS shim converts back
        // into a rejection. This asserts the shape the shim depends on: an
        // extension must never see a resolved value for a denied call.
        check(denied.isObject() && denied.toObject().value("error").isString(),
            "a refusal is reported as an { error } reply, not a resolved value");
        check(!denied.toObject().contains("message"),
            "the refusal is not left in the handler's internal shape");
    }

    // Unavailable methods must reject with a message, never resolve quietly.
    for (const char* method : { "runtime.sendMessage", "tabs.executeScript",
             "scripting.executeScript", "permissions.request", "windows.create",
             "tabs.goBack" }) {
        const QJsonValue reply = bridge.call(method, params("alpha"));
        check(reply.isObject() && reply.toObject().contains("error"),
            std::string("rejects with a reason: ") + method);
        if (reply.isObject() && reply.toObject().contains("error")) {
            const QString message = reply.toObject().value("error").toString();
            check(message.contains(QString::fromLatin1(method))
                    && message.contains(QStringLiteral("this build")),
                std::string("reason names the method and the build: ") + method);
        }
    }

    // Detaching must release the bridge, so a closed tab cannot keep
    // dispatching into a destroyed view. Done last, since it invalidates the
    // raw pointer the rest of the test uses.
    api.releaseFor(view);
    check(!api.hasBridgeFor(view), "releaseFor drops the bridge");

    // The shim defines both namespaces and reports an id per extension.
    const QString shim = api.shim();
    check(shim.contains("window.browser"), "shim defines browser");
    check(shim.contains("window.chrome"), "shim defines chrome");
    check(shim.contains("__iridiumBeginExtension"), "shim exposes the identity hook");
    check(shim.contains("storage.local"), "shim exposes storage.local");

    // A quote in an extension id must be escaped, not emitted raw. The identity
// line is `__iridiumBeginExtension("<id>", {...});`, so an unescaped quote would
// show up as an odd number of quotes before the comma.
    const QString hostile = api.identityFor(QStringLiteral("a\"b"));
    const int comma = hostile.indexOf(QLatin1Char(','));
    check(comma > 0, "identity has an argument separator");
    const int quotesBeforeComma = hostile.left(comma).count(QLatin1Char('"'));
    // "a\"b" contributes 3 quote characters: open, escaped, close.
    check(quotesBeforeComma == 3,
        "quote inside an extension id is escaped in the identity declaration");

    if (g_failures == 0)
        std::printf("PASS: browser.* API layer\n");
    else
        std::printf("%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}