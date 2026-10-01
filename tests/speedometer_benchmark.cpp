// Runs Speedometer 3 through the real engine and prints the score.
//
// This is a tool, not a CTest test: it needs network access and takes several
// minutes. Speedometer starts automatically and reports its state in
// document.body[data-benchmark-state]; the final score ends up in
// #result-number.
//
// Usage: speedometer-benchmark [iterationCount]   (default 5)

#include "engine/webkit/WebKitView.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>

namespace {

constexpr int kPollIntervalMs = 2000;
constexpr int kTimeoutMs = 20 * 60 * 1000;

void sleepWithEvents(int milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
}

std::string evaluate(iridium::engine::webkit::WebKitView& view, const std::string& script)
{
    std::string result;
    bool done = false;
    view.evaluateJavaScript(script, [&result, &done](const std::string& value) {
        result = value;
        done = true;
    });
    QElapsedTimer timer;
    timer.start();
    while (!done && timer.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    const int iterations = argc > 1 ? std::atoi(argv[1]) : 5;
    const std::string url =
        "https://browserbench.org/Speedometer3.0/?startAutomatically=true&iterationCount="
        + std::to_string(iterations);

    iridium::engine::webkit::WebKitView view;
    view.resize(1280, 900);
    view.show();
    view.load(url);

    QElapsedTimer timer;
    timer.start();
    std::string score;
    for (;;) {
        // The deployed Speedometer only fills #result-number when the run is
        // complete (it does not expose a state attribute).
        const std::string result = evaluate(view,
            "(document.getElementById('result-number') || {}).textContent || ''");

        if (qEnvironmentVariableIsSet("SPEEDOMETER_VERBOSE"))
            std::fprintf(stderr, "[%4.0fs] result=\"%s\"\n",
                timer.elapsed() / 1000.0, result.c_str());

        if (result == "Error") {
            std::fprintf(stderr, "FAIL: Speedometer reported an error\n");
            return 1;
        }
        if (!result.empty()) {
            score = result;
            break;
        }
        if (timer.elapsed() >= kTimeoutMs) {
            std::fprintf(stderr, "FAIL: timed out waiting for the score\n");
            return 1;
        }
        sleepWithEvents(kPollIntervalMs);
    }

    std::printf("SCORE %s (iterations=%d, %.0fs)\n", score.c_str(), iterations, timer.elapsed() / 1000.0);
    return 0;
}
