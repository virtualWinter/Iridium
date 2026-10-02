// Verifies the Iridium user agent and actual OS/CPU in JavaScript and HTTP,
// including origins for which WebKit normally substitutes a compatibility UA.
// A loopback proxy serves all fixtures; no request reaches those real sites.

#define QT_NO_KEYWORDS
#include "engine/webkit/WebKitView.hpp"

#include <wpe/webkit.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <sys/utsname.h>

#ifndef IRIDIUM_VERSION
#define IRIDIUM_VERSION "0.0.1"
#endif

namespace {

constexpr int kTimeoutMs = 30000;

constexpr char kPage[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>ua-start</title></head>
<body><script>document.title = 'ua:' + JSON.stringify({
    userAgent: navigator.userAgent, platform: navigator.platform,
    appVersion: navigator.appVersion, hardwareConcurrency: navigator.hardwareConcurrency
});</script></body></html>)HTML";

constexpr char kHttpPage[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>ua-start</title>
<link rel="stylesheet" href="/style.css"><script src="/script.js"></script>
<script>
const frameReport = new Promise(resolve => {
    addEventListener('message', event => {
        if (event.data.kind === 'frame') resolve(event.data);
    });
});
const workerReport = new Promise((resolve, reject) => {
    const worker = new Worker('/worker.js');
    worker.onmessage = event => { resolve(event.data); worker.terminate(); };
    worker.onerror = event => reject(new Error(event.message));
});
const fetchReport = fetch('/fetch-redirect').then(response => response.text());
addEventListener('load', async () => {
    try {
        const [frame, worker, fetch] = await Promise.all([frameReport, workerReport, fetchReport]);
        document.title = 'ua:' + JSON.stringify({
            document: {
                userAgent: navigator.userAgent, platform: navigator.platform,
                appVersion: navigator.appVersion, hardwareConcurrency: navigator.hardwareConcurrency
            },
            script: window.scriptUserAgent, frame, worker,
            workerFetch: worker.httpUserAgent, fetch
        });
    } catch (error) { document.title = 'ua-error:' + error; }
});
</script></head><body><iframe src="/frame"></iframe></body></html>)HTML";

class UserAgentServer final : public QTcpServer {
public:
    struct Request {
        QByteArray target;
        QString path;
        QByteArray userAgent;
    };

    UserAgentServer()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto* socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer = QByteArray()]() mutable {
                    buffer += socket->readAll();
                    if (!buffer.contains("\r\n\r\n"))
                        return;
                    const auto lines = buffer.left(buffer.indexOf("\r\n\r\n")).split('\n');
                    const QByteArray target = lines.value(0).split(' ').value(1);
                    const QString path = QUrl::fromEncoded(target).path();
                    QByteArray userAgent;
                    for (const auto& line : lines) {
                        const auto colon = line.indexOf(':');
                        if (colon >= 0 && line.left(colon).trimmed().toLower() == "user-agent")
                            userAgent = line.mid(colon + 1).trimmed();
                    }
                    requests.append({ target, path, userAgent });
                    respond(socket, path, userAgent);
                    buffer.clear();
                });
            }
        });
    }

    QList<Request> requests;

