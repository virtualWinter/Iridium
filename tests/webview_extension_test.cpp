// End-to-end check of the extensions layer against a real page: a content
// script from an on-disk extension runs, matches the right URLs, and can reach
// the browser.* API through the real script-message bridge.

#include "engine/WebView.hpp"
#include "engine/webkit/WebKitView.hpp"
#include "extensions/ExtensionApi.hpp"
#include "extensions/ExtensionHost.hpp"
#include "extensions/ExtensionPaths.hpp"
#include "extensions/ExtensionRegistry.hpp"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using iridium::engine::webkit::WebKitView;
using iridium::extensions::ExtensionApiHost;
using iridium::extensions::ExtensionHost;
using iridium::extensions::ExtensionRegistry;
using iridium::extensions::TabSnapshot;

namespace {

constexpr int kTimeoutMs = 30000;

int g_failures = 0;

void check(bool condition, const char* what)
{
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

void checkContains(const std::string& haystack, const std::string& needle, const char* what)
{
    if (haystack.find(needle) != std::string::npos)
        return;
    std::printf("FAIL: %s\n  expected: %s\n  in: %s\n", what, needle.c_str(),
        haystack.c_str());
    ++g_failures;
}

// Written to disk and loaded over file:// because <all_urls> deliberately does
// not cover data: URLs, and the test wants a realistic page load.
constexpr char kPage[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>start</title></head>
<body></body></html>)HTML";

bool writeFile(const QString& path, const QByteArray& contents)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    return file.write(contents) == contents.size();
}

bool pumpUntil(const std::function<bool()>& done, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (done())
            return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return done();
}

std::string localUri(const QString& path)
{
    return QUrl::fromLocalFile(path).toString().toStdString();
}

// A host with no real window: enough for the API layer, which is exercised
// through the bridge rather than through tab widgets.
class NullHost final : public ExtensionApiHost {
public:
    std::vector<TabSnapshot> tabs() const override { return {}; }
    std::optional<TabSnapshot> activeTab() const override { return std::nullopt; }
    std::optional<TabSnapshot> tab(int) const override { return std::nullopt; }
    int createTab(const QString&, bool) override { return -1; }
    void removeTab(int) override {}
    void updateTab(int, const QJsonObject&) override {}
    void activateTab(int) override {}
    std::vector<int> windowIds() const override { return { 1 }; }
    int currentWindow() const override { return 1; }
};

} // namespace

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    QTemporaryDir xdg;
    QTemporaryDir profile;
    QTemporaryDir root;
    if (!xdg.isValid() || !profile.isValid() || !root.isValid()) {
        std::printf("FAIL: could not create temp dirs\n");
        return 1;
    }
    qputenv("XDG_DATA_HOME", xdg.path().toUtf8());

    const QByteArray probeScript = R"JS(window.addEventListener('load', function() {
        var out = ['injected'];
        out.push('id=' + browser.runtime.id);
        out.push('name=' + browser.runtime.getManifest().name);
        out.push('url=' + browser.runtime.getURL('bg.js'));

        // allSettled throughout: Promise.all would short-circuit on the first
        // rejection and hide whether the working calls actually succeeded.
        // Results come back in the title, which is the only channel the test
        // can observe from inside the page.
        browser.storage.local.set({ token: 42 }).then(function() {
            return browser.storage.local.get(['token']);
        }).then(function(stored) {
            out.push('stored=' + JSON.stringify(stored));
            return browser.i18n.getUILanguage();
        }).then(function(language) {
            out.push('lang=' + language);
            // Unavailable methods must reject rather than resolve undefined.
            return Promise.allSettled([
                browser.runtime.sendMessage({}),
                browser.storage.sync.get(null)
            ]);
        }).then(function(settled) {
            out.push('sendMsgRejected=' + settled[0].status);
            out.push('syncRejected=' + settled[1].status);
            // "tabs" is optional and ungranted, so this must be refused. If the
            // gate were missing it would resolve, which is the failure this
            // assertion exists to catch.
            return browser.tabs.query({}).then(function() {
                out.push('tabs=ALLOWED');
            }, function(error) {
                out.push('tabs=REFUSED');
                out.push('tabsWhy=' + (error && error.message));
            });
        }).then(function() {
            document.title = out.join('|');
        }, function(error) {
            out.push('unexpected=' + (error && error.message));
            document.title = out.join('|');
        });
    });
)JS";

    // "storage" is granted in the manifest; "tabs" is optional so the permission
    // gate can be observed end to end below.
