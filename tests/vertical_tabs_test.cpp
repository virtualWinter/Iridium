// Exercises the real sidebar: flat list rendering, row geometry and tab actions.
#include "browser/Browser.hpp"
#include "ui/BrowserStyle.hpp"
#include "ui/decoration/windowdecoration.h"
#include "ui/settings/SettingsStore.hpp"
#include "ui/settings/SettingsWindow.hpp"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QImage>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStyleOptionViewItem>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

QColor pixelAt(const QImage& image, const QPoint& logicalPosition)
{
    return image.pixelColor(qRound(logicalPosition.x() * image.devicePixelRatio()),
        qRound(logicalPosition.y() * image.devicePixelRatio()));
}

double contrastRatio(const QColor& a, const QColor& b)
{
    const auto luminance = [](const QColor& color) {
        const auto channel = [](int value) {
            const double srgb = value / 255.0;
            return srgb <= 0.04045 ? srgb / 12.92 : std::pow((srgb + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * channel(color.red()) + 0.7152 * channel(color.green())
            + 0.0722 * channel(color.blue());
    };
    const double first = luminance(a);
    const double second = luminance(b);
    return (std::max(first, second) + 0.05) / (std::min(first, second) + 0.05);
}

double strongestContrast(const QImage& image, const QRect& area, const QColor& background)
{
    double result = 1.0;
    const qreal scale = image.devicePixelRatio();
    for (int y = qRound(area.top() * scale); y < qRound((area.bottom() + 1) * scale); ++y) {
        for (int x = qRound(area.left() * scale); x < qRound((area.right() + 1) * scale); ++x)
            result = std::max(result, contrastRatio(image.pixelColor(x, y), background));
    }
    return result;
}

void checkTabState(QListWidget* tabs, const iridium::ui::TabColors& colors,
    bool selected, bool hovered, bool active)
{
    const QColor background = selected ? colors.selectedBackground
        : (hovered ? colors.hoverBackground : colors.background);
    const QColor foreground = selected ? colors.selectedText
        : (hovered ? colors.hoverText : colors.text);
    const std::string state = std::string(active ? "active " : "inactive ")
        + (selected ? "selected " : "normal ") + (hovered ? "hovered tab" : "tab");
    check(contrastRatio(background, foreground) >= 4.5, state + " colour pair meets 4.5:1");

    // Render the shipping delegate with each real Qt state, including inactive
    // selection, rather than assuming a foreground palette role reaches paint.
    QStyleOptionViewItem option;
    option.initFrom(tabs);
    option.widget = tabs;
    option.rect = QRect(QPoint(), tabs->visualItemRect(tabs->item(0)).size());
    option.decorationSize = tabs->iconSize();
    option.textElideMode = tabs->textElideMode();
    option.state &= ~(QStyle::State_Selected | QStyle::State_MouseOver
        | QStyle::State_Active | QStyle::State_HasFocus);
    if (selected)
        option.state |= QStyle::State_Selected;
    if (hovered)
        option.state |= QStyle::State_MouseOver;
    if (active)
        option.state |= QStyle::State_Active;
    option.palette.setCurrentColorGroup(active ? QPalette::Active : QPalette::Inactive);

    const qreal scale = tabs->devicePixelRatioF();
    QImage image(qRound(option.rect.width() * scale), qRound(option.rect.height() * scale),
        QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(scale);
    image.fill(colors.background);
    {
        QPainter painter(&image);
        tabs->itemDelegate()->paint(&painter, option, tabs->model()->index(0, 0));
    }
    check(pixelAt(image, QPoint(4, option.rect.center().y())) == background,
        state + " paints the expected background");
    const QRect textArea(34, 4, option.rect.width() - 68, option.rect.height() - 8);
    const QRect closeArea(option.rect.right() - 27, 4, 24, option.rect.height() - 8);
    check(strongestContrast(image, textArea, background) >= 4.5,
        state + " renders readable title pixels");
    check(strongestContrast(image, closeArea, background) >= 3.0,
        state + " renders a readable close glyph");
    if (selected) {
        check(contrastRatio(colors.selectedIndicator, background) >= 3.0
                && contrastRatio(colors.selectedIndicator, colors.background) >= 3.0,
            state + " has a contrasting selection indicator");
        check(pixelAt(image, QPoint(0, option.rect.center().y())) == colors.selectedIndicator,
            state + " renders the selection indicator");
    }
}

bool useBrowserMenu(QToolButton* button, QAction* action, bool keyboard = false)
{
    auto* menu = button->menu();
    bool opened = false;
    // InstantPopup runs a nested event loop. Drive the real popup once it is
    // visible, then always dismiss it so a failed selection cannot hang a test.
    QTimer choose;
    choose.setSingleShot(true);
    QObject::connect(&choose, &QTimer::timeout, menu, [&] {
        opened = menu->isVisible() && QApplication::activePopupWidget() == menu;
        if (action && opened) {
            if (keyboard) {
                menu->setActiveAction(action);
                QTest::keyClick(menu, Qt::Key_Return);
            } else {
                QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier,
                    menu->actionGeometry(action).center());
            }
        } else {
            QTest::keyClick(menu, Qt::Key_Escape);
        }
        menu->hide();
    });
    choose.start(0);
    if (keyboard) {
        button->setFocus();
        QTest::keyClick(button, Qt::Key_Space);
    } else {
        QTest::mouseClick(button, Qt::LeftButton);
    }
    choose.stop();
    QCoreApplication::processEvents();
    return opened;
}

} // namespace

int main(int argc, char** argv)
{
    QTemporaryDir xdg;
    if (!xdg.isValid())
        return 1;
    qputenv("XDG_DATA_HOME", xdg.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", xdg.path().toUtf8());
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setStyle(QStringLiteral("Fusion"));
    app.setFont(QFont(QStringLiteral("Sans Serif"), 10));

    // Sweep the luminance boundary where choosing black versus white matters.
    // Every input deliberately asks for unreadable, background-coloured text.
    for (int gray = 0; gray <= 255; ++gray) {
        QPalette palette;
        const QColor color(gray, gray, gray);
        palette.setColor(QPalette::Window, color);
        palette.setColor(QPalette::WindowText, color);
        palette.setColor(QPalette::Highlight, color);
        const auto colors = iridium::ui::tabColors(palette);
        check(contrastRatio(colors.background, colors.text) >= 4.5
                && contrastRatio(colors.hoverBackground, colors.hoverText) >= 4.5
                && contrastRatio(colors.selectedBackground, colors.selectedText) >= 4.5,
            "all text states meet 4.5:1 at gray " + std::to_string(gray));
        check(contrastRatio(colors.selectedIndicator, colors.background) >= 3.0
                && contrastRatio(colors.selectedIndicator, colors.selectedBackground) >= 3.0,
            "the selection indicator meets 3:1 at gray " + std::to_string(gray));
    }

    iridium::Browser browser("about:blank");
    auto& window = browser.window();
    iridium::SettingsStore::instance().setHomePage(QStringLiteral("about:blank"));
    window.show();
    app.processEvents();

    auto* sidebar = window.findChild<QWidget*>(QStringLiteral("arcSidebar"));
    auto* header = window.findChild<QWidget*>(QStringLiteral("sidebarHeader"));
    auto* tabs = window.findChild<QListWidget*>(QStringLiteral("tabList"));
    auto* address = window.findChild<QLineEdit*>(QStringLiteral("addressBar"));
    auto* pages = window.findChild<QStackedWidget*>(QStringLiteral("rendererPages"));
    check(sidebar && header && tabs && address && pages, "the sidebar widgets exist");
    if (!sidebar || !header || !tabs || !address || !pages)
        return 1;

    QToolButton* newTab = nullptr;
    auto* menuButton = header->findChild<QToolButton*>(QStringLiteral("browserMenuButton"));
    auto* browserMenu = menuButton ? menuButton->menu() : nullptr;
    auto* settingsAction = window.findChild<QAction*>(QStringLiteral("openSettingsAction"));
    auto* historyAction = window.findChild<QAction*>(QStringLiteral("openHistoryAction"));
    auto actions = sidebar->findChildren<QToolButton*>(QStringLiteral("sidebarAction"));
    check(actions.size() == 2, "the sidebar foot holds only New Tab and Downloads");
    check(menuButton && browserMenu && settingsAction && historyAction,
        "the overflow menu and its actions exist");
    check(!sidebar->findChildren<QToolButton*>(QStringLiteral("browserMenuButton")).isEmpty(),
        "the menu button is not duplicated in the sidebar foot");
    for (auto* button : actions) {
        if (button->toolTip().startsWith(QStringLiteral("New tab")))
            newTab = button;
        check(button->toolTip() != QStringLiteral("Settings") && button->toolTip() != QStringLiteral("History"),
            "Settings and History no longer occupy separate sidebar buttons");
    }
    check(newTab != nullptr, "the new-tab button exists");
    if (!newTab || !menuButton || !browserMenu || !settingsAction || !historyAction)
        return 1;

    // Beside the navigation buttons, after Reload and before the window
    // decorations. Compared as left-to-right geometry, because findChildren
    // returns children in construction order rather than layout order.
    {
        const QRect menuBounds = menuButton->geometry();
        const QRect reloadBounds = [header] {
            for (auto* button : header->findChildren<QToolButton*>(QStringLiteral("chromeButton"))) {
                if (button->toolTip() == QStringLiteral("Reload"))
                    return button->geometry();
            }
            return QRect();
        }();
        check(!reloadBounds.isNull() && reloadBounds.right() < menuBounds.left()
                && menuBounds.left() >= reloadBounds.left(),
            "the menu sits directly after Reload in the header");
        for (auto* decoration : header->findChildren<WindowDecoration*>()) {
            const QRect bounds = decoration->geometry();
            check(bounds.left() > menuBounds.right() || bounds.right() < menuBounds.left(),
                "the menu does not overlap the window decorations");
        }
        check(menuButton->parentWidget() == header, "the menu is part of the header row");
        check(menuButton->toolTip() == QStringLiteral("Menu")
                && menuButton->accessibleName() == QStringLiteral("Menu"),
            "the icon-only menu is labelled for tooltips and assistive technology");
    }

    check(menuButton->popupMode() == QToolButton::InstantPopup && !menuButton->icon().isNull(),
        "one menu click opens the popup with a visible icon");
    check(browserMenu->actions() == QList<QAction*>({ historyAction, settingsAction }),
        "History and Settings share the compact menu");
    check(historyAction->shortcut() == QKeySequence(QStringLiteral("Ctrl+H"))
            && settingsAction->shortcut() == QKeySequence(QStringLiteral("Ctrl+,"))
            && window.actions().contains(historyAction) && window.actions().contains(settingsAction),
        "the menu actions also own the existing window shortcuts");

    check(useBrowserMenu(menuButton, settingsAction), "clicking the menu opens the actual popup");
    auto* settings = window.findChild<iridium::SettingsWindow*>();
    check(settings && settings->isVisible(), "choosing Settings opens its dialog");
    if (settings) {
        settings->selectCategory(QString(iridium::SettingsWindow::appearanceCategory()));
        settings->close();
        app.processEvents();
        check(useBrowserMenu(menuButton, historyAction, true), "the menu can also be operated with the keyboard");
        auto* settingsPanes = settings->findChild<QStackedWidget*>(QStringLiteral("settingsPanes"));
        check(settings->isVisible() && settingsPanes
                && settingsPanes->currentWidget()->objectName() == QStringLiteral("historyPage"),
            "choosing History opens the history category in the reused dialog");
        settings->close();
        app.processEvents();
        check(useBrowserMenu(menuButton, settingsAction), "the menu can be reopened after closing settings");
        check(window.findChildren<iridium::SettingsWindow*>().size() == 1 && settings->isVisible()
                && settingsPanes && settingsPanes->currentWidget()->objectName() == QStringLiteral("historyPage"),
            "Settings reuses the same dialog and remembers the selected category");
        settings->close();
        app.processEvents();
        check(useBrowserMenu(menuButton, nullptr) && !settings->isVisible(),
            "Escape dismisses the menu without opening a dialog");

        for (const auto& [key, name] : {
                 std::pair { Qt::Key_Comma, "Settings" }, std::pair { Qt::Key_H, "History" } }) {
            window.activateWindow();
            tabs->setFocus();
            app.processEvents();
            QTest::keyClick(tabs, key, Qt::ControlModifier);
            app.processEvents();
            check(settings->isVisible(), std::string(name) + " keyboard shortcut still opens the dialog");
            settings->close();
            app.processEvents();
        }
    }

    check(tabs->count() == 1, "one initial tab");
    newTab->click();
    app.processEvents();
    check(tabs->count() == 2 && window.tabCount() == 2 && browser.tabCount() == 2,
        "one new-tab click creates exactly one tab");
    newTab->click();
    app.processEvents();
    check(tabs->count() == 3 && browser.tabCount() == 3, "another click creates a third tab");
    if (tabs->count() != 3)
        return 1;

    QPixmap favicon(16, 16);
    favicon.fill(QColor(QStringLiteral("#567ac8")));
    for (int row = 0; row < tabs->count(); ++row) {
        tabs->item(row)->setText(QStringLiteral("Tab %1").arg(row + 1));
        tabs->item(row)->setIcon(QIcon(favicon));
    }
    app.processEvents();

    check(tabs->parentWidget() == sidebar, "tabs sit directly in the sidebar");
    check(tabs->frameWidth() == 0 && tabs->frameShape() == QFrame::NoFrame,
        "the tab list has no inset frame");
    check(tabs->viewport()->geometry() == tabs->rect(),
        "the viewport fills the list without an inner inset");
    check(tabs->iconSize() == QSize(16, 16), "favicons have a consistent size");
    check(tabs->horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff,
        "long titles cannot add a horizontal scrollbar");
    check(tabs->dragDropMode() == QAbstractItemView::InternalMove,
        "tabs remain draggable within the list");

    for (const QSize size : { QSize(1200, 800), QSize(700, 480) }) {
        window.resize(size);
        app.processEvents();
        check(sidebar->mapTo(window.centralWidget(), QPoint(sidebar->width(), 0)).x()
                == window.centralWidget()->width(),
            "the sidebar stays against the right edge after resizing");
        check(header->geometry().bottom() < address->geometry().top()
                && address->geometry().bottom() < tabs->geometry().top(),
            "header, address field and tabs are ordered vertically");
        check(tabs->x() == address->x() && tabs->width() == address->width(),
            "tabs align with the address field without a second inset");
        const QRect first = tabs->visualItemRect(tabs->item(0));
        check(first.height() >= 30 && first.height() <= 48,
            "tab rows have a compact, usable height");
        for (int row = 0; row < tabs->count(); ++row) {
            const QRect rect = tabs->visualItemRect(tabs->item(row));
            check(rect.height() == first.height() && rect.width() == first.width(),
                "tab rows have consistent geometry");
        }
        for (auto* decoration : sidebar->findChildren<WindowDecoration*>()) {
            check(decoration->parentWidget() == header,
                "window controls are directly in the header");
            check(decoration->mapTo(&window, QPoint()).y() < 60,
                "window controls stay at the top");
        }
        for (auto* button : actions)
            check(button->y() > tabs->geometry().bottom(), "actions stay below the tabs");
    }

    window.resize(1000, 700);
    struct PaletteCase {
        const char* name;
        const char* background;
        const char* text;
        const char* accent;
    };
    for (const auto& theme : {
             PaletteCase { "light", "#e8e8e8", "#202020", "#365fcf" },
             PaletteCase { "dark", "#252525", "#ededed", "#4668bf" },
             PaletteCase { "pale-accent", "#fff6df", "#e7d9b7", "#ffdc80" },
             PaletteCase { "dim-dark", "#1e2430", "#283142", "#263d52" },
             PaletteCase { "mid-gray", "#777777", "#7a7a7a", "#777777" },
             PaletteCase { "tinted", "#406080", "#506e86", "#60809c" },
         }) {
        QPalette palette = app.palette();
        palette.setColor(QPalette::Window, QColor(theme.background));
        palette.setColor(QPalette::WindowText, QColor(theme.text));
        palette.setColor(QPalette::Base, QColor(theme.background));
        palette.setColor(QPalette::Text, QColor(theme.text));
        palette.setColor(QPalette::ButtonText, palette.color(QPalette::Text));
        palette.setColor(QPalette::Highlight, QColor(theme.accent));
        palette.setColor(QPalette::HighlightedText, QColor(theme.accent));
        palette.setColor(QPalette::Midlight, QColor(theme.background));
        palette.setColor(QPalette::Inactive, QPalette::Text, QColor(theme.background));
        palette.setColor(QPalette::Inactive, QPalette::WindowText, QColor(theme.background));
        palette.setColor(QPalette::Inactive, QPalette::HighlightedText, QColor(theme.accent));
        app.setPalette(palette);
        window.setPalette(palette);
        app.processEvents();
        check(window.styleSheet() == iridium::ui::sharedStyleSheet(palette),
            "palette changes automatically refresh the tab stylesheet");
        const auto colors = iridium::ui::tabColors(palette);

        const QImage buttonImage = menuButton->grab().toImage();
        check(strongestContrast(buttonImage, menuButton->rect(), colors.background) >= 3.0,
            std::string(theme.name) + " keeps the overflow icon readable after a palette change");
        browserMenu->popup(menuButton->mapToGlobal(QPoint(0, 0)));
        browserMenu->setActiveAction(historyAction);
        app.processEvents();
        const QImage menuImage = browserMenu->grab().toImage();
        const QRect selectedAction = browserMenu->actionGeometry(historyAction);
        check(pixelAt(menuImage, QPoint(selectedAction.left() + 3, selectedAction.center().y()))
                == colors.hoverBackground,
            "the menu uses the shared neutral hover fill");
        check(strongestContrast(menuImage, selectedAction.adjusted(12, 2, -12, -2), colors.hoverBackground) >= 4.5,
            std::string(theme.name) + " renders readable menu entries and shortcuts");
        if (argc > 1) {
            const QString directory = QString::fromLocal8Bit(argv[1]);
            check(QDir().mkpath(directory), "the menu screenshot directory exists");
            check(menuImage.save(directory + QStringLiteral("/browser-menu-%1.png").arg(QString::fromLatin1(theme.name))),
                "the menu screenshot is saved");
        }
        browserMenu->hide();
        app.processEvents();

        const QImage image = window.grab().toImage();
        const QPoint unusedSpace = tabs->viewport()->mapTo(&window,
            QPoint(12, tabs->viewport()->height() - 12));
        check(pixelAt(image, unusedSpace) == colors.background,
            std::string(theme.name) + " tabs blend into the sidebar");
        const QRect selected = tabs->visualItemRect(tabs->currentItem());
        const QPoint selection = tabs->viewport()->mapTo(&window,
            QPoint(selected.left() + 4, selected.center().y()));
        check(pixelAt(image, selection) == colors.selectedBackground,
            "the active tab has a visible selection background");

        for (bool active : { false, true }) {
            for (bool hovered : { false, true }) {
                for (bool selected : { false, true })
                    checkTabState(tabs, colors, selected, hovered, active);
            }
        }

        // Padding must protect the close glyph, even with a favicon and a title
        // much wider than the sidebar. Compare only that region's pixels.
        auto* item = tabs->currentItem();
        const QRect closeArea(selected.right() - 27, selected.top(), 24, selected.height());
        const QImage shortTitle = tabs->viewport()->grab(closeArea).toImage();
        item->setText(QString(200, QLatin1Char('W')));
        app.processEvents();
        const QImage longTitle = tabs->viewport()->grab(closeArea).toImage();
        check(shortTitle == longTitle, "long titles never overlap the close control");
        check(!tabs->horizontalScrollBar()->isVisible(), "long titles stay within the sidebar");
        item->setText(QStringLiteral("An example tab with a long title that should be elided"));
        app.processEvents();

        if (argc > 1) {
            const QString directory = QString::fromLocal8Bit(argv[1]);
            check(QDir().mkpath(directory), "the screenshot directory exists");
            check(window.grab().save(directory + QStringLiteral("/vertical-tabs-%1.png")
                    .arg(QString::fromLatin1(theme.name))),
                "the sidebar screenshot is saved");
        }
        item->setText(QStringLiteral("Tab 3"));
    }

    const int firstId = window.tabIdForRow(0);
    const int secondId = window.tabIdForRow(1);
    check(tabs->model()->moveRow(QModelIndex(), 0, QModelIndex(), 3),
        "the tab model can reorder a row");
    app.processEvents();
    check(window.tabIdForRow(2) == firstId && window.tabIdForRow(0) == secondId,
        "reordering also updates the window's tab order");
    const auto snapshots = browser.tabs();
    check(snapshots.size() == 3 && snapshots[2].id == firstId,
        "the browser agrees with the displayed tab order");

    const QRect firstRow = tabs->visualItemRect(tabs->item(0));
    QTest::mouseClick(tabs->viewport(), Qt::LeftButton, Qt::NoModifier,
        QPoint(firstRow.left() + 50, firstRow.center().y()));
    check(window.currentTabRow() == 0 && pages->currentWidget()
            == reinterpret_cast<iridium::engine::WebView*>(
                tabs->item(0)->data(Qt::UserRole).value<quintptr>())->widget(),
        "clicking a tab selects its page");

    const QRect lastRow = tabs->visualItemRect(tabs->item(2));
    QTest::mouseClick(tabs->viewport(), Qt::LeftButton, Qt::NoModifier,
        QPoint(lastRow.right() - 16, lastRow.center().y()));
    app.processEvents();
    check(tabs->count() == 2 && window.tabCount() == 2 && browser.tabCount() == 2,
        "the close control removes exactly its tab");
    check(!browser.tab(firstId).has_value(), "closing removes the intended tab");
    check(window.currentTabRow() == 0, "closing a background tab preserves the selection");

    window.activateWindow();
    tabs->setFocus();
    app.processEvents();
    QTest::keyClick(tabs, Qt::Key_T, Qt::ControlModifier);
    app.processEvents();
    check(tabs->count() == 3 && browser.tabCount() == 3, "Ctrl+T still creates one tab");
    QTest::keyClick(tabs, Qt::Key_W, Qt::ControlModifier);
    app.processEvents();
    check(tabs->count() == 2 && browser.tabCount() == 2, "Ctrl+W still closes one tab");

    if (g_failures == 0)
        std::printf("PASS: vertical tabs\n");
    return g_failures == 0 ? 0 : 1;
}