private:
    static void respond(QTcpSocket* socket, const QString& path, const QByteArray& userAgent)
    {
        QByteArray status = "200 OK";
        QByteArray type = "text/html; charset=utf-8";
        QByteArray headers;
        QByteArray body;
        if (path == QLatin1String("/redirect") || path == QLatin1String("/fetch-redirect")) {
            status = "302 Found";
            headers = path == QLatin1String("/redirect") ? "Location: /page\r\n" : "Location: /fetch\r\n";
        } else if (path == QLatin1String("/page")) {
            body = kHttpPage;
        } else if (path == QLatin1String("/frame")) {
            body = "<!doctype html><script>parent.postMessage({kind:'frame',"
                   "userAgent:navigator.userAgent,platform:navigator.platform,"
                   "appVersion:navigator.appVersion,hardwareConcurrency:navigator.hardwareConcurrency}, '*');</script>";
        } else if (path == QLatin1String("/script.js")) {
            type = "application/javascript";
            body = "window.scriptUserAgent = navigator.userAgent;";
        } else if (path == QLatin1String("/worker.js")) {
            type = "application/javascript";
            body = "fetch('/worker-fetch').then(r => r.text()).then(httpUserAgent => "
                   "postMessage({userAgent:navigator.userAgent,platform:navigator.platform,"
                   "appVersion:navigator.appVersion,hardwareConcurrency:navigator.hardwareConcurrency,httpUserAgent}));";
        } else if (path == QLatin1String("/style.css")) {
            type = "text/css";
            body = "body { color: black; }";
        } else if (path == QLatin1String("/fetch") || path == QLatin1String("/worker-fetch")) {
            type = "text/plain";
            body = userAgent;
        } else {
            status = "404 Not Found";
        }
        socket->write("HTTP/1.1 " + status + "\r\nContent-Type: " + type
            + "\r\nCache-Control: no-store\r\nConnection: close\r\n" + headers
            + "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
        socket->disconnectFromHost();
    }
};

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

} // namespace

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir environment;
    if (!environment.isValid()) {
        std::fprintf(stderr, "FAIL: could not create an isolated test environment\n");
        return 1;
    }
    qputenv("XDG_DATA_HOME", environment.path().toUtf8());
    qputenv("XDG_CACHE_HOME", environment.path().toUtf8());
    QApplication app(argc, argv);

    struct utsname system {};
    if (uname(&system) != 0) {
        std::fprintf(stderr, "FAIL: could not read the host OS and CPU architecture\n");
        return 1;
    }
    const QByteArray expectedPlatform = QByteArray(system.sysname) + ' ' + system.machine;
    const QByteArray expectedVersion = QByteArray(IRIDIUM_VERSION) + " (" + expectedPlatform + ')';
    const QByteArray expected = "Iridium/" + expectedVersion;

    UserAgentServer server;
    if (!server.listen(QHostAddress::LocalHost)) {
        std::fprintf(stderr, "FAIL: could not start the local HTTP fixture: %s\n", qPrintable(server.errorString()));
        return 1;
    }

    iridium::engine::webkit::WebKitView view;
    auto* nativeView = static_cast<WebKitWebView*>(view.nativeWebView());
    auto* session = webkit_web_view_get_network_session(nativeView);
    const QByteArray proxyUri = "http://127.0.0.1:" + QByteArray::number(server.serverPort());
    auto* proxy = webkit_network_proxy_settings_new(proxyUri.constData(), nullptr);
    webkit_network_session_set_proxy_settings(session, WEBKIT_NETWORK_PROXY_MODE_CUSTOM, proxy);
    webkit_network_proxy_settings_free(proxy);
    std::string title;
    view.setTitleChangedHandler([&title](const std::string& value) { title = value; });
    view.resize(640, 480);
    view.show();

    int failures = 0;
    const auto checkNavigator = [&](const QJsonObject& report, const std::string& context) {
        for (const auto& [field, value] : {
                 std::pair { "userAgent", expected }, std::pair { "platform", expectedPlatform },
                 std::pair { "appVersion", expectedVersion } }) {
            const QByteArray actual = report.value(QLatin1String(field)).toString().toUtf8();
            if (actual != value) {
                std::fprintf(stderr, "FAIL: %s navigator.%s: '%s' (expected '%s')\n",
                    context.c_str(), field, actual.constData(), value.constData());
                ++failures;
            }
        }
        // Keep WebKit's native privacy-limited CPU count rather than exposing
        // or inventing the host's exact logical-processor count.
        if (report.value(QStringLiteral("hardwareConcurrency")).toInt() < 1) {
            std::fprintf(stderr, "FAIL: %s did not expose a usable native hardwareConcurrency\n", context.c_str());
            ++failures;
        }
    };
    for (const char* origin : { "data:", "https://duckduckgo.com/", "https://accounts.google.com/",
             "https://drive.google.com/", "https://docs.google.com/", "https://www.paypal.com/" }) {
        title.clear();
        if (std::string(origin) == "data:")
            view.load("data:text/html;base64," + QByteArray(kPage, sizeof(kPage) - 1).toBase64().toStdString());
        else
            webkit_web_view_load_html(nativeView, kPage, origin);
        if (!pumpUntil([&title] { return title.rfind("ua:", 0) == 0; }, kTimeoutMs)) {
            std::fprintf(stderr, "FAIL: %s did not report the user agent\n", origin);
            ++failures;
            continue;
        }
        const auto report = QJsonDocument::fromJson(QByteArray::fromStdString(title.substr(3))).object();
        checkNavigator(report, origin);
        std::printf("%s navigator: %s\n", origin, title.substr(3).c_str());
    }

    const auto checkHttpPage = [&](const char* origin, bool reload) {
        title.clear();
        server.requests.clear();
        if (reload)
            view.reload();
        else
            view.load(std::string(origin) + "/redirect");
        if (!pumpUntil([&title] { return title.rfind("ua:", 0) == 0 || title.rfind("ua-error:", 0) == 0; }, kTimeoutMs)) {
            std::fprintf(stderr, "FAIL: %s HTTP page did not report (title=%s, requests=%lld)\n",
                origin, title.c_str(), static_cast<long long>(server.requests.size()));
            ++failures;
            return;
        }
        const auto report = QJsonDocument::fromJson(QByteArray::fromStdString(title.substr(3))).object();
        if (report.isEmpty()) {
            std::fprintf(stderr, "FAIL: %s returned an invalid JavaScript report: %s\n", origin, title.c_str());
            ++failures;
            return;
        }
        for (const char* scope : { "document", "frame", "worker" })
            checkNavigator(report.value(QLatin1String(scope)).toObject(), std::string(origin) + ' ' + scope);
        for (const char* field : { "script", "workerFetch", "fetch" }) {
            const QByteArray actual = report.value(QLatin1String(field)).toString().toUtf8();
            if (actual != expected) {
                std::fprintf(stderr, "FAIL: %s %s user agent: '%s'\n", origin, field, actual.constData());
                ++failures;
            }
        }
        QHash<QString, int> counts;
        for (const auto& request : server.requests) {
            ++counts[request.path];
            if (request.userAgent != expected) {
                std::fprintf(stderr, "FAIL: HTTP %s sent User-Agent: '%s'\n",
                    request.target.constData(), request.userAgent.constData());
                ++failures;
            }
        }
        for (const char* path : { "/redirect", "/page", "/script.js", "/style.css", "/frame",
                 "/worker.js", "/worker-fetch", "/fetch-redirect", "/fetch" }) {
            if (reload && std::string(path) == "/redirect")
                continue;
            if (!counts.contains(QLatin1String(path))) {
                std::fprintf(stderr, "FAIL: %s did not request %s\n", origin, path);
                ++failures;
            }
        }
        std::printf("%s%s: checked JavaScript and %lld HTTP request headers\n", origin, reload ? " (reload)" : "",
            static_cast<long long>(server.requests.size()));
    };
    for (const char* origin : { "http://ordinary.example", "http://duckduckgo.com", "http://accounts.google.com",
             "http://drive.google.com", "http://docs.google.com", "http://www.paypal.com" })
        checkHttpPage(origin, false);
    checkHttpPage("http://www.paypal.com", true);

    if (!failures)
        std::puts("PASS: Iridium user agent");
    return failures ? 1 : 0;
}
