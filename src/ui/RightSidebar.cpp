#include "ui/RightSidebar.hpp"

#include <QApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPainter>
#include <QPixmap>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

namespace iridium {

namespace {

QString sectionTitle(int section)
{
    switch (section) {
    case RightSidebar::Bookmarks:
        return QStringLiteral("Bookmarks");
    case RightSidebar::History:
        return QStringLiteral("History");
    case RightSidebar::Tabs:
        return QStringLiteral("Tabs");
    case RightSidebar::Extensions:
        return QStringLiteral("Extensions");
    default:
        return {};
    }
}

QIcon chevronIcon(bool expanded)
{
    // A chevron rather than a font glyph, so it follows the active palette
    // instead of whatever the theme's default font happens to provide.
    QPixmap pixmap(12, 12);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor color = QApplication::palette().color(QPalette::ButtonText);
    painter.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (expanded) {
        painter.drawPolyline(QPolygonF({ QPointF(2.5, 4.5), QPointF(6, 8), QPointF(9.5, 4.5) }));
    } else {
        painter.drawPolyline(QPolygonF({ QPointF(4.5, 2.5), QPointF(8, 6), QPointF(4.5, 9.5) }));
    }
    return QIcon(pixmap);
}

QIcon panelIcon()
{
    // Three stacked lines: a sidebar glyph, drawn rather than shipped as an
    // asset so it matches the other icons in this window.
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QApplication::palette().color(QPalette::ButtonText), 1.5,
        Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(QPointF(9.5, 2.5), QPointF(9.5, 13.5));
    painter.drawLine(QPointF(3, 4.5), QPointF(7, 4.5));
    painter.drawLine(QPointF(3, 8), QPointF(7, 8));
    painter.drawLine(QPointF(3, 11.5), QPointF(7, 11.5));
    return QIcon(pixmap);
}

QIcon closeIcon()
{
    QPixmap pixmap(12, 12);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QApplication::palette().color(QPalette::ButtonText), 1.6,
        Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(QPointF(3, 3), QPointF(9, 9));
    painter.drawLine(QPointF(9, 3), QPointF(3, 9));
    return QIcon(pixmap);
}

} // namespace

RightSidebar::RightSidebar(QWidget* parent)
    : QWidget(parent)
{
    buildUi();
    applyOpenSection();
    refreshIcons();
}

void RightSidebar::buildUi()
{
    setObjectName(QStringLiteral("rightSidebar"));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // The rail stays visible when the panel is collapsed, so the panel can
    // always be reopened without a menu or a keyboard shortcut to remember.
    m_rail = new QWidget(this);
    m_rail->setObjectName(QStringLiteral("rightSidebarRail"));
    auto* railLayout = new QVBoxLayout(m_rail);
    railLayout->setContentsMargins(3, 6, 3, 8);
    railLayout->setSpacing(6);

    m_toggleButton = new QToolButton(m_rail);
    m_toggleButton->setObjectName(QStringLiteral("sidebarRailButton"));
    m_toggleButton->setIcon(panelIcon());
    m_toggleButton->setFixedSize(28, 28);
    m_toggleButton->setToolTip(QStringLiteral("Toggle the sidebar (Ctrl+B)"));
    m_toggleButton->setAccessibleName(QStringLiteral("Toggle the sidebar"));
    connect(m_toggleButton, &QToolButton::clicked, this, [this] {
        if (m_expanded)
            collapse();
        else
            expand();
    });
    railLayout->addWidget(m_toggleButton);
    railLayout->addStretch(1);
    layout->addWidget(m_rail);

    m_panel = new QWidget(this);
    m_panel->setObjectName(QStringLiteral("rightSidebarPanel"));
    auto* panelLayout = new QVBoxLayout(m_panel);
    panelLayout->setContentsMargins(4, 6, 8, 10);
    panelLayout->setSpacing(8);

    auto* header = new QWidget(m_panel);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(4);
    m_panelTitle = new QLabel(header);
    m_panelTitle->setObjectName(QStringLiteral("rightSidebarTitle"));
    headerLayout->addWidget(m_panelTitle);
    headerLayout->addStretch(1);
    m_closeButton = new QToolButton(header);
    m_closeButton->setObjectName(QStringLiteral("sidebarRailButton"));
    m_closeButton->setIcon(closeIcon());
    m_closeButton->setFixedSize(24, 24);
    m_closeButton->setToolTip(QStringLiteral("Hide the sidebar"));
    m_closeButton->setAccessibleName(QStringLiteral("Hide the sidebar"));
    connect(m_closeButton, &QToolButton::clicked, this, &RightSidebar::closeRequested);
    headerLayout->addWidget(m_closeButton);
    panelLayout->addWidget(header);

    for (int index = 0; index < SectionCount; ++index) {
        QToolButton* sectionHeader = new QToolButton(m_panel);
        sectionHeader->setObjectName(QStringLiteral("sidebarSectionHeader"));
        sectionHeader->setCheckable(true);
        sectionHeader->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        sectionHeader->setText(sectionTitle(index));
        sectionHeader->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        sectionHeader->setCursor(Qt::PointingHandCursor);
        sectionHeader->setAccessibleName(sectionTitle(index));
        connect(sectionHeader, &QToolButton::clicked, this, [this, index] {
            // Accordion: opening the open section closes the whole panel, which
            // is how a sidebar with four sections stays out of the way.
            if (m_expanded && m_openSection == index) {
                collapse();
                return;
            }
            m_expanded = true;
            setOpenSection(index);
            applyOpenSection();
        });
        panelLayout->addWidget(sectionHeader);
        m_sections[index].header = sectionHeader;

        QWidget* body = new QWidget(m_panel);
        auto* bodyLayout = new QVBoxLayout(body);
        bodyLayout->setContentsMargins(0, 0, 0, 0);
        bodyLayout->setSpacing(4);
        panelLayout->addWidget(body, 1);
        m_sections[index].body = body;
    }

    // Bookmarks: no bookmark service exists yet, so the panel says so rather
    // than showing an empty list that looks like "you have no bookmarks".
    m_bookmarksPlaceholder = new QLabel(
        QStringLiteral("Bookmarks are not implemented yet.\n"
                       "Iridium has no bookmark store, so there is nothing to show here."),
        m_sections[Bookmarks].body);
    m_bookmarksPlaceholder->setObjectName(QStringLiteral("sidebarPlaceholder"));
    m_bookmarksPlaceholder->setWordWrap(true);
    m_bookmarksPlaceholder->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    static_cast<QVBoxLayout*>(m_sections[Bookmarks].body->layout())
        ->addWidget(m_bookmarksPlaceholder);

    m_historySearch = new QLineEdit(m_sections[History].body);
    m_historySearch->setObjectName(QStringLiteral("sidebarSearch"));
    m_historySearch->setPlaceholderText(QStringLiteral("Search history"));
    m_historySearch->setClearButtonEnabled(true);
    connect(m_historySearch, &QLineEdit::textChanged, this,
        &RightSidebar::historySearchChanged);
    static_cast<QVBoxLayout*>(m_sections[History].body->layout())
        ->addWidget(m_historySearch);

    m_historyList = new QListWidget(m_sections[History].body);
    m_historyList->setObjectName(QStringLiteral("sidebarHistoryList"));
    connect(m_historyList, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        if (item)
            emit historyEntryActivated(item->data(Qt::UserRole).toString());
    });
    static_cast<QVBoxLayout*>(m_sections[History].body->layout())->addWidget(m_historyList);