const QString extDir = root.path() + QStringLiteral("/probe");
    if (!writeFile(extDir + QStringLiteral("/manifest.json"),
            R"({"manifest_version":3,"name":"Probe","version":"1.0",
                "permissions":["storage"],
                "optional_permissions":["tabs"],
                "content_scripts":[{"matches":["<all_urls>"],"js":["content.js"]}]})")
        || !writeFile(extDir + QStringLiteral("/content.js"), probeScript)) {
        std::printf("FAIL: could not write extension fixture\n");
        return 1;
    }

    const QString pagePath = root.path() + QStringLiteral("/page.html");
    if (!writeFile(pagePath, QByteArray(kPage, sizeof(kPage) - 1))) {
        std::printf("FAIL: could not write page fixture\n");
        return 1;
    }

    ExtensionRegistry registry(profile.path());
    registry.loadFrom(root.path());
    check(registry.extensions().size() == 1, "one extension discovered");
    check(registry.setEnabled(QStringLiteral("probe"), true), "extension enabled");

    NullHost host;
    ExtensionHost extensions(registry, &host);

    WebKitView view;
    view.resize(640, 480);
    view.show();

    std::string title;
    view.setTitleChangedHandler([&title](const std::string& value) { title = value; });

    // Attach before loading, then let the host inject for this URL.
    extensions.attachView(&view);
    view.load(localUri(pagePath));
    // Re-inject after the URL is known, which is what the UI does on navigation.
    extensions.refreshView(&view);

    // The page may already have been rewritten by the content script by now, so
    // accept either the untouched title or a finished report.
    const bool loaded = pumpUntil([&title] {
        return title == "start" || title.find("sendMsgRejected=") != std::string::npos;
    }, kTimeoutMs);
    if (!loaded) {
        std::printf("FAIL: page did not load (title='%s')\n", title.c_str());
        return 1;
    }

    const bool ran = pumpUntil([&title] { return title.rfind("injected", 0) == 0; },
        kTimeoutMs);
    if (!ran) {
        std::printf("FAIL: content script did not run (title='%s')\n", title.c_str());
        return 1;
    }

    const bool settled = pumpUntil(
        [&title] { return title.find("tabsWhy=") != std::string::npos; }, kTimeoutMs);
    if (!settled) {
        std::printf("FAIL: API calls never settled (title='%s')\n", title.c_str());
        return 1;
    }

    std::printf("      title: %s\n", title.c_str());

    checkContains(title, "injected", "content script ran");
    checkContains(title, "id=probe", "runtime.id is the running extension");
    checkContains(title, "name=Probe", "getManifest returns the manifest");
    checkContains(title, "url=iridium-extension://probe/bg.js", "getURL builds an extension URL");
    checkContains(title, "stored={\"token\":42}", "storage.local round-trips");
    checkContains(title, "lang=en-US", "i18n.getUILanguage answers");
    checkContains(title, "sendMsgRejected=rejected",
        "runtime.sendMessage rejects, not resolves undefined");
    checkContains(title, "syncRejected=rejected", "storage.sync rejects");
    checkContains(title, "tabs=REFUSED",
        "a call needing an ungranted optional permission is refused");
    checkContains(title, "tabsWhy=browser.tabs.query requires the \"tabs\" permission",
        "the refusal names the method and the permission");

    // Detaching must stop injection and release the bridge.
    extensions.detachView(&view);
    title.clear();
    view.load(localUri(pagePath));
    pumpUntil([&title] { return title == "start"; }, kTimeoutMs);
    check(title.find("injected") == std::string::npos,
        "no scripts are injected after detach");

    if (g_failures == 0)
        std::printf("PASS: extensions work end to end through the engine\n");
    else
        std::printf("%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}