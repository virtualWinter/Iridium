// Verifies that the engine identifies itself with the Iridium user agent
// instead of a Safari/macOS-style string, while keeping the WebKit token for
// site compatibility.

#include "engine/webkit/WebKitView.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>

#include <cstdio>
#include <functional>
#include <string>

#ifndef IRIDIUM_VERSION
#define IRIDIUM_VERSION "0.0.1"
#endif

namespace {

constexpr int kTimeoutMs = 30000;

constexpr char kPage[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>ua-start</title></head>
<body><script>document.title = 'ua:' + navigator.userAgent;</script></body></html>)HTML";

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
    QApplication app(argc, argv);

    iridium::engine::webkit::WebKitView view;
    std::string title;
    view.setTitleChangedHandler([&title](const std::string& value) { title = value; });
    view.resize(640, 480);
    view.show();

    view.load("data:text/html;base64," + QByteArray(kPage, sizeof(kPage) - 1).toBase64().toStdString());
    if (!pumpUntil([&title] { return title.rfind("ua:", 0) == 0; }, kTimeoutMs)) {
        std::fprintf(stderr, "FAIL: page did not report the user agent\n");
        return 1;
    }

    const std::string userAgent = title.substr(3);
    std::printf("user agent: %s\n", userAgent.c_str());

    const bool hasIridium = userAgent.find(std::string("Iridium/") + IRIDIUM_VERSION) != std::string::npos;
    const bool hasMozilla = userAgent.find("Mozilla") != std::string::npos;
    const bool hasWebKit = userAgent.find("AppleWebKit") != std::string::npos;
    const bool hasSafari = userAgent.find("Safari") != std::string::npos
        || userAgent.find("Macintosh") != std::string::npos
        || userAgent.find("Mac OS X") != std::string::npos
        || userAgent.find("Version/") != std::string::npos;

    if (!hasIridium || hasMozilla || hasWebKit || hasSafari) {
        std::fprintf(stderr, "FAIL: unexpected user agent (iridium=%d mozilla=%d webkit=%d safari=%d)\n",
            hasIridium, hasMozilla, hasWebKit, hasSafari);
        return 1;
    }

    std::puts("PASS: Iridium user agent");
    return 0;
}