    m_tabsList = new QListWidget(m_sections[Tabs].body);
    m_tabsList->setObjectName(QStringLiteral("sidebarTabsList"));
    connect(m_tabsList, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        if (item)
            emit tabActivated(item->data(Qt::UserRole).toInt());
    });
    static_cast<QVBoxLayout*>(m_sections[Tabs].body->layout())->addWidget(m_tabsList);

    m_extensionsList = new QListWidget(m_sections[Extensions].body);
    m_extensionsList->setObjectName(QStringLiteral("sidebarExtensionsList"));
    connect(m_extensionsList, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        // Only the check state is a request; a text edit would arrive here too.
        if (!item || !(item->flags() & Qt::ItemIsUserCheckable))
            return;
        emit extensionToggleRequested(item->data(Qt::UserRole).toString(),
            item->checkState() == Qt::Checked);
    });
    static_cast<QVBoxLayout*>(m_sections[Extensions].body->layout())
        ->addWidget(m_extensionsList);

    layout->addWidget(m_panel, 1);
    setFixedWidth(kRailWidth + kPanelWidth);
}

void RightSidebar::refreshIcons()
{
    // The rail button icon is built once at construction from the palette, but
    // the colour-scheme handler repaints the palette underneath us, so the
    // icons are rebuilt whenever it changes.
    if (m_toggleButton)
        m_toggleButton->setIcon(panelIcon());
    if (m_closeButton)
        m_closeButton->setIcon(closeIcon());
    for (int index = 0; index < SectionCount; ++index) {
        if (m_sections[index].header)
            m_sections[index].header->setIcon(chevronIcon(m_expanded && m_openSection == index));
    }
}

