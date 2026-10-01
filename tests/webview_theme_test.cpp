// Verifies that pages see the system color scheme through the engine adapter.
//
// The legacy libwpe embedding path never receives WPE Platform settings, so
// the engine emulates prefers-color-scheme: matchMedia is patched and the
// media queries of accessible stylesheets are rewritten. The test page reports
// both the matchMedia result and the computed style of a media-query rule; the
// test forces both schemes through WebKitView::setPreferredColorScheme() so it
// does not depend on the host color scheme.

#include "engine/webkit/WebKitView.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>

#include <cstdio>
#include <functional>
#include <string>

namespace {

constexpr int kTimeoutMs = 30000;

constexpr char kPage[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>theme-start</title>
<style>
#box { width: 10px; height: 10px; background-color: rgb(255, 255, 255); }
@media (prefers-color-scheme: dark) { #box { background-color: rgb(0, 0, 0); } }
</style></head>
<body><div id="box"></div>
<script>
function report() {
    const dark = window.matchMedia('(prefers-color-scheme: dark)').matches;
    const background = getComputedStyle(document.getElementById('box')).backgroundColor;
    const darkCss = background === 'rgb(0, 0, 0)';
    document.title = (dark ? 'dark' : 'light') + '-mq' + (dark ? 1 : 0) + '-css' + (darkCss ? 1 : 0);
}
window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', report);
document.addEventListener('DOMContentLoaded', report);
</script></body></html>)HTML";

std::string dataUri()
{
    const QByteArray encoded = QByteArray(kPage, sizeof(kPage) - 1).toBase64();
    return "data:text/html;base64," + encoded.toStdString();
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

bool isThemeTitle(const std::string& title)
{
    return title.rfind("dark-", 0) == 0 || title.rfind("light-", 0) == 0;
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

    view.load(dataUri());
    if (!pumpUntil([&title] { return isThemeTitle(title); }, kTimeoutMs)) {
        std::fprintf(stderr, "FAIL: no color scheme reported (title=\"%s\")\n", title.c_str());
        return 1;
    }
    std::printf("initial: %s\n", title.c_str());

    view.setPreferredColorScheme(true);
    if (!pumpUntil([&title] { return title == "dark-mq1-css1"; }, kTimeoutMs)) {
        std::fprintf(stderr, "FAIL: expected dark-mq1-css1, got \"%s\"\n", title.c_str());
        return 1;
    }
    std::printf("dark: %s\n", title.c_str());

    view.setPreferredColorScheme(false);
    if (!pumpUntil([&title] { return title == "light-mq0-css0"; }, kTimeoutMs)) {
        std::fprintf(stderr, "FAIL: expected light-mq0-css0, got \"%s\"\n", title.c_str());
        return 1;
    }
    std::printf("light: %s\nPASS\n", title.c_str());
    return 0;
}
