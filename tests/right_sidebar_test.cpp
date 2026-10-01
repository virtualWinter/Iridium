// The right-hand panel as a widget: which section is open, what the panel
// shows when it is handed data, and that the actions it offers come back out as
// signals rather than being handled here.
//
// This is deliberately a widget test with no MainWindow, no engine and no
// profile. The panel is a view, and the point of the checks below is that it
// stays one: if it ever needs a browser service to render, this file has to grow
// a dependency, and that should be a deliberate change.

#include "ui/RightSidebar.hpp"

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMetaObject>
#include <QSignalSpy>
#include <QToolButton>

#include <cstdio>
#include <string>

using iridium::RightSidebar;

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

// The panel's lists are addressed by name rather than by their position among
// the children, so adding a section cannot silently repoint a check at the
// wrong widget. A missing name is reported by the caller, not dereferenced.
QListWidget* listNamed(RightSidebar& panel, const char* name)
{
    return panel.findChild<QListWidget*>(QString::fromLatin1(name));
}

// Activating a row is what a double-click or Return does; the signal is what
// the window listens to.
void activate(QListWidget* list, int row)
{
    if (!list || row < 0 || row >= list->count())
        return;
    QListWidgetItem* item = list->item(row);
    QMetaObject::invokeMethod(list, "itemActivated", Qt::DirectConnection,
        Q_ARG(QListWidgetItem*, item));
}

void testInitialState()
{
    RightSidebar panel;

    check(panel.sectionCount() == 4, "four sections");
    check(panel.openSection() == RightSidebar::Bookmarks,
        "bookmarks is the open section by default");
    check(panel.isExpanded(), "the panel starts expanded");
    check(panel.width() == RightSidebar::kRailWidth + RightSidebar::kPanelWidth,
        "the expanded panel is the rail plus the panel width");
}

void testCollapseAndExpand()
{
    RightSidebar panel;
    QSignalSpy expanded(&panel, &RightSidebar::expandedChanged);

    panel.collapse();
    check(!panel.isExpanded(), "collapse hides the body");
    check(panel.width() == RightSidebar::kRailWidth,
        "the collapsed panel is just the rail");
    check(expanded.count() == 1, "collapse reports itself once");

    panel.expand();
    check(panel.isExpanded(), "expand shows the body again");
    check(expanded.count() == 2, "expand reports itself once");
    check(expanded.last().at(0).toBool(), "the signal carries the new state");

    // A repeated request is not a change, and saying so again would make a
    // listener think the panel moved when it did not.
    panel.collapse();
    panel.collapse();
    check(expanded.count() == 3, "a repeated collapse is not reported again");
}

void testAccordion()
{
    RightSidebar panel;
    QSignalSpy sections(&panel, &RightSidebar::openSectionChanged);

    panel.setOpenSection(RightSidebar::History);
    check(panel.openSection() == RightSidebar::History, "the section changes");
    check(sections.count() == 1, "one report");
    check(sections.last().at(0).toInt() == RightSidebar::History,
        "the signal carries the section");

    panel.setOpenSection(RightSidebar::History);
    check(sections.count() == 1, "reselecting the open section is not reported");

    panel.setOpenSection(99);
    check(panel.openSection() == RightSidebar::History,
        "an unknown section leaves the panel alone");

    // A collapsed panel still knows which section it would reopen, so choosing a
    // section has to bring the body back rather than silently doing nothing.
    panel.collapse();
    panel.setOpenSection(RightSidebar::Tabs);
    check(panel.isExpanded(), "choosing a section reopens the panel");
    check(panel.openSection() == RightSidebar::Tabs, "and selects it");
}

void testHistoryEntries()
{
    RightSidebar panel;
    QListWidget* history = listNamed(panel, "sidebarHistoryList");
    check(history != nullptr, "the history section has a list");
    if (!history)
        return;
    QSignalSpy activated(&panel, &RightSidebar::historyEntryActivated);

    // A page with no title is the common case in a history store, so the
    // fallback to the URL is part of what is being checked.
    panel.setHistoryEntries({
        { QStringLiteral("https://example.com/"), QStringLiteral("Example"), QStringLiteral("just now") },
        { QStringLiteral("https://kde.org/"), QStringLiteral("KDE"), QStringLiteral("yesterday") },
        { QStringLiteral("https://untitled.test/"), {}, {} },
    });

    check(history->count() == 3, "every entry becomes a row");
    if (history->count() != 3)
        return;
    check(history->item(0)->text() == QStringLiteral("Example"),
        "a titled entry shows its title");
    check(history->item(2)->text() == QStringLiteral("https://untitled.test/"),
        "an untitled entry falls back to its URL");
    check(history->item(1)->toolTip().contains(QStringLiteral("https://kde.org/")),
        "the tooltip carries the URL");

    activate(history, 0);
    check(activated.count() == 1, "activating a row reports it once");
    if (activated.count() == 1) {
        check(activated.last().at(0).toString() == QStringLiteral("https://example.com/"),
            "and carries the URL, not the title");
    }

    panel.setHistoryEntries({});
    check(history->count() == 0, "empty history empties the list");
}

