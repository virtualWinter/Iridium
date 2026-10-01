#include "ui/settings/SettingsWindow.hpp"

#include "ui/BrowserStyle.hpp"
#include "ui/MainWindow.hpp"
#include "ui/decoration/decorationtheme.h"
#include "ui/decoration/windowdecoration.h"
#include "ui/settings/AppearancePage.hpp"
#include "ui/settings/ExtensionsPage.hpp"
#include "ui/settings/GeneralPage.hpp"
#include "ui/settings/HistoryPage.hpp"
#include "ui/settings/ProfilesPage.hpp"

#include <QFrame>
#include <QGuiApplication>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QShortcut>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>

namespace iridium {

namespace {
// A real role, not UserRole+n: item data is keyed by int.
constexpr int kCategoryRole = Qt::UserRole + 100;
} // namespace

SettingsWindow::SettingsWindow(MainWindow& window, extensions::ExtensionRegistry& registry,
    history::HistoryStore& historyStore, QWidget* parent)
    : QDialog(parent)
    , m_window(window)
    , m_registry(registry)
    , m_history(historyStore)
{
    setObjectName(QStringLiteral("settingsWindow"));
    setWindowTitle(tr("Settings"));
    setModal(true);
    resize(880, 600);
    // A dialog should not be resized smaller than its panes can usefully be.
    setMinimumSize(680, 480);

    // Same shell as the browser window: rounded, translucent, frameless, with
    // the same stylesheet. Without this the settings window is the one place in
    // the application that looks like it came from somewhere else.
    m_chrome = std::make_unique<ui::FramelessChrome>(this);
    setStyleSheet(ui::sharedStyleSheet(palette()));

    buildUi();
    selectCategoryByIndex(0);
}

SettingsWindow::~SettingsWindow() = default;

void SettingsWindow::buildUi()
{
    // The shell widget. Its own name is what the shared stylesheet keys the
    // rounded background on, so it is not the dialog itself.
    auto* root = new QWidget(this);
    root->setObjectName(QStringLiteral("settingsRoot"));
    auto* rootLayout = new QHBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // The pane goes in a bordered frame inset from the shell, the way the page
    // is inset from the window edge in the browser window. It is added first so
    // the rail takes the right edge, matching where the browser's sidebar sits.
    auto* content = new QFrame(root);
    content->setObjectName(QStringLiteral("settingsContent"));
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(14, 12, 14, 14);
    contentLayout->setSpacing(0);

    m_panes = new QStackedWidget(content);
    m_panes->setObjectName(QStringLiteral("settingsPanes"));
    contentLayout->addWidget(m_panes, 1);
    rootLayout->addWidget(content, 1);

    // The category rail, against the right edge like the browser sidebar.
    auto* rail = new QWidget(root);
    rail->setObjectName(QStringLiteral("settingsCategories"));
    rail->setFixedWidth(200);
    auto* railLayout = new QVBoxLayout(rail);
    railLayout->setContentsMargins(8, 6, 8, 10);
    railLayout->setSpacing(10);

    // A frameless window has no title bar, so the rail header stands in for one:
    // window controls in the corner, empty space between them to drag by. It is
    // added to the rail layout before anything else, which is what puts it at
    // the top of the window.
    auto* header = new QWidget(rail);
    // Kept, because addWindowControls parents the decoration to it and the
    // header is otherwise a local.
    m_railHeader = header;
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(4);
    railLayout->addWidget(header);

    auto* dragHandle = m_chrome->dragHandle();
    dragHandle->setParent(header);
    dragHandle->setMinimumWidth(8);
    headerLayout->addWidget(dragHandle, 1);
    addWindowControls(headerLayout);

    m_categories = new QListWidget(rail);
    m_categories->setObjectName(QStringLiteral("settingsCategoryList"));
    m_categories->setFixedWidth(184);
    m_categories->setSelectionMode(QAbstractItemView::SingleSelection);
    m_categories->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_categories->setFrameShape(QFrame::NoFrame);
    railLayout->addWidget(m_categories, 1);

    // Connected once, before any row exists, so selecting a category cannot
    // arrive before there is a pane to show.
    connect(m_categories, &QListWidget::currentRowChanged,
        this, &SettingsWindow::selectCategoryByIndex);

    rootLayout->addWidget(rail);

    // A QDialog's own layout would centre this on top of the shell margins;
    // the shell is the dialog's whole content, so the margins are zero and the
    // rail's own margins do the spacing.
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    outer->addWidget(root);

    const auto addCategory = [this](const QString& id, const QString& label,
        QStyle::StandardPixmap icon) {
        auto* item = new QListWidgetItem(style()->standardIcon(icon), label, m_categories);
        item->setData(kCategoryRole, id);
        item->setSizeHint(QSize(0, 36));
        return item;
    };

    addCategory(generalCategory(), tr("General"), QStyle::SP_FileDialogContentsView);
    addCategory(historyCategory(), tr("History"), QStyle::SP_FileDialogInfoView);
    addCategory(extensionsCategory(), tr("Extensions"),
        QStyle::SP_FileDialogDetailedView);
    addCategory(profilesCategory(), tr("Profiles"), QStyle::SP_DirIcon);
    addCategory(appearanceCategory(), tr("Appearance"), QStyle::SP_DesktopIcon);

    m_generalPage = new GeneralPage(m_panes);
    m_paneForCategory.insert(generalCategory(), m_generalPage);
    m_panes->addWidget(m_generalPage);

    // Visited while this window was closed are recorded by the tabs, and the pane
    // holds no copy of the data, so it is refreshed each time the window opens.
    m_historyPage = new history::HistoryPage(m_history,
        [this](const QString& url) { m_window.openTab(url, true); }, m_panes);
    m_paneForCategory.insert(historyCategory(), m_historyPage);
    m_panes->addWidget(m_historyPage);

    auto* extensions = new ExtensionsPage(m_registry, m_panes);
    m_paneForCategory.insert(extensionsCategory(), extensions);
    m_panes->addWidget(extensions);

    // Kept, not just registered in the map: showEvent has to re-read it, because
    // the store is re-pointed at another profile's file on a profile switch and
    // the pane is built once.
    m_appearancePage = new AppearancePage(m_panes);
    m_paneForCategory.insert(appearanceCategory(), m_appearancePage);
    m_panes->addWidget(m_appearancePage);

    m_profilesPage = new ProfilesPage(m_panes);
    m_paneForCategory.insert(profilesCategory(), m_profilesPage);
    m_panes->addWidget(m_profilesPage);

    // Ctrl+W and Escape both close, matching the rest of the browser. Escape is
    // a QDialog default; the shortcut is here because the frameless window no
    // longer has a title-bar close button to fall back on.
    auto* closeShortcut = new QShortcut(QKeySequence::Close, this);
    connect(closeShortcut, &QShortcut::activated, this, &QDialog::accept);
}

void SettingsWindow::addWindowControls(QLayout* layout)
{
    if (!layout)
        return;
    const auto& theme = DecorationTheme::system();

    // The right-hand set, falling back to the left: a theme may put its controls
    // on either side, and the header has room for either.
    const DecorationTheme::Side side =
        theme.buttons(DecorationTheme::Side::Right).isEmpty()
        ? DecorationTheme::Side::Left : DecorationTheme::Side::Right;
    auto* decoration = new WindowDecoration(side, m_railHeader);
    // Initialised here rather than left to the first changeEvent: the buttons
    // are painted before any activation event arrives, and an unset
    // m_maximized would draw the restore artwork as though it were maximize.
    decoration->setWindowActive(isActiveWindow());
    decoration->setMaximized(isMaximized());

    connect(decoration, &WindowDecoration::minimizeRequested,
        this, &QWidget::showMinimized);
    connect(decoration, &WindowDecoration::maximizeRestoreRequested, this, [this] {
        isMaximized() ? showNormal() : showMaximized();
    });
    connect(decoration, &WindowDecoration::closeRequested, this, &QDialog::accept);

    // Added after the drag handle, so the controls sit against the window's right
    // edge and the draggable space is what is left over.
    layout->addWidget(decoration);
    m_decorations.append(decoration);
}

void SettingsWindow::selectCategory(const QString& id)
{
    for (int row = 0; row < m_categories->count(); ++row) {
        if (m_categories->item(row)->data(kCategoryRole).toString() == id) {
            m_categories->setCurrentRow(row);
            return;
        }
    }
}

void SettingsWindow::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    // Every page reads live state that changes while this window is closed, and
    // none of them holds a copy of it. Reloading all of them is what keeps a
    // setting changed elsewhere from showing its old value here.
    if (m_generalPage)
        m_generalPage->refresh();
    if (m_historyPage)
        m_historyPage->refresh();
    if (m_profilesPage)
        m_profilesPage->refresh();
    if (m_appearancePage)
        m_appearancePage->refresh();
}

void SettingsWindow::updateDecorationState()
{
    for (auto* decoration : m_decorations) {
        decoration->setWindowActive(isActiveWindow());
        decoration->setMaximized(isMaximized());
    }
}

void SettingsWindow::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange
        || event->type() == QEvent::ActivationChange) {
        updateDecorationState();
    }
}

void SettingsWindow::selectCategoryByIndex(int index)
{
    if (index < 0 || index >= m_categories->count())
        return;
    auto* item = m_categories->item(index);
    QWidget* pane = m_paneForCategory.value(item->data(kCategoryRole).toString(), nullptr);
    if (pane)
        m_panes->setCurrentWidget(pane);
}

} // namespace iridium
