// Covers the settings window as a whole: every category builds, each is
// selectable, and the live-state pages refresh when the window is shown.
//
// The point is that all five categories coexist. A pane that fails to construct
// or registers under the wrong id would otherwise only show up at runtime.

#include "browser/Browser.hpp"
#include "browser/HistoryStore.hpp"
#include "browser/ProfileManager.hpp"
#include "extensions/ExtensionRegistry.hpp"
#include "ui/BrowserStyle.hpp"
#include "ui/MainWindow.hpp"
#include "ui/decoration/windowdecoration.h"
#include "ui/settings/AppearancePage.hpp"
#include "ui/settings/GeneralPage.hpp"
#include "ui/settings/SettingsStore.hpp"
#include "ui/settings/HistoryPage.hpp"
#include "ui/settings/SettingsWindow.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QLineEdit>
#include <QListWidget>
#include <QSet>
#include <QShowEvent>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QToolButton>

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

    // The window is frameless, so it has no title bar to close it and the
    // dismissal affordance is the system window control added by
    // addWindowControls(). Checked structurally: a QDialogButtonBox is what the
    // window used to own, and this asserts there is still a way to close it
    // without hard-coding which control that turns out to be.
    check(settings.findChild<WindowDecoration*>() != nullptr
            || settings.findChild<QDialogButtonBox*>() != nullptr,
        "the settings window has a way to be dismissed");

    // The shell the browser window also uses, so the two look like one
    // application rather than a browser window beside a platform dialog.
    check(settings.styleSheet() == iridium::ui::sharedStyleSheet(settings.palette()),
        "the settings window uses the shared stylesheet");
    check(settings.testAttribute(Qt::WA_TranslucentBackground),
        "the settings window is translucent like the browser window");
    check(settings.windowFlags().testFlag(Qt::FramelessWindowHint),
        "the settings window is frameless like the browser window");
    check(settings.findChild<QWidget*>(QStringLiteral("settingsRoot")) != nullptr,
        "the settings window has the shared rounded shell widget");
    check(settings.findChild<QWidget*>(QStringLiteral("settingsCategories")) != nullptr,
        "the category rail exists and is named for the shared stylesheet");

    // The window controls belong at the TOP of the window, in the rail's header
    // where a title bar's controls would be. They were once appended to the
    // rail's own layout, which put them at the bottom below the category list,
    // while a comment in the same function claimed they were in the header.
    //
    // Checked after the first show() below rather than here, because a layout has
    // no geometry until it has been activated: measured before that, every child
    // reports y=0 and the comparison passes for the wrong reason. Asserted by
    // geometry rather than by widget name, since the whole point is where it
    // ended up on screen.
    auto* decoration = settings.findChild<WindowDecoration*>();
    check(decoration != nullptr,
        "the settings window has the system window controls");

    // Showing the window refreshes the live-state pages. History is recorded
    // here while the window is not shown, which is exactly the case refresh()
    // exists for.
    history.recordVisit(QStringLiteral("https://late.test/"),
        QStringLiteral("Recorded While Hidden"));
    settings.show();
    app.processEvents();
    settings.hide();

    if (decoration) {
        auto* header = decoration->parentWidget();
        auto* list = settings.findChild<QListWidget*>(
            QStringLiteral("settingsCategoryList"));
        check(header != nullptr, "the controls are parented to a header");
        check(list != nullptr, "the category list exists");

        if (header && list) {
            // Mapped into the dialog's own coordinates, because each widget's y
            // is relative to its own parent and the rail is itself inset, so
            // comparing raw y values across parents would not mean anything.
            QWidget* window = &settings;
            const QPoint headerAt = header->mapTo(window, QPoint(0, 0));
            const QPoint listAt = list->mapTo(window, QPoint(0, 0));
            const QPoint controlsAt = decoration->mapTo(window, QPoint(0, 0));

            check(controlsAt.y() >= 0 && controlsAt.y() < 80,
                "the window controls are at the top of the window, at y="
                    + std::to_string(controlsAt.y()));
            check(headerAt.y() + header->height() <= listAt.y(),
                "the header sits above the category list: header ends at y="
                    + std::to_string(headerAt.y() + header->height())
                    + ", the list starts at y=" + std::to_string(listAt.y()));
        }
    }

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

    // Showing the window re-reads every page, not only the two that were already
    // being refreshed. A setting changed while the window is closed used to be
    // shown as its old value: the general and appearance panes were filled once,
    // when they were built, and never again.
    {
        SettingsStore& store = SettingsStore::instance();
        auto* generalPage = settings.findChild<iridium::GeneralPage*>(
            QStringLiteral("generalPage"));
        check(generalPage != nullptr, "the general page is in the window");
        auto* homeField = generalPage
            ? generalPage->findChild<QLineEdit*>(QStringLiteral("settingsSearch"))
            : nullptr;
        check(homeField != nullptr, "the general page has a text field");
        if (homeField) {
            store.setHomePage(QStringLiteral("https://changed-while-hidden.test/"));
            settings.show();
            settings.hide();
            check(homeField->text()
                    == QStringLiteral("https://changed-while-hidden.test/"),
                "a homepage set while the window was closed is shown, got "
                    + homeField->text().toStdString());

            // Cleared while hidden: the field must go back to empty rather than
            // keeping the previous value, because the stored value is now unset
            // and the default is not something the field claims to hold.
            store.setHomePage({});
            settings.show();
            settings.hide();
            check(homeField->text().isEmpty(),
                "an unset homepage shows as an empty field, got "
                    + homeField->text().toStdString());
        }

        store.setColorScheme(QStringLiteral("light"));
        settings.show();
        settings.hide();
        auto* appearancePage = settings.findChild<iridium::AppearancePage*>(
            QStringLiteral("appearancePage"));
        auto* schemeCombo = appearancePage
            ? appearancePage->findChild<QComboBox*>(
                QStringLiteral("settingsCombo"))
            : nullptr;
        check(schemeCombo != nullptr, "the appearance page has a combo");
        check(schemeCombo && schemeCombo->currentData().toString()
                == QStringLiteral("light"),
            "a colour scheme set while the window was closed is shown");
    }

    // Opening the settings window twice must not use a freed dialog.
    //
    // MainWindow caches the dialog pointer and used to set WA_DeleteOnClose on
    // it without clearing the pointer, so the second open found a non-null
    // pointer to a deleted QDialog and called show() on freed memory. Driven
    // through the sidebar's settings button, which is the path a user takes, and
    // closing in between with Escape: the escape route and the button route are
    // the two ways the window is dismissed.
    {
        // Located by its tooltip, which is what identifies it to a user. Every
        // sidebar button shares one object name, so the name cannot address one.
        QList<QToolButton*> buttons;
        for (auto* button : window.findChildren<QToolButton*>()) {
            if (button->toolTip().compare(QStringLiteral("Settings"),
                    Qt::CaseInsensitive) == 0) {
                buttons.append(button);
            }
        }
        auto* settingsButton = buttons.isEmpty() ? nullptr : buttons.first();
        check(settingsButton != nullptr, "the sidebar has a settings button");

        if (settingsButton) {
            // Shown, because a dialog's isVisible() is false while its parent is
            // hidden, which would make the visibility assertions below pass for
            // the wrong reason.
            window.show();
            app.processEvents();

            // Every settings window parented to the browser window, not just the
            // first: this test already built one of its own above, so a single
            // findChild would return that one rather than the one the button
            // opened.
            const auto settingsWindows = [&window] {
                QList<SettingsWindow*> found;
                for (auto* candidate : window.findChildren<SettingsWindow*>())
                    found.append(candidate);
                return found;
            };
            const int before = settingsWindows().size();

            settingsButton->click();     // first open
            app.processEvents();
            const QList<SettingsWindow*> afterFirst = settingsWindows();
            check(afterFirst.size() == before + 1,
                "the first open creates a settings window, got "
                    + std::to_string(afterFirst.size()) + " for "
                    + std::to_string(before) + " before");
            // The last one is the newly created one: the click appends it.
            SettingsWindow* first = afterFirst.isEmpty()
                ? nullptr : afterFirst.last();
            check(first != nullptr, "the first open creates the settings window");
            check(first && first->isVisible(), "and shows it");

            if (first) {
                first->close();          // the dismissal that used to free it
                app.processEvents();
                check(!first->isVisible(), "closing hides it");
            }

            settingsButton->click();     // second open: the stale-pointer path
            app.processEvents();
            const QList<SettingsWindow*> afterSecond = settingsWindows();
            check(afterSecond.size() == before + 1,
                "the second open does not leave a deleted window behind, got "
                    + std::to_string(afterSecond.size()) + " for "
                    + std::to_string(before) + " before");
            if (!afterSecond.isEmpty()) {
                SettingsWindow* second = afterSecond.last();
                check(second == first,
                    "the same dialog is reused rather than a new one built");
                check(second->isVisible(), "and it is shown");
            }
        }
    }

    history.close();

    if (g_failures == 0)
        std::printf("PASS: settings window categories\n");
    else
        std::printf("%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}