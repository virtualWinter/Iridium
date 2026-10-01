// Verifies file downloads through the engine adapter.
//
// The test page contains a link with the `download` attribute; a synthetic
// click starts the download, the engine saves it and reports progress and
// completion states through the public handler. XDG_CONFIG_HOME is pointed at
// a temporary directory so the test never writes into the real ~/Downloads.

#include "engine/webkit/WebKitView.hpp"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QMouseEvent>
#include <QThread>

#include <cstdio>
#include <functional>
#include <string>

namespace {

constexpr int kTimeoutMs = 30000;
constexpr char kContent[] = "Iridium download test\n";
constexpr char kFileName[] = "iridium-download-test.txt";

QString prepareDownloadEnvironment()
{
    const QString root = QDir::tempPath() + QStringLiteral("/iridium-download-test");
    QDir(root).removeRecursively();
    if (!QDir().mkpath(root + QStringLiteral("/config")) || !QDir().mkpath(root + QStringLiteral("/Downloads")))
        return {};

    QFile userDirs(root + QStringLiteral("/config/user-dirs.dirs"));
    if (!userDirs.open(QIODevice::WriteOnly | QIODevice::Text))
        return {};
    userDirs.write(QStringLiteral("XDG_DOWNLOAD_DIR=\"%1/Downloads\"\n").arg(root).toUtf8());
    userDirs.close();

    qputenv("XDG_CONFIG_HOME", (root + QStringLiteral("/config")).toUtf8());
    return root;
}

std::string dataUri()
{
    const QByteArray href = "data:text/plain;base64," + QByteArray(kContent, sizeof(kContent) - 1).toBase64();
    QByteArray page = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>download-start</title></head>
<body style="margin:0">
<a id="link" download="iridium-download-test.txt" href="@HREF@"
   style="display:block;width:200px;height:80px">download</a>
</body></html>)HTML";
    page.replace("@HREF@", href);
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

void clickAt(QWidget& widget, const QPointF& position)
{
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
    const QString root = prepareDownloadEnvironment();
    if (root.isEmpty()) {
        std::fprintf(stderr, "FAIL: could not prepare download environment\n");
        return 1;
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    struct Capture {
        bool seen { false };
        iridium::engine::DownloadState state;
    } capture;

    int result = 0;
    {
        iridium::engine::webkit::WebKitView view;
        std::string title;
        view.setTitleChangedHandler([&title](const std::string& value) { title = value; });
        view.setDownloadStateHandler([&capture](const iridium::engine::DownloadState& state) {
            capture.seen = true;
            capture.state = state;
        });
        view.resize(640, 480);
        view.show();

        view.load(dataUri());
        if (!pumpUntil([&title] { return title == "download-start"; }, kTimeoutMs)) {
            std::fprintf(stderr, "FAIL: page did not load (title=\"%s\")\n", title.c_str());
            result = 1;
        } else {
            clickAt(view, QPointF(60, 40));
            const bool finished = pumpUntil([&capture] {
                return capture.seen && (capture.state.finished || capture.state.failed);
            }, kTimeoutMs);
            if (!finished) {
                std::fprintf(stderr, "FAIL: download did not finish\n");
                result = 1;
            } else if (capture.state.failed) {
                std::fprintf(stderr, "FAIL: download failed: %s\n", capture.state.error.c_str());
                result = 1;
            } else if (capture.state.fileName != kFileName) {
                std::fprintf(stderr, "FAIL: unexpected file name \"%s\"\n", capture.state.fileName.c_str());
                result = 1;
            } else {
                QFile file(QString::fromStdString(capture.state.destination));
                const QByteArray expected(kContent, sizeof(kContent) - 1);
                if (!file.open(QIODevice::ReadOnly) || file.readAll() != expected) {
                    std::fprintf(stderr, "FAIL: downloaded file is missing or has wrong content\n");
                    result = 1;
                } else {
                    std::printf("PASS: downloaded %s (%lld bytes)\n", capture.state.fileName.c_str(),
                        static_cast<long long>(expected.size()));
                }
            }
        }
    }

    QDir(root).removeRecursively();
    return result;
}
