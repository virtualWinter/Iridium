// Exercises the real sidebar: flat list rendering, row geometry and tab actions.
#include "browser/Browser.hpp"
#include "ui/BrowserStyle.hpp"
#include "ui/decoration/windowdecoration.h"
#include "ui/settings/SettingsStore.hpp"

#include <QApplication>
#include <QDir>
#include <QImage>
#include <QLineEdit>
#include <QListWidget>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

#include <cstdio>
#include <string>

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
    const auto actions = sidebar->findChildren<QToolButton*>(QStringLiteral("sidebarAction"));
    check(actions.size() == 4, "the sidebar has its four actions");
    for (auto* button : actions) {
        if (button->toolTip().startsWith(QStringLiteral("New tab")))
            newTab = button;
    }
    check(newTab != nullptr, "the new-tab button exists");
    if (!newTab)
        return 1;

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
    for (bool dark : { false, true }) {
        QPalette palette = app.palette();
        palette.setColor(QPalette::Window, QColor(dark ? "#252525" : "#e8e8e8"));
        palette.setColor(QPalette::Base, QColor(dark ? "#171717" : "#ffffff"));
        palette.setColor(QPalette::Text, QColor(dark ? "#ededed" : "#202020"));
        palette.setColor(QPalette::ButtonText, palette.color(QPalette::Text));
        palette.setColor(QPalette::Highlight, QColor(dark ? "#4668bf" : "#365fcf"));
        palette.setColor(QPalette::HighlightedText, Qt::white);
        palette.setColor(QPalette::Midlight, QColor(dark ? "#363636" : "#d4d4d4"));
        app.setPalette(palette);
        window.setPalette(palette);
        window.setStyleSheet(iridium::ui::sharedStyleSheet(palette));
        app.processEvents();

        const QImage image = window.grab().toImage();
        const QPoint unusedSpace = tabs->viewport()->mapTo(&window,
            QPoint(12, tabs->viewport()->height() - 12));
        check(pixelAt(image, unusedSpace) == palette.color(QPalette::Window),
            dark ? "dark tabs blend into the sidebar" : "light tabs blend into the sidebar");
        const QRect selected = tabs->visualItemRect(tabs->currentItem());
        const QPoint selection = tabs->viewport()->mapTo(&window,
            QPoint(selected.left() + 4, selected.center().y()));
        check(pixelAt(image, selection) == palette.color(QPalette::Highlight),
            "the active tab has a visible selection background");

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
            check(window.grab().save(directory + (dark
                    ? QStringLiteral("/vertical-tabs-dark.png")
                    : QStringLiteral("/vertical-tabs-light.png"))),
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
