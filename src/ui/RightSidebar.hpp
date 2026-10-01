#pragma once

#include <QObject>
#include <QString>
#include <QWidget>

#include <vector>

class QLabel;
class QLineEdit;
class QListWidget;
class QToolButton;

namespace iridium {

// The right-hand panel host, in the spirit of Zen's sidebar: a column of
// collapsible sections beside the page, one open at a time.
//
// It is a view, deliberately. It holds no browser state of its own: the section
// contents come from the services that already own that state (the history
// store, the tab list, the extension registry) and are pushed in through
// setters, and anything the user does comes back out as a signal. MainWindow
// wires the two together. That keeps this testable with no window, no engine and
// no profile, and it means a panel cannot quietly become a second source of
// truth for state the browser already keeps.
//
// It is also where an extension's `browser.sidebarAction` panel will render
// once the extension host is enabled, which is why this is a panel host rather
// than a fixed history pane.
class RightSidebar final : public QWidget {
    Q_OBJECT
public:
    explicit RightSidebar(QWidget* parent = nullptr);

    // Sections in display order. Bookmarks is first and open by default, as in
    // the layout this is modelled on.
    enum Section { Bookmarks, History, Tabs, Extensions, SectionCount };

    // A history row, reduced to what the panel draws. The store owns the
    // formatting decision; the panel only lays out strings.
    struct HistoryEntry {
        QString url;
        QString title;
        QString subtitle;
    };

    struct TabEntry {
        int row { 0 };
        QString title;
        QString subtitle;
        bool active { false };
    };

    struct ExtensionEntry {
        QString id;
        QString name;
        bool enabled { false };
    };

    int sectionCount() const { return SectionCount; }
    int openSection() const { return m_openSection; }
    void setOpenSection(int section);
    // True while the panel body is showing; false when only the rail is.
    bool isExpanded() const { return m_expanded; }
    // Collapses the panel to its rail, and back.
    void collapse();
    void expand();

    void setHistoryEntries(const std::vector<HistoryEntry>& entries);
    // The panel holds the text; the store does the querying, because that is
    // where the index is.
    QString historySearchText() const;

    void setTabs(const std::vector<TabEntry>& tabs);
    void setExtensions(const std::vector<ExtensionEntry>& extensions);

    // Width used when the panel body is showing.
    static constexpr int kPanelWidth = 280;
    // The collapsed strip: enough for the toggle, nothing else.
    static constexpr int kRailWidth = 34;

signals:
    void openSectionChanged(int section);
    void expandedChanged(bool expanded);
    void closeRequested();
    void historyEntryActivated(const QString& url);
    void historySearchChanged(const QString& text);
    void tabActivated(int row);
    void extensionToggleRequested(const QString& id, bool enabled);

private:
    // Rebuilds the icons from the current palette. The colour-scheme handler
    // changes the palette underneath the window, and these icons are painted
    // rather than loaded, so they have to be repainted with it.
    void refreshIcons();

    struct SectionWidgets {
        QToolButton* header { nullptr };
        QWidget* body { nullptr };
    };

    void buildUi();
    void rebuildHistory();
    void rebuildTabs();
    void rebuildExtensions();
    // Applies the accordion rule: opening one section closes the others, and
    // updates the panel title to match.
    void applyOpenSection();

    SectionWidgets m_sections[SectionCount];
    QWidget* m_rail { nullptr };
    QToolButton* m_toggleButton { nullptr };
    QWidget* m_panel { nullptr };
    QLabel* m_panelTitle { nullptr };
    QToolButton* m_closeButton { nullptr };
    QLineEdit* m_historySearch { nullptr };
    QListWidget* m_historyList { nullptr };
    QLabel* m_bookmarksPlaceholder { nullptr };
    QListWidget* m_tabsList { nullptr };
    QListWidget* m_extensionsList { nullptr };

    int m_openSection { Bookmarks };
    bool m_expanded { true };
    std::vector<HistoryEntry> m_history;
    std::vector<TabEntry> m_tabs;
    std::vector<ExtensionEntry> m_extensions;
};

} // namespace iridium