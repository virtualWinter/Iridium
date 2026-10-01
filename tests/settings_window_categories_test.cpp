// Covers the settings window as a whole: every category builds, each is
// selectable, and the live-state pages refresh when the window is shown.
//
// The point is that all five categories coexist. A pane that fails to construct
// or registers under the wrong id would otherwise only show up at runtime.

#include "browser/Browser.hpp"
#include "browser/HistoryStore.hpp"
#include "browser/ProfileManager.hpp"
#include "extensions/ExtensionRegistry.hpp"
#include "ui/MainWindow.hpp"
#include "ui/settings/SettingsStore.hpp"
#include "ui/settings/HistoryPage.hpp"
#include "ui/settings/SettingsWindow.hpp"

#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QListWidget>
#include <QSet>
#include <QShowEvent>
#include <QStackedWidget>
#include <QTemporaryDir>

#include <cstdio>
#include <string>

using iridium::Browser;
using iridium::MainWindow;
using iridium::SettingsStore;
using iridium::SettingsWindow;
using iridium::extensions::ExtensionRegistry;

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    QTemporaryDir xdg;
    if (!xdg.isValid()) {
        std::printf("FAIL: could not create temp dirs\n");
        return 1;
    }
    qputenv("XDG_DATA_HOME", xdg.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", xdg.path().toUtf8());

    Browser browser;
    MainWindow window(browser, std::string());
    window.resize(1000, 700);

    SettingsStore::instance().pointAtProfile(
        iridium::ProfileManager::instance().current().path);

    ExtensionRegistry registry(
        iridium::ProfileManager::instance().current().path);

    iridium::history::HistoryStore history(
        iridium::ProfileManager::instance().current());
    QString error;
    if (!history.open(&error)) {
        std::printf("FAIL: history could not be opened: %s\n",
            qPrintable(error));
        return 1;
    }

    SettingsWindow settings(window, registry, history, &window);

    // Every category must select a real pane. A missing or empty pane would
    // otherwise open as a blank page with no explanation.
    const QStringList categories = {
        QString(SettingsWindow::generalCategory()),
        QString(SettingsWindow::historyCategory()),
        QString(SettingsWindow::extensionsCategory()),
        QString(SettingsWindow::profilesCategory()),
        QString(SettingsWindow::appearanceCategory()),
    };
    auto* panes = settings.findChild<QStackedWidget*>(
        QStringLiteral("settingsPanes"));
    check(panes != nullptr, "the settings window has a pane stack");
    for (const QString& category : categories) {
        settings.selectCategory(category);
        if (!panes) {
            check(false, "selecting " + category.toStdString()
                + " needs a pane stack");
            continue;
        }
        // Selecting a category must show a real pane, not leave the stack on an
        // index that does not exist.
        const int index = panes->currentIndex();
        check(index >= 0 && index < panes->count(),
            "selecting " + category.toStdString() + " shows a pane");
        check(panes->currentWidget() != nullptr,
            "selecting " + category.toStdString() + " has a current widget");
    }

    // Selecting each category must actually move the stack: if they all resolved
    // to the same index, the above would pass while only one pane was reachable.
    {
        QSet<int> shown;
        for (const QString& category : categories) {
            settings.selectCategory(category);
            if (panes)
                shown.insert(panes->currentIndex());
        }
        check(shown.size() == categories.size(),
            "each category shows a distinct pane, got "
                + std::to_string(shown.size()) + " distinct panes for "
                + std::to_string(categories.size()) + " categories");
    }

    // Selecting an id that is not a category must leave the current pane alone
    // rather than blanking the window.
    {
        settings.selectCategory(QString(SettingsWindow::historyCategory()));
        const int before = panes ? panes->currentIndex() : -1;
        settings.selectCategory(QStringLiteral("no-such-category"));
        check(panes && panes->currentIndex() == before,
            "an unknown category id leaves the pane unchanged");
    }

    // The window owns a dialog button box, so it can be dismissed.
    check(settings.findChild<QDialogButtonBox*>() != nullptr,
        "the settings window has a close button box");

    // Showing the window refreshes the live-state pages. History is recorded
    // here while the window is not shown, which is exactly the case refresh()
    // exists for.
    history.recordVisit(QStringLiteral("https://late.test/"),
        QStringLiteral("Recorded While Hidden"));
    settings.show();
    settings.hide();

    // Scoped to the history page, because the extensions page has a list of its
    // own and picking any list with rows would test the wrong one.
    settings.selectCategory(QString(SettingsWindow::historyCategory()));
    auto* historyPage = settings.findChild<iridium::history::HistoryPage*>(
        QStringLiteral("historyPage"));
    check(historyPage != nullptr, "the history page is in the window");
    auto* historyList = historyPage
        ? historyPage->findChild<QListWidget*>()
        : nullptr;
    check(historyList != nullptr, "the history page has a list");
    check(historyList && historyList->count() == 1,
        "the history pane lists exactly the visit recorded while hidden, got "
            + std::to_string(historyList ? historyList->count() : -1));
    if (historyList && historyList->count() == 1) {
        check(historyList->item(0)->text()
            .contains(QStringLiteral("Recorded While Hidden")),
            "and it is the one recorded while hidden");
    }

    // Each category remains selectable after the refresh, so refreshing does not
    // leave the window in a broken state.
    for (const QString& category : categories)
        settings.selectCategory(category);

    history.close();

    if (g_failures == 0)
        std::printf("PASS: settings window categories\n");
    else
        std::printf("%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}