// Verifies WebGL and 2D canvas rendering through the engine adapter.
//
// The test page creates a WebGL context, clears it and reads the pixel back,
// then does the same for a 2D canvas, reporting the result through
// document.title. This catches missing features or broken GL/canvas backends
// even when rendering is software (the SHM bring-up path forces software GL).

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
<html><head><meta charset="utf-8"><title>gl-start</title></head>
<body style="margin:0">
<canvas id="c" width="320" height="240"></canvas>
<script>
function check() {
    const canvas = document.getElementById('c');
    const gl = canvas.getContext('webgl') || canvas.getContext('experimental-webgl');
    if (!gl)
        return 'gl-missing';

    gl.clearColor(0.2, 0.4, 0.8, 1.0);
    gl.clear(gl.COLOR_BUFFER_BIT);
    const pixel = new Uint8Array(4);
    gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, pixel);
    const glOk = pixel[0] < 64 && pixel[1] >= 64 && pixel[1] <= 160 && pixel[2] > 160;

    const canvas2d = document.createElement('canvas');
    canvas2d.width = 8;
    canvas2d.height = 8;
    const context = canvas2d.getContext('2d');
    context.fillStyle = '#ff0000';
    context.fillRect(0, 0, 8, 8);
    const data = context.getImageData(0, 0, 1, 1).data;
    const canvasOk = data[0] === 255 && data[1] === 0 && data[2] === 0;

    return 'gl-ok-' + (glOk ? 'webgl' : 'webgl-pixels') + '-' + (canvasOk ? 'canvas' : 'canvas-pixels');
}
document.title = check();
</script>
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
    const bool ok = pumpUntil(
        [&title] { return title == "gl-ok-webgl-canvas"; }, kTimeoutMs);
    if (!ok) {
        std::fprintf(stderr, "FAIL: unexpected result \"%s\"\n", title.c_str());
        return 1;
    }

    std::printf("PASS: %s\n", title.c_str());
    return 0;
}
