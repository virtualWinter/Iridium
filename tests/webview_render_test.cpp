// Verifies that rendered frames reach the widget with the right colors and
// orientation. This exercises the presentation path (EGL readback or SHM
// fallback) end-to-end by painting a known page and inspecting the widget.

#include "engine/webkit/WebKitView.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QThread>

#include <cstdio>
#include <functional>
#include <string>

namespace {

constexpr int kTimeoutMs = 30000;

constexpr char kPage[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>pixel-test</title></head>
<body style="margin:0;background:#ff0000">
<div style="position:absolute;left:0;top:0;width:200px;height:100px;background:#00ff00"></div>
</body></html>)HTML";

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

bool isGreen(const QColor& color)
{
    return color.green() > 200 && color.red() < 60 && color.blue() < 60;
}

bool isRed(const QColor& color)
{
    return color.red() > 200 && color.green() < 60 && color.blue() < 60;
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
    if (!pumpUntil([&title] { return title == "pixel-test"; }, kTimeoutMs)) {
        std::fprintf(stderr, "FAIL: page did not load\n");
        return 1;
    }

    // Wait until a frame with the expected content has been painted.
    QImage shot;
    const bool painted = pumpUntil([&view, &shot] {
        shot = view.grab().toImage();
        return shot.width() > 620 && shot.height() > 460 && isGreen(shot.pixel(30, 20));
    }, kTimeoutMs);
    if (!painted) {
        std::fprintf(stderr, "FAIL: expected frame was not painted\n");
        return 1;
    }

    const QColor topLeft(shot.pixel(30, 20));
    const QColor topRight(shot.pixel(600, 20));
    const QColor bottomLeft(shot.pixel(30, 440));
    const QColor bottomRight(shot.pixel(600, 440));
    if (!isGreen(topLeft) || !isRed(topRight) || !isRed(bottomLeft) || !isRed(bottomRight)) {
        std::fprintf(stderr, "FAIL: unexpected pixels TL=%s TR=%s BL=%s BR=%s\n",
            topLeft.name().toUtf8().constData(), topRight.name().toUtf8().constData(),
            bottomLeft.name().toUtf8().constData(), bottomRight.name().toUtf8().constData());
        return 1;
    }

    std::printf("PASS: frame rendered with correct colors and orientation\n");
    return 0;
}
