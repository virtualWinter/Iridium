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

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QLayout>
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

bool controlPaintedIn(const QImage& windowImage, SettingsWindow& settings,
    QAbstractButton* button)
{
    const QImage expected = button->grab().toImage();
    const QPoint at = button->mapTo(&settings, QPoint());
    const QPoint pixelAt(qRound(at.x() * windowImage.devicePixelRatio()),
        qRound(at.y() * windowImage.devicePixelRatio()));
    const QColor background = settings.palette().color(QPalette::Active, QPalette::Window);
    int ink = 0;
    int matchingInk = 0;
    for (int y = 0; y < expected.height(); ++y) {
        for (int x = 0; x < expected.width(); ++x) {
            const QColor color = expected.pixelColor(x, y);
            if (color.alpha() == 0 || color == background)
                continue;
            ++ink;
            if (windowImage.pixelColor(pixelAt.x() + x, pixelAt.y() + y) == color)
                ++matchingInk;
        }
    }
    return ink > 0 && matchingInk >= ink * 0.9;
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

    // Categories and all panes start at the top, without a reserved title-bar
    // row. The controls occupy only the pane heading's top-right corner.
    // Measure after show(): before layout activation, child positions are not
    // meaningful and cannot catch an incorrectly placed rail or header.
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

    if (decoration) {
        auto* corner = decoration->parentWidget();
        auto* rail = settings.findChild<QWidget*>(QStringLiteral("settingsCategories"));
        auto* content = settings.findChild<QFrame*>(QStringLiteral("settingsContent"));
        auto* list = settings.findChild<QListWidget*>(
            QStringLiteral("settingsCategoryList"));
        check(corner && corner == settings.findChild<QWidget*>(QStringLiteral("settingsControls")),
            "the controls are parented to the compact corner overlay");
        check(settings.findChild<QWidget*>(QStringLiteral("settingsHeader")) == nullptr,
            "there is no separate window header row");
        check(rail != nullptr, "the category rail exists");
        check(content != nullptr, "the settings content exists");
        check(list != nullptr, "the category list exists");

        if (corner && rail && content && list && panes) {
            for (const QSize size : { QSize(880, 600), QSize(680, 480), QSize(1100, 760) }) {
                settings.resize(size);
                app.processEvents();
                const QPoint cornerAt = corner->mapTo(&settings, QPoint());
                const QPoint listAt = list->mapTo(&settings, QPoint());
                const QPoint controlsAt = decoration->mapTo(&settings, QPoint());
                const QPoint railAt = rail->mapTo(&settings, QPoint());
                const QPoint contentAt = content->mapTo(&settings, QPoint());
                const QPoint panesAt = panes->mapTo(&settings, QPoint());

                check(railAt == QPoint() && rail->width() == 200
                        && rail->height() == settings.height(),
                    "the settings categories stay against the left edge after resizing");
                check(contentAt.x() == railAt.x() + rail->width()
                        && contentAt.x() + content->width() == settings.width()
                        && contentAt.y() == 0 && content->height() == settings.height(),
                    "the settings content fills the space to the right of the categories");
                check(listAt.y() == 10 && panesAt.y() < 20,
                    "navigation and panes use top padding, not a title-bar offset");
                check(cornerAt.y() == 0 && cornerAt.x() + corner->width() == settings.width()
                        && corner->width() < content->width(),
                    "only the unused top-right corner is occupied by window chrome");
                check(!rail->isAncestorOf(decoration),
                    "moving the categories cannot move the window controls");
                check(controlsAt.y() == 6
                        && settings.width() - controlsAt.x() - decoration->width() == 8,
                    "the window controls keep their existing top-right insets");
                auto* dragHandle = corner->findChild<QWidget*>(QStringLiteral("settingsDragHandle"));
                check(dragHandle && settings.childAt(dragHandle->mapTo(&settings,
                        dragHandle->rect().center())) == dragHandle,
                    "the corner still has an accessible drag region");
                for (auto* button : decoration->findChildren<QAbstractButton*>()) {
                    check(settings.childAt(button->mapTo(&settings, button->rect().center())) == button,
                        "content does not cover the window controls' hit targets");
                }

                const QPoint firstCategory = list->viewport()->mapTo(&settings,
                    list->visualItemRect(list->item(0)).center());
                check(settings.childAt(firstCategory) == list->viewport(),
                    "the top category remains clickable, not covered by a drag overlay");

                for (const QString& category : categories) {
                    settings.selectCategory(category);
                    app.processEvents();
                    const QImage rendered = settings.grab().toImage();
                    check(decoration->isVisible() && decoration->height() > 0,
                        category.toStdString() + " keeps the window controls visible");
                    const QPoint currentControlsAt = decoration->mapTo(&settings, QPoint());
                    check(currentControlsAt.y() == 6
                            && settings.width() - currentControlsAt.x() - decoration->width() == 8,
                        category.toStdString() + " keeps the controls at the same top-right position");
                    check(!decoration->findChildren<QAbstractButton*>().isEmpty(),
                        category.toStdString() + " has actual window-control buttons");
                    for (auto* button : decoration->findChildren<QAbstractButton*>()) {
                        check(settings.childAt(button->mapTo(&settings, button->rect().center())) == button,
                            category.toStdString() + " keeps window-control hit targets above the pane");
                        check(controlPaintedIn(rendered, settings, button),
                            category.toStdString() + " renders the window-control artwork above the pane");
                    }
                    auto* page = panes->currentWidget();
                    auto* heading = page->findChild<QLabel*>(QStringLiteral("settingsHeading"),
                        Qt::FindDirectChildrenOnly);
                    check(heading != nullptr, category.toStdString() + " has a pane heading");
                    if (!heading)
                        continue;
                    check(heading->mapTo(&settings, QPoint()).y() == panesAt.y(),
                        category.toStdString() + " starts its heading at the top of the pane");
                    const QRect textArea(heading->mapTo(&settings, heading->contentsRect().topLeft()),
                        heading->contentsRect().size());
                    check(textArea.right() < cornerAt.x()
                            && textArea.width() >= heading->fontMetrics().horizontalAdvance(heading->text()),
                        category.toStdString() + " keeps its title clear of the corner controls");
                    auto* firstBodyItem = page->layout()->itemAt(1);
                    check(firstBodyItem != nullptr, category.toStdString() + " has content below its heading");
                    if (firstBodyItem) {
                        const QPoint firstBodyAt = page->mapTo(&settings,
                            firstBodyItem->geometry().topLeft());
                        check(firstBodyAt.y() >= cornerAt.y() + corner->height(),
                            category.toStdString() + " keeps fields and subtitles clear of the controls");
                    }
                    if (argc > 1 && size == QSize(880, 600)) {
                        const QString directory = QString::fromLocal8Bit(argv[1]);
                        check(QDir().mkpath(directory), "the settings screenshot directory exists");
                        check(rendered.save(directory
                                + QStringLiteral("/settings-%1-compact.png").arg(category)),
                            category.toStdString() + " compact layout screenshot is saved");
                    }
                }
            }

            settings.resize(880, 600);
            settings.selectCategory(QString(SettingsWindow::generalCategory()));
            app.processEvents();
            if (argc > 1) {
                const QString directory = QString::fromLocal8Bit(argv[1]);
                check(QDir().mkpath(directory), "the settings screenshot directory exists");
                check(settings.grab().save(directory + QStringLiteral("/settings-left-sidebar.png")),
                    "the settings layout screenshot is saved");
            }
        }
    }
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
    // through the sidebar menu's Settings action, which is the path a user takes, and
    // closing in between with Escape: the escape route and the button route are
    // the two ways the window is dismissed.
    {
        auto* settingsAction = window.findChild<QAction*>(QStringLiteral("openSettingsAction"));
        check(settingsAction != nullptr, "the sidebar menu has a Settings action");

        if (settingsAction) {
            // Shown, because a dialog's isVisible() is false while its parent is
            // hidden, which would make the visibility assertions below pass for
            // the wrong reason.
            window.show();
            app.processEvents();

            // Every settings window parented to the browser window, not just the
            // first: this test already built one of its own above, so a single
            // findChild would return that one rather than the one the action
            // opened.
            const auto settingsWindows = [&window] {
                QList<SettingsWindow*> found;
                for (auto* candidate : window.findChildren<SettingsWindow*>())
                    found.append(candidate);
                return found;
            };
            const int before = settingsWindows().size();

            settingsAction->trigger();   // first open
            app.processEvents();
            const QList<SettingsWindow*> afterFirst = settingsWindows();
            check(afterFirst.size() == before + 1,
                "the first open creates a settings window, got "
                    + std::to_string(afterFirst.size()) + " for "
                    + std::to_string(before) + " before");
            // The last one is the newly created one: the action appends it.
            SettingsWindow* first = afterFirst.isEmpty()
                ? nullptr : afterFirst.last();
            check(first != nullptr, "the first open creates the settings window");
            check(first && first->isVisible(), "and shows it");

            if (first) {
                first->close();          // the dismissal that used to free it
                app.processEvents();
                check(!first->isVisible(), "closing hides it");
            }

            settingsAction->trigger();   // second open: the stale-pointer path
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
