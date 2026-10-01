// Verifies progressive media playback through the engine adapter.
//
// The test generates a small WebM clip (VP8 video + Vorbis audio) with
// GStreamer, embeds it into a data: URL page, clicks the video element and
// waits for playback to advance. It skips itself when the GStreamer encoder
// elements are unavailable, and catches regressions such as missing audio
// sinks (autoaudiosink), decoders or demuxers.

#include "engine/webkit/WebKitView.hpp"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QMouseEvent>
#include <QThread>

#include <gst/gst.h>

#include <cstdio>
#include <functional>
#include <optional>
#include <string>

namespace {

constexpr int kTimeoutMs = 45000;

bool elementAvailable(const char* name)
{
    GstElementFactory* factory = gst_element_factory_find(name);
    if (!factory)
        return false;
    gst_object_unref(factory);
    return true;
}

std::optional<QByteArray> generateClip()
{
    for (const char* required : { "videotestsrc", "audiotestsrc", "vp8enc", "vorbisenc", "webmmux", "filesink" }) {
        if (!elementAvailable(required)) {
            std::fprintf(stderr, "skipping: GStreamer element %s is missing\n", required);
            return std::nullopt;
        }
    }

    const QString path = QDir::tempPath() + QStringLiteral("/iridium-media-test.webm");
    QFile::remove(path);
    const QByteArray description = QStringLiteral(
        "videotestsrc num-buffers=90 ! video/x-raw,width=320,height=240,framerate=30/1 "
        "! vp8enc deadline=1 ! webmmux name=mux ! filesink location=%1 "
        "audiotestsrc num-buffers=200 ! vorbisenc ! mux.").arg(path).toUtf8();

    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(description.constData(), &error);
    if (!pipeline || error) {
        std::fprintf(stderr, "skipping: could not build clip pipeline: %s\n",
            error ? error->message : "unknown error");
        g_clear_error(&error);
        if (pipeline)
            gst_object_unref(pipeline);
        return std::nullopt;
    }

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBus* bus = gst_element_get_bus(pipeline);
    GstMessage* message = gst_bus_timed_pop_filtered(bus, 30 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    const bool generated = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
    if (message)
        gst_message_unref(message);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    if (!generated) {
        std::fprintf(stderr, "skipping: clip generation failed\n");
        return std::nullopt;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() == 0) {
        std::fprintf(stderr, "skipping: generated clip is empty\n");
        return std::nullopt;
    }
    return file.readAll();
}

std::string dataUri(const QByteArray& clip)
{
    QByteArray page = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>media-start</title></head>
<body style="margin:0">
<video id="v" src="data:video/webm;base64,@CLIP@" width="320" height="240" muted
       onclick="this.play()"></video>
<script>
const video = document.getElementById('v');
const timer = setInterval(() => {
    if (video.error) {
        document.title = 'media-error-' + video.error.code;
        clearInterval(timer);
    } else if (video.currentTime > 0.2) {
        document.title = 'media-playing';
        clearInterval(timer);
    }
}, 100);
</script>
</body></html>)HTML";
    page.replace("@CLIP@", clip.toBase64());
    return "data:text/html;base64," + page.toBase64().toStdString();
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

// The video element occupies the top-left corner of the page; clicks double as
// the user gesture that allows playback to start.
void clickVideoElement(QWidget& widget)
{
    const QPointF position(160, 120);
    const QPoint global = widget.mapToGlobal(position.toPoint());
    QMouseEvent press(QEvent::MouseButtonPress, position, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, position, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    press.setTimestamp(1000);
    release.setTimestamp(1010);
    QApplication::sendEvent(&widget, &press);
    QApplication::sendEvent(&widget, &release);
}

} // namespace

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    gst_init(nullptr, nullptr);

    const auto clip = generateClip();
    if (!clip.has_value())
        return 77; // CTest skip code.

    iridium::engine::webkit::WebKitView view;
    std::string title;
    view.setTitleChangedHandler([&title](const std::string& value) { title = value; });
    view.resize(640, 480);
    view.show();

    view.load(dataUri(*clip));
    if (!pumpUntil([&title] { return title == "media-start"; }, kTimeoutMs)) {
        std::fprintf(stderr, "FAIL: media page did not load (title=\"%s\")\n", title.c_str());
        return 1;
    }

    clickVideoElement(view);
    if (!pumpUntil([&title] { return title == "media-playing"; }, kTimeoutMs)) {
        std::fprintf(stderr, "FAIL: playback did not advance (title=\"%s\")\n", title.c_str());
        return 1;
    }

    std::printf("PASS: local WebM clip played\n");
    return 0;
}
