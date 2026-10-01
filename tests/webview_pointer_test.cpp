// Regression test for the pointer input path into WPE.
//
// WPE's legacy libwpe pointer events encode buttons as a 1-based index
// (left = 1, right = 2, middle = 3, back = 4, forward = 5); that is what
// WebKit's WebEventFactory consumes in WebEventFactory::createWebMouseEvent().
// Linux input-event codes such as BTN_LEFT (0x110) do not match any button and
// the resulting mouse event is dropped, so these events must be translated.
//
// The test drives the real WebKitView with synthetic Qt mouse events and
// observes the page's reaction through the public title callback.

#include "engine/webkit/WebKitView.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMouseEvent>
#include <QThread>
#include <QWheelEvent>

#include <cstdio>
#include <functional>
#include <string>

namespace {

constexpr int kTimeoutMs = 20000;

constexpr char kPage[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>idle</title></head>
<body style="margin:0"
      onmousemove="document.title='move-'+Math.round(event.clientX)+'x'+Math.round(event.clientY)">
<div id="left" style="position:absolute;left:0;top:0;width:240px;height:120px"
     onclick="document.title='left-click'">left</div>
<div id="right" style="position:absolute;left:0;top:160px;width:240px;height:120px"
     onmousedown="if(event.button===2)document.title='right-down'">right</div>
<div id="wheel" style="position:absolute;left:0;top:320px;width:240px;height:120px"
     onwheel="document.title='wheel-'+(event.deltaY<0?'up':'down')">wheel</div>
</body></html>)HTML";

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

quint64 nextTimestamp()
{
    static quint64 timestamp = 1000;
    timestamp += 10;
    return timestamp;
}

void sendMouseEvent(QWidget& widget, QEvent::Type type, const QPointF& position,
    Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent event(type, position, widget.mapToGlobal(position.toPoint()),
        button, buttons, Qt::NoModifier);
    event.setTimestamp(nextTimestamp());
    QApplication::sendEvent(&widget, &event);
}

void click(QWidget& widget, const QPointF& position, Qt::MouseButton button)
{
    sendMouseEvent(widget, QEvent::MouseButtonPress, position, button, button);
    sendMouseEvent(widget, QEvent::MouseButtonRelease, position, button, Qt::NoButton);
}

int fail(const char* message, const std::string& title)
{
    std::fprintf(stderr, "FAIL: %s (title=\"%s\")\n", message, title.c_str());
    return 1;
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
    view.resize(800, 600);
    view.show();

    view.load(dataUri());
    if (!pumpUntil([&title] { return title == "idle"; }, kTimeoutMs))
        return fail("page did not load", title);
    std::puts("page loaded");

    sendMouseEvent(view, QEvent::MouseMove, QPointF(140, 40), Qt::NoButton, Qt::NoButton);
    if (!pumpUntil([&title] { return title == "move-140x40"; }, kTimeoutMs))
        return fail("mouse move was not delivered to the page", title);
    std::puts("mouse move delivered");

    click(view, QPointF(120, 60), Qt::LeftButton);
    if (!pumpUntil([&title] { return title == "left-click"; }, kTimeoutMs))
        return fail("left click was not delivered to the page", title);
    std::puts("left click delivered");

    click(view, QPointF(120, 220), Qt::RightButton);
    if (!pumpUntil([&title] { return title == "right-down"; }, kTimeoutMs))
        return fail("right click was not delivered to the page", title);
    std::puts("right click delivered");

    // A wheel event above the page must reach the DOM as a negative deltaY.
    QWheelEvent wheel(QPointF(120, 380), view.mapToGlobal(QPoint(120, 380)),
        QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
        Qt::ScrollUpdate, false, Qt::MouseEventNotSynthesized);
    wheel.setTimestamp(nextTimestamp());
    QApplication::sendEvent(&view, &wheel);
    if (!pumpUntil([&title] { return title == "wheel-up"; }, kTimeoutMs))
        return fail("wheel event was not delivered to the page", title);
    std::puts("wheel event delivered");

    std::puts("PASS");
    return 0;
}