void testTabsAndActivation()
{
    RightSidebar panel;
    QListWidget* tabs = listNamed(panel, "sidebarTabsList");
    check(tabs != nullptr, "the tabs section has a list");
    if (!tabs)
        return;
    QSignalSpy activated(&panel, &RightSidebar::tabActivated);

    panel.setTabs({
        { 0, QStringLiteral("Example"), QStringLiteral("https://example.com/"), true },
        { 3, QStringLiteral("KDE"), QStringLiteral("https://kde.org/"), false },
        { 7, {}, QStringLiteral("about:blank"), false },
    });

    check(tabs->count() == 3, "every tab becomes a row");
    if (tabs->count() != 3)
        return;
    check(tabs->item(0)->text() == QStringLiteral("Example"), "tab title");
    check(tabs->item(2)->text() == QStringLiteral("about:blank"),
        "an untitled tab falls back to its URL");

    // The value carried is the window's row for the tab, which is not the tab id
    // and not this list's own position. Getting that wrong would switch tabs.
    activate(tabs, 1);
    check(activated.count() == 1, "activating a tab reports it");
    if (activated.count() == 1)
        check(activated.last().at(0).toInt() == 3, "and carries the row");

    panel.setTabs({});
    check(tabs->count() == 0, "no tabs means no rows");
}

void testExtensionToggles()
{
    RightSidebar panel;
    QListWidget* extensions = listNamed(panel, "sidebarExtensionsList");
    check(extensions != nullptr, "the extensions section has a list");
    if (!extensions)
        return;
    QSignalSpy toggled(&panel, &RightSidebar::extensionToggleRequested);

    panel.setExtensions({
        { QStringLiteral("alpha"), QStringLiteral("Alpha"), true },
        { QStringLiteral("beta"), QStringLiteral("Beta"), false },
    });

    check(extensions->count() == 2, "every extension becomes a row");
    if (extensions->count() != 2)
        return;
    check(extensions->item(0)->checkState() == Qt::Checked,
        "an enabled extension is checked");
    check(extensions->item(1)->checkState() == Qt::Unchecked,
        "a disabled extension is unchecked");

    // Filling the list must not read as the user asking to toggle anything.
    check(toggled.count() == 0,
        "populating the list does not report a toggle request");

    extensions->item(1)->setCheckState(Qt::Checked);
    check(toggled.count() == 1, "checking a row reports a request");
    if (toggled.count() == 1) {
        check(toggled.last().at(0).toString() == QStringLiteral("beta"),
            "the request carries the extension id");
        check(toggled.last().at(1).toBool(), "and the state it wants");
    }

    panel.setExtensions({});
    check(extensions->count() == 0, "no extensions means no rows");
}

void testSearchReportsText()
{
    RightSidebar panel;
    auto* search = panel.findChild<QLineEdit*>(QStringLiteral("sidebarSearch"));
    check(search != nullptr, "the history section has a search box");
    if (!search)
        return;
    QSignalSpy changed(&panel, &RightSidebar::historySearchChanged);

    search->setText(QStringLiteral("kde"));
    check(changed.count() == 1, "typing reports the text");
    if (changed.count() == 1)
        check(changed.last().at(0).toString() == QStringLiteral("kde"),
            "and carries it");
    check(panel.historySearchText() == QStringLiteral("kde"),
        "the panel holds the text so the window can query the store");
}

void testBookmarksSectionSaysSo()
{
    // There is no bookmark store. The panel has to say that rather than show an
    // empty list, which would read as "you have no bookmarks".
    RightSidebar panel;
    auto* placeholder = panel.findChild<QLabel*>(QStringLiteral("sidebarPlaceholder"));
    check(placeholder != nullptr, "the bookmarks section explains itself");
    if (placeholder)
        check(placeholder->text().contains(QStringLiteral("not implemented")),
            "and says what is missing");
}

void testSectionHeadersAreLabelled()
{
    RightSidebar panel;
    const auto headers = panel.findChildren<QToolButton*>();
    int sections = 0;
    for (QToolButton* button : headers) {
        const QString name = button->accessibleName();
        if (name == QLatin1String("Bookmarks") || name == QLatin1String("History")
            || name == QLatin1String("Tabs") || name == QLatin1String("Extensions")) {
            ++sections;
        }
    }
    check(sections == 4, "each section header is labelled for assistive technology");
}

} // namespace

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    testInitialState();
    testCollapseAndExpand();
    testAccordion();
    testHistoryEntries();
    testTabsAndActivation();
    testExtensionToggles();
    testSearchReportsText();
    testBookmarksSectionSaysSo();
    testSectionHeadersAreLabelled();

    if (g_failures == 0)
        std::printf("PASS: right sidebar panel\n");
    else
        std::printf("%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}