void RightSidebar::setOpenSection(int section)
{
    if (section < 0 || section >= SectionCount)
        return;
    if (m_openSection == section) {
        applyOpenSection();
        return;
    }
    m_openSection = section;
    m_expanded = true;
    applyOpenSection();
    emit openSectionChanged(section);
    emit expandedChanged(true);
}

void RightSidebar::applyOpenSection()
{
    for (int index = 0; index < SectionCount; ++index) {
        const bool open = m_expanded && index == m_openSection;
        m_sections[index].body->setVisible(open);
        m_sections[index].header->setChecked(open);
        m_sections[index].header->setIcon(chevronIcon(open));
    }
    m_panel->setVisible(m_expanded);
    m_panelTitle->setText(sectionTitle(m_openSection));
    // Fixed rather than a maximum: the panel is a fixed-width column, and
    // letting it be resized would mean arbitrating a splitter against the
    // window's frameless resize handling for no benefit.
    setFixedWidth(m_expanded ? kRailWidth + kPanelWidth : kRailWidth);
    // With one section open the body takes the remaining height, so the header
    // column does not stretch the panel.
    for (int index = 0; index < SectionCount; ++index) {
        const bool open = index == m_openSection;
        m_sections[index].body->setSizePolicy(QSizePolicy::Preferred,
            open ? QSizePolicy::Expanding : QSizePolicy::Ignored);
    }
}

void RightSidebar::collapse()
{
    if (!m_expanded)
        return;
    m_expanded = false;
    applyOpenSection();
    emit expandedChanged(false);
}

void RightSidebar::expand()
{
    if (m_expanded)
        return;
    m_expanded = true;
    applyOpenSection();
    emit openSectionChanged(m_openSection);
    emit expandedChanged(true);
}

void RightSidebar::setHistoryEntries(const std::vector<HistoryEntry>& entries)
{
    m_history = entries;
    rebuildHistory();
}

void RightSidebar::rebuildHistory()
{
    const QSignalBlocker blocked(m_historyList);
    m_historyList->clear();
    for (const HistoryEntry& entry : m_history) {
        auto* item = new QListWidgetItem(entry.title.isEmpty() ? entry.url : entry.title,
            m_historyList);
        item->setToolTip(entry.url);
        item->setData(Qt::UserRole, entry.url);
        if (!entry.subtitle.isEmpty()) {
            item->setToolTip(entry.subtitle + QLatin1Char('\n') + entry.url);
        }
    }
}

QString RightSidebar::historySearchText() const
{
    return m_historySearch ? m_historySearch->text() : QString();
}

void RightSidebar::setTabs(const std::vector<TabEntry>& tabs)
{
    m_tabs = tabs;
    rebuildTabs();
}

void RightSidebar::rebuildTabs()
{
    // Rebuilt on every change rather than mutated in place: a tab list is
    // short, and the row indices here are the contract with MainWindow.
    const QSignalBlocker blocked(m_tabsList);
    m_tabsList->clear();
    for (const TabEntry& tab : m_tabs) {
        auto* item = new QListWidgetItem(tab.title.isEmpty() ? tab.subtitle : tab.title,
            m_tabsList);
        item->setToolTip(tab.subtitle);
        item->setData(Qt::UserRole, tab.row);
        if (tab.active)
            item->setSelected(true);
    }
}

void RightSidebar::setExtensions(const std::vector<ExtensionEntry>& extensions)
{
    m_extensions = extensions;
    rebuildExtensions();
}

void RightSidebar::rebuildExtensions()
{
    // itemChanged fires for the check state set here, so the signal is blocked:
    // populating the list is not the user asking to toggle anything.
    const QSignalBlocker blocked(m_extensionsList);
    m_extensionsList->clear();
    for (const ExtensionEntry& extension : m_extensions) {
        auto* item = new QListWidgetItem(extension.name, m_extensionsList);
        item->setData(Qt::UserRole, extension.id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(extension.enabled ? Qt::Checked : Qt::Unchecked);
    }
}

} // namespace iridium