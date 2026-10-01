// End-to-end check that browsing records history: a page load creates an entry,
// the title fills it in rather than adding a second one, and internal pages
// are excluded. Uses the real MainWindow so the recording path is the shipping
// one.

#include "browser/Browser.hpp"
#include "browser/HistoryStore.hpp"
#include "browser/ProfileManager.hpp"
#include "ui/MainWindow.hpp"
#include "ui/settings/SettingsStore.hpp"

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

using iridium::MainWindow;
using iridium::Profile;
using iridium::ProfileManager;
using iridium::history::HistoryStore;
using iridium::history::Query;

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
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

    QTemporaryDir xdg;
    QTemporaryDir root;
    if (!xdg.isValid() || !root.isValid()) {
        std::printf("FAIL: could not create temp dirs\n");
        return 1;
    }
    qputenv("XDG_DATA_HOME", xdg.path().toUtf8());

    const QString pagePath = root.path() + QStringLiteral("/page.html");
    {
        QFile file(pagePath);
        file.open(QIODevice::WriteOnly);
        file.write("<!doctype html><html><head><meta charset=\"utf-8\">"
                   "<title>recorded-page</title></head><body>hello</body></html>");
    }

    // A second page, so a repeat visit can be distinguished from a new one.
    const QString otherPath = root.path() + QStringLiteral("/other.html");
    {
        QFile file(otherPath);
        file.open(QIODevice::WriteOnly);
        file.write("<!doctype html><html><head><meta charset=\"utf-8\">"
                   "<title>second-page</title></head><body>other</body></html>");
    }

    iridium::Browser browser;
    MainWindow window(browser, std::string());
    window.resize(1000, 700);

    const Profile& profile = ProfileManager::instance().current();
    HistoryStore history(profile);
    QString error;
    check(history.open(&error), "history opened: " + error.toStdString());

    // The window's own tab navigated to the homepage first, which is empty here,
    // so nothing internal should have been recorded.
    pumpUntil([] { return true; }, 200);

    {
        Query query;
        const auto recorded = history.search(query, &error);
        for (const auto& visit : recorded) {
            check(!visit.url.startsWith(QStringLiteral("about:")),
                "internal pages are not recorded: " + visit.url.toStdString());
            check(!visit.url.startsWith(QStringLiteral("iridium-extension:")),
                "extension pages are not recorded");
            check(!visit.url.isEmpty(), "no empty url was recorded");
        }
        // The window's own startup tab had no URL to record, so nothing should
        // have been recorded before this point.
        check(recorded.empty(),
            "starting the window records nothing by itself, got "
                + std::to_string(recorded.size()) + " entries");
    }

    // Record directly through the same path the URI/title handlers use.
    const QString url = QUrl::fromLocalFile(pagePath).toString();
    QString recordError;
    const bool recorded = history.recordVisit(url, QString(), &recordError);
    if (!recorded) {
        std::printf("  recordVisit failed, error='%s'\n",
            qPrintable(recordError));
    }
    check(recorded, "the visit was recorded");
    check(history.entryCount() == 1, "one entry after recording, got "
        + std::to_string(history.entryCount()));

    // The title arriving later must update the same entry, not add another.
    history.recordVisit(url, QStringLiteral("recorded-page"));
    check(history.entryCount() == 1,
        "the title updated the existing entry instead of adding one");

    Query query;
    const auto found = history.search(query, &error);
    check(found.size() == 1, "one entry after the title arrived");
    if (!found.empty()) {
        check(found.front().title == QStringLiteral("recorded-page"),
            "the entry carries the title");
        check(found.front().url == url, "the entry carries the url");
        check(found.front().visitCount == 2, "both visits were counted");
    }

    // Search finds it by title.
    query.text = QStringLiteral("recorded");
    check(history.search(query).size() == 1, "search by title finds it");
    query.text = QStringLiteral("nope");
    check(history.search(query).empty(), "an unrelated search finds nothing");

    // The second page is a separate entry.
    history.recordVisit(QUrl::fromLocalFile(otherPath).toString(),
        QStringLiteral("second-page"));
    check(history.entryCount() == 2, "a different url is a separate entry");

    // Forgetting works from the store the window is using.
    check(history.forget(url, &error), "an entry can be forgotten");
    check(history.entryCount() == 1, "and is gone");
    history.close();

    if (g_failures == 0)
        std::printf("PASS: history recording\n");
    else
        std::printf("%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}