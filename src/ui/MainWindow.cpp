#include "ui/MainWindow.hpp"

#include "ui/BrowserStyle.hpp"

#include "browser/Browser.hpp"
#include "browser/HistoryStore.hpp"
#include "browser/ProfileManager.hpp"
#include "engine/webkit/WebKitView.hpp"
#include "extensions/ExtensionPaths.hpp"
#include "ui/decoration/decorationtheme.h"
#include "ui/decoration/windowdecoration.h"
#include "ui/settings/SettingsStore.hpp"
#include "ui/settings/SettingsWindow.hpp"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QApplication>
#include <QDesktopServices>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QProgressBar>
#include <QSvgRenderer>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <QWindow>
#include <QUrl>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <utility>

namespace iridium {

namespace {

// History retention, applied once per launch. Both bounds matter: the age
// sweep discards a long tail nobody will search, and the cap stops a heavy
// browsing day from growing the table without limit.
constexpr int kMaxHistoryEntries = 50000;
constexpr int kHistoryRetentionDays = 90;

QRect tabCloseRect(const QRect& row)
{
    return QRect(row.right() - 27, row.center().y() - 12, 24, 24);
}

class TabItemDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
        const QModelIndex& index) const override
    {
        // The item padding reserves the close-button space; the style handles
        // favicon placement and title elision within the remaining width.
        QStyledItemDelegate::paint(painter, option, index);

        const QPoint center = tabCloseRect(option.rect).center();
        const QColor color = option.state.testFlag(QStyle::State_Selected)
            ? option.palette.color(QPalette::HighlightedText)
            : option.palette.color(QPalette::Text);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap));
        painter->drawLine(center + QPoint(-3, -3), center + QPoint(3, 3));
        painter->drawLine(center + QPoint(3, -3), center + QPoint(-3, 3));
        painter->restore();
    }
};

class TabListWidget final : public QListWidget {
public:
    using QListWidget::QListWidget;
    std::function<void(int)> closeRequested;

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        const QModelIndex index = indexAt(event->position().toPoint());
        if (event->button() == Qt::LeftButton && index.isValid()
            && tabCloseRect(visualItemRect(item(index.row())))
                   .contains(event->position().toPoint())) {
            if (closeRequested)
                closeRequested(index.row());
            event->accept();
            return;
        }
        QListWidget::mousePressEvent(event);
    }
};

QIcon makePlusIcon(const QColor& color)
{
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(QPointF(8, 3), QPointF(8, 13));
    painter.drawLine(QPointF(3, 8), QPointF(13, 8));
    return QIcon(pixmap);
}

// A clock face, drawn rather than themed so the button is never empty when the
// icon theme has no view-history entry.
QIcon makeHistoryIcon(const QColor& color)
{
    constexpr int size = 16;
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 1.5));
    painter.drawEllipse(QRectF(2.0, 2.0, size - 4.0, size - 4.0));
    painter.drawLine(QPointF(8, 8), QPointF(8, 4.5));
    painter.drawLine(QPointF(8, 8), QPointF(11, 9.5));
    return QIcon(pixmap);
}

QIcon makeDownloadIcon(const QColor& color)
{
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawLine(QPointF(8, 2.5), QPointF(8, 10.5));
    painter.drawLine(QPointF(4.5, 7), QPointF(8, 10.5));
    painter.drawLine(QPointF(11.5, 7), QPointF(8, 10.5));
    painter.drawLine(QPointF(3, 13.5), QPointF(13, 13.5));
    return QIcon(pixmap);
}

} // namespace

MainWindow::MainWindow(Browser& browser, std::string initialUrl)
    : m_browser(browser)
    , m_initialUrl(std::move(initialUrl))
{
    setWindowTitle("Iridium");
    resize(1200, 800);
    // The shell, the drag region and edge resizing, and the stylesheet the
    // settings window also uses. FramelessChrome owns the event filter, so this
    // window no longer installs one itself.
    m_chrome = std::make_unique<ui::FramelessChrome>(this);
    setStyleSheet(ui::sharedStyleSheet(palette()));

    auto* root = new QWidget(this);
    root->setObjectName(QStringLiteral("browserRoot"));
    auto* rootLayout = new QHBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto* sidebar = new QWidget(root);
    sidebar->setObjectName(QStringLiteral("arcSidebar"));
    sidebar->setFixedWidth(276);
    auto* sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(8, 3, 8, 12);
    sidebarLayout->setSpacing(10);

    auto* header = new QWidget(sidebar);
    header->setObjectName(QStringLiteral("sidebarHeader"));
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(4);

    // Decoration widgets already lay out their own buttons. Keep them and the
    // navigation buttons directly in the header, without grouping containers.
    const auto addDecoration = [this, header](DecorationTheme::Side side) -> WindowDecoration* {
        if (DecorationTheme::system().buttons(side).isEmpty())
            return nullptr;
        auto* decoration = new WindowDecoration(side, header);
        m_decorations.append(decoration);
        connect(decoration, &WindowDecoration::minimizeRequested,
            this, &QWidget::showMinimized);
        connect(decoration, &WindowDecoration::maximizeRestoreRequested, this, [this] {
            isMaximized() ? showNormal() : showMaximized();
        });
        connect(decoration, &WindowDecoration::closeRequested, this, &QWidget::close);
        decoration->setWindowActive(isActiveWindow());
        decoration->setMaximized(isMaximized());
        return decoration;
    };

    if (auto* left = addDecoration(DecorationTheme::Side::Left))
        headerLayout->addWidget(left);

    auto makeNavigationButton = [this, header](const QString& tooltip, QStyle::StandardPixmap icon) {
        auto* button = new QToolButton(header);
        button->setObjectName(QStringLiteral("chromeButton"));
        button->setToolTip(tooltip);
        button->setFixedSize(30, 30);
        button->setIcon(style()->standardIcon(icon));
        return button;
    };
    m_backButton = makeNavigationButton(QStringLiteral("Back"), QStyle::SP_ArrowBack);
    m_forwardButton = makeNavigationButton(QStringLiteral("Forward"), QStyle::SP_ArrowForward);
    m_reloadButton = makeNavigationButton(QStringLiteral("Reload"), QStyle::SP_BrowserReload);
    headerLayout->addWidget(m_backButton);
    headerLayout->addWidget(m_forwardButton);
    headerLayout->addWidget(m_reloadButton);

    auto* dragHandle = m_chrome->dragHandle();
    dragHandle->setParent(header);
    dragHandle->setMinimumWidth(8);
    headerLayout->addWidget(dragHandle, 1);

    if (auto* right = addDecoration(DecorationTheme::Side::Right))
        headerLayout->addWidget(right);

    sidebarLayout->addWidget(header);

    m_addressBar = new QLineEdit(sidebar);
    m_addressBar->setObjectName(QStringLiteral("addressBar"));
    m_addressBar->setPlaceholderText(QStringLiteral("Search or enter website"));
    m_addressBar->setClearButtonEnabled(true);
    sidebarLayout->addWidget(m_addressBar);

    auto* tabs = new TabListWidget(sidebar);
    m_tabs = tabs;
    m_tabs->setItemDelegate(new TabItemDelegate(m_tabs));
    m_tabs->setObjectName(QStringLiteral("tabList"));
    m_tabs->setFrameShape(QFrame::NoFrame);
    m_tabs->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_tabs->setIconSize(QSize(16, 16));
    m_tabs->setUniformItemSizes(true);
    m_tabs->setWordWrap(false);
    m_tabs->setTextElideMode(Qt::ElideRight);
    m_tabs->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tabs->setDragDropMode(QAbstractItemView::InternalMove);
    m_tabs->setDefaultDropAction(Qt::MoveAction);
    m_tabs->setSpacing(2);
    sidebarLayout->addWidget(m_tabs, 1);
    m_networkManager = new QNetworkAccessManager(this);

    // One registry and host for the window; extensions are discovered from the
    // XDG paths so a user drop-in is picked up on the next launch.
    //
    // The profile is the root of per-profile state, so everything below hangs
    // off ProfileManager::current() rather than a hard-coded default.
    m_profile = &ProfileManager::instance().current();
    SettingsStore::instance().pointAtProfile(m_profile->path);

    m_extensionRegistry = std::make_unique<extensions::ExtensionRegistry>(
        m_profile->path);
    m_extensionHost = std::make_unique<extensions::ExtensionHost>(
        *m_extensionRegistry, &browser, this);
    m_extensionHost->loadDefaults();

    m_history = std::make_unique<history::HistoryStore>(*m_profile);
    QString historyError;
    if (!m_history->open(&historyError)) {
        reportHistoryProblem(historyError);
    } else {
        // Swept once per launch. Doing it here rather than on every visit keeps
        // the retention policy off the path a page load depends on, and a
        // startup sweep is enough because the table only grows between runs.
        QString pruneError;
        m_history->prune(kMaxHistoryEntries, kHistoryRetentionDays,
            &pruneError);
    }

    // The download preference lives in the settings layer; hand the engine a
    // way to read it rather than importing settings from the engine.
    engine::webkit::WebKitView::setDownloadDirectoryProvider([] {
        const QString configured = SettingsStore::instance().downloadDirectory();
        return configured.isEmpty() ? QString() : configured;
    });

    auto* sidebarActions = new QHBoxLayout;
    sidebarActions->setSpacing(2);

    const auto makeSidebarAction = [sidebar](const QString& tooltip, const QIcon& icon) {
        auto* button = new QToolButton(sidebar);
        button->setObjectName(QStringLiteral("sidebarAction"));
        button->setToolTip(tooltip);
        button->setIcon(icon);
        button->setCursor(Qt::PointingHandCursor);
        return button;
    };
    const QColor buttonColor = palette().color(QPalette::ButtonText);

    auto* addTabButton = makeSidebarAction(QStringLiteral("New tab (Ctrl+T)"),
        QIcon::fromTheme(QStringLiteral("tab-new"), makePlusIcon(buttonColor)));
    sidebarActions->addWidget(addTabButton);

    auto* historyButton = makeSidebarAction(QStringLiteral("History"),
        QIcon::fromTheme(QStringLiteral("view-history"), makeHistoryIcon(buttonColor)));
    connect(historyButton, &QToolButton::clicked, this, &MainWindow::openHistory);
    sidebarActions->addWidget(historyButton);

    sidebarActions->addStretch(1);

    auto* settingsButton = makeSidebarAction(QStringLiteral("Settings"),
        QIcon::fromTheme(QStringLiteral("configure"),
            style()->standardIcon(QStyle::SP_FileDialogDetailedView)));
    connect(settingsButton, &QToolButton::clicked, this, &MainWindow::openSettings);
    sidebarActions->addWidget(settingsButton);

    m_downloadButton = makeSidebarAction(QStringLiteral("Downloads"),
        QIcon::fromTheme(QStringLiteral("download"), makeDownloadIcon(buttonColor)));
    m_downloadsMenu = new QMenu(m_downloadButton);
    m_downloadButton->setMenu(m_downloadsMenu);
    m_downloadButton->setPopupMode(QToolButton::InstantPopup);
    connect(m_downloadsMenu, &QMenu::aboutToShow,
        this, &MainWindow::rebuildDownloadsMenu);
    sidebarActions->addWidget(m_downloadButton);
    sidebarLayout->addLayout(sidebarActions);

    m_downloadsRefresh = new QTimer(this);
    m_downloadsRefresh->setSingleShot(true);
    m_downloadsRefresh->setInterval(150);
    connect(m_downloadsRefresh, &QTimer::timeout, this, [this] {
        if (m_downloadsMenu->isVisible())
            rebuildDownloadsMenu();
    });

    auto* rendererFrame = new QFrame(root);
    rendererFrame->setObjectName(QStringLiteral("rendererFrame"));
    auto* rendererLayout = new QVBoxLayout(rendererFrame);
    rendererLayout->setContentsMargins(0, 3, 3, 3);
    rendererLayout->setSpacing(0);

    m_pages = new QStackedWidget(rendererFrame);
    m_pages->setObjectName(QStringLiteral("rendererPages"));
    auto* empty = new QLabel(m_pages);
    empty->setObjectName(QStringLiteral("noTabsState"));
    empty->setAlignment(Qt::AlignCenter);
    m_emptyState = empty;
    m_pages->addWidget(m_emptyState);
    rendererLayout->addWidget(m_pages);

    // The page takes the space, and the sidebar sits against the right edge.
    // Order in this layout is what decides which edge the sidebar hugs, so the
    // sidebar is added last.
    rootLayout->addWidget(rendererFrame, 1);
    rootLayout->addWidget(sidebar);

    setCentralWidget(root);

    connect(addTabButton, &QToolButton::clicked, this, &MainWindow::newTab);
    tabs->closeRequested = [this](int row) { closeTab(row); };
    connect(m_tabs, &QListWidget::currentRowChanged, this, [this](int index) {
        if (index < 0 || index >= m_views.size()) {
            m_pages->setCurrentWidget(m_emptyState);
            m_addressBar->clear();
        } else {
            m_pages->setCurrentWidget(m_views[index]->widget());
            m_addressBar->setText(m_uris.value(index));
            updateNavigationButtons();
        }
    });
    connect(m_tabs->model(), &QAbstractItemModel::rowsMoved, this,
        [this](const QModelIndex&, int first, int last, const QModelIndex&, int destination) {
            if (first != last)
                return;
            const int to = destination > first ? destination - 1 : destination;
            if (to < 0 || to >= m_views.size() || to == first)
                return;
            auto* page = m_views[first]->widget();
            m_pages->removeWidget(page);
            m_views.move(first, to);
            m_uris.move(first, to);
            m_faviconUrls.move(first, to);
            m_pages->insertWidget(to + 1, page);
            m_browser.moveTab(first, to);
            const int current = m_tabs->currentRow();
            if (current >= 0)
                m_pages->setCurrentWidget(m_views[current]->widget());
        });
    connect(m_addressBar, &QLineEdit::returnPressed, this, &MainWindow::navigateToAddress);
    connect(m_backButton, &QToolButton::clicked, this, [this] {
        const int row = m_tabs->currentRow();
        if (row >= 0) m_browser.goBack(m_views[row]);
        updateNavigationButtons();
    });
    connect(m_forwardButton, &QToolButton::clicked, this, [this] {
        const int row = m_tabs->currentRow();
        if (row >= 0) m_browser.goForward(m_views[row]);
        updateNavigationButtons();
    });
    connect(m_reloadButton, &QToolButton::clicked, this, [this] {
        const int row = m_tabs->currentRow();
        if (row >= 0) m_browser.reload(m_views[row]);
    });

    auto* createShortcut = new QShortcut(QKeySequence::AddTab, this);
    connect(createShortcut, &QShortcut::activated, this, &MainWindow::newTab);
    auto* closeShortcut = new QShortcut(QKeySequence::Close, this);
    connect(closeShortcut, &QShortcut::activated, this, [this] { closeTab(m_tabs->currentRow()); });
    auto* focusAddressShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+L")), this);
    connect(focusAddressShortcut, &QShortcut::activated, m_addressBar, [this] {
        m_addressBar->setFocus();
        m_addressBar->selectAll();
    });
    auto* switchNextShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Tab")), this);
    connect(switchNextShortcut, &QShortcut::activated, this, [this] {
        if (m_tabs->count()) m_tabs->setCurrentRow((m_tabs->currentRow() + 1) % m_tabs->count());
    });

    auto* settingsShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+,")), this);
    connect(settingsShortcut, &QShortcut::activated, this, &MainWindow::openSettings);

    auto* historyShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+H")), this);
    connect(historyShortcut, &QShortcut::activated, this, &MainWindow::openHistory);

    connect(&SettingsStore::instance(), &SettingsStore::forcedColorSchemeChanged,
        this, [this] { applyColorScheme(); });

    applyColorScheme();
    updateDecorationState();
    newTab();
}

MainWindow::~MainWindow()
{
    releaseTabs();
}

void MainWindow::releaseTabs()
{
    // closeTab() removes from m_views, so iterate over a copy: the vector is
    // mutated as we go.
    const QList<engine::WebView*> views = m_views;
    for (auto* view : views) {
        if (!view)
            continue;
        m_views.removeAll(view);
        // Same as closeTab(): the widget must leave the stack, or it stays a
        // child of this window and Qt deletes it after Browser has handed the
        // view to its pool.
        m_pages->removeWidget(view->widget());
        // The widget may be pooled for reuse, so it must not stay a child of
        // this window. Browser::recycle does the unparenting; doing it here too
        // would detach a view that is about to be destroyed.
        m_extensionHost->detachView(view);
        m_browser.closeTab(view);
    }
    m_views.clear();
}

void MainWindow::applyColorScheme()
{
    // Apply to every open view, not just future ones, so a change in settings
    // takes effect on tabs that are already loaded.
    const std::optional<bool> forced = SettingsStore::instance().forcedColorScheme();
    for (auto* view : m_views) {
        if (view)
            view->setForcedColorScheme(forced);
    }
}

// Tab ids handed to extensions. Derived from the view pointer rather than
// stored, because the view is stable for a tab's lifetime and the pool reuses
// views only after they leave the list.
int MainWindow::tabIdForRow(int row) const
{
    if (row < 0 || row >= m_views.size())
        return -1;
    auto* view = m_views.at(row);
    return static_cast<int>(reinterpret_cast<quintptr>(view));
}

int MainWindow::rowForTabId(int id) const
{
    for (int row = 0; row < m_views.size(); ++row) {
        if (tabIdForRow(row) == id)
            return row;
    }
    return -1;
}

int MainWindow::rowForView(engine::WebView* view) const
{
    return m_views.indexOf(view);
}

int MainWindow::currentTabRow() const
{
    return m_tabs ? m_tabs->currentRow() : -1;
}

std::vector<extensions::TabSnapshot> MainWindow::tabSnapshots() const
{
    std::vector<extensions::TabSnapshot> result;
    result.reserve(m_views.size());
    for (int row = 0; row < m_views.size(); ++row) {
        auto* view = m_views.at(row);
        extensions::TabSnapshot snapshot;
        snapshot.id = tabIdForRow(row);
        snapshot.windowId = 1;
        snapshot.active = row == m_tabs->currentRow();
        snapshot.index = row;
        if (view) {
            snapshot.url = QString::fromStdString(view->currentUrl());
            if (auto* item = m_tabs->item(row))
                snapshot.title = item->text();
            if (row < m_faviconUrls.size())
                snapshot.favIconUrl = m_faviconUrls.at(row);
        }
        result.push_back(std::move(snapshot));
    }
    return result;
}

std::optional<extensions::TabSnapshot> MainWindow::tabSnapshot(int id) const
{
    const int row = rowForTabId(id);
    if (row < 0)
        return std::nullopt;
    const std::vector<extensions::TabSnapshot> all = tabSnapshots();
    if (row >= static_cast<int>(all.size()))
        return std::nullopt;
    return all[static_cast<std::size_t>(row)];
}

// Browser calls this for extensions::ExtensionApiHost::removeTab.
void MainWindow::closeTabByIndex(int index)
{
    closeTab(index);
}

void MainWindow::selectTab(int row)
{
    if (row < 0 || row >= m_views.size())
        return;
    m_tabs->setCurrentRow(row);
    m_pages->setCurrentWidget(m_views.at(row)->widget());
    updateNavigationButtons();
}

void MainWindow::navigateRow(int row, const QString& uri)
{
    if (row < 0 || row >= m_views.size())
        return;
    if (auto* view = m_views.at(row))
        m_browser.navigate(view, uri.toStdString());
}

void MainWindow::openTab(const QString& url, bool active)
{
    newTab();
    const int row = m_views.size() - 1;
    if (row < 0)
        return;
    if (!url.isEmpty())
        navigateRow(row, url);
    if (active)
        selectTab(row);
}

void MainWindow::recordVisit(engine::WebView* view, const QString& url, const QString& title)
{
    if (!m_history || !m_history->isOpen())
        return;

    const int row = m_views.indexOf(view);
    if (row < 0 || row >= m_uris.size())
        return;

    // Either half may be supplied. Navigation supplies the URL with no title,
    // because the tab still shows the *previous* page's title at that point and
    // using it would mislabel the entry. The titleChanged handler supplies the
    // title a moment later and fills the entry in.
    const QString target = url.isEmpty() ? m_uris.at(row) : url;
    const QString pageTitle = url.isEmpty() ? title : QString();

    // Internal pages are not part of a browsing history, and neither are blank
    // tabs: recording them would fill it with noise.
    if (target.isEmpty() || !isHistoryWorthy(target))
        return;

    // A view with no title yet still records, so a page that never sets one is
    // still findable by URL.
    QString error;
    if (!m_history->recordVisit(target, pageTitle, &error))
        reportHistoryProblem(error);
}

bool MainWindow::isHistoryWorthy(const QString& url)
{
    static const QStringList excluded = {
        QStringLiteral("about:"),
        QStringLiteral("iridium-extension:"),
        QStringLiteral("data:"),
        QStringLiteral("blob:"),
        QStringLiteral("javascript:"),
        QStringLiteral("chrome:"),
    };
    for (const QString& scheme : excluded) {
        if (url.startsWith(scheme))
            return false;
    }
    return true;
}

void MainWindow::reportHistoryProblem(const QString& message)
{
    // History failing is not worth interrupting the user over: browsing carries
    // on without it. Surfaced once so a persistent problem is visible.
    if (m_historyErrorShown)
        return;
    m_historyErrorShown = true;
    std::fprintf(stderr, "iridium: history unavailable: %s\n",
        message.toUtf8().constData());
}

void MainWindow::openHistory()
{
    if (!m_settingsWindow) {
        m_settingsWindow = new SettingsWindow(
            *this, *m_extensionRegistry, *m_history, this);
    }
    // The history list lives in the same window as the other settings, so a
    // visitor opens once and can switch to extensions or preferences.
    m_settingsWindow->selectCategory(
        QString(SettingsWindow::historyCategory()));
    m_settingsWindow->show();
    m_settingsWindow->raise();
    m_settingsWindow->activateWindow();
}

void MainWindow::newTab()
{
    // The command-line URL wins for the first tab only; afterwards the homepage
    // preference applies, so a configured start page is actually used.
    //
    // The homepage goes through resolveAddressInput, like anything else typed in
    // the address bar. Without that, a homepage stored as "example.com" was
    // handed to the engine verbatim, which cannot load a bare host, so a
    // plausible setting did nothing visible.
    std::string uri = std::exchange(m_initialUrl, std::string());
    if (uri.empty()) {
        uri = SettingsStore::instance()
            .resolveAddressInput(SettingsStore::instance().homePage()).toStdString();
    }
    auto* view = m_browser.newTab();
    m_pages->addWidget(view->widget());
    m_views.append(view);
    // New tabs follow the current preference rather than the system default.
    view->setForcedColorScheme(SettingsStore::instance().forcedColorScheme());
    m_uris.append(QString{});
    m_faviconUrls.append(QString{});
    auto* item = new QListWidgetItem(QStringLiteral("New Tab"));
    item->setData(Qt::UserRole, QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(view)));
    m_tabs->addItem(item);
    m_tabs->setCurrentRow(m_views.size() - 1);
    view->setTitleChangedHandler([this, view](const std::string& rawTitle) {
        const int row = m_views.indexOf(view);
        if (row >= 0)
            m_tabs->item(row)->setText(rawTitle.empty() ? QStringLiteral("New Tab")
                                                        : QString::fromUtf8(rawTitle));
        // A title that arrives after the visit fills in the entry rather than
        // adding a second one.
        recordVisit(view, QString{}, QString::fromUtf8(rawTitle));
    });
    view->setUriChangedHandler([this, view](const std::string& uri) {
        const int row = m_views.indexOf(view);
        const QString url = QString::fromUtf8(uri);
        if (row >= 0)
            m_uris[row] = url;
        if (row == m_tabs->currentRow())
            m_addressBar->setText(url);
        updateNavigationButtons();
        // Recorded on navigation, when the URL is known but the title may not be
        // yet; the title arrives separately and updates the same entry.
        recordVisit(view, url, QString{});
    });
    view->setFaviconUrlChangedHandler([this, view](const std::string& rawUrl) {
        const int row = m_views.indexOf(view);
        if (row < 0)
            return;
        const QString url = QString::fromUtf8(rawUrl);
        m_faviconUrls[row] = url;
        auto* reply = m_networkManager->get(QNetworkRequest(QUrl(url)));
        connect(reply, &QNetworkReply::finished, this, [this, reply, view, url] {
            const int currentRow = m_views.indexOf(view);
            if (currentRow >= 0 && m_faviconUrls.value(currentRow) == url
                && reply->error() == QNetworkReply::NoError) {
                const QByteArray bytes = reply->readAll();
                QImage image;
                if (!image.loadFromData(bytes)) {
                    QSvgRenderer svg(bytes);
                    if (svg.isValid()) {
                        image = QImage(64, 64, QImage::Format_ARGB32_Premultiplied);
                        image.fill(Qt::transparent);
                        QPainter painter(&image);
                        svg.render(&painter);
                    }
                }
                if (!image.isNull())
                    m_tabs->item(currentRow)->setIcon(QIcon(QPixmap::fromImage(
                        image.scaled(24, 24, Qt::KeepAspectRatio, Qt::SmoothTransformation))));
            }
            reply->deleteLater();
        });
    });
    view->setDownloadStateHandler([this, view](const engine::DownloadState& state) {
        updateDownload(view, state);
    });

    // Extensions need the view attached before the document loads, so the
    // shim and content scripts are in place at document start.
    m_extensionHost->attachView(view);

    QTimer::singleShot(0, view->widget(), [this, view, uri = std::move(uri)] {
        if (!m_views.contains(view))
            return;
        // Re-inject now that the URI is known, then navigate.
        m_extensionHost->refreshView(view);
        if (!uri.empty())
            m_browser.navigate(view, uri);
    });
}

void MainWindow::closeTab(int index)
{
    if (index < 0 || index >= m_views.size())
        return;
    auto* view = m_views.takeAt(index);
    // A closed view is kept for reuse, possibly past this window. Drop the
    // handlers that capture `this` so nothing runs against a stale window
    // while the view sits in the pool; newTab() installs fresh ones.
    view->setTitleChangedHandler(nullptr);
    view->setUriChangedHandler(nullptr);
    view->setFaviconUrlChangedHandler(nullptr);
    view->setDownloadStateHandler(nullptr);
    for (auto it = m_downloads.begin(); it != m_downloads.end();) {
        if (it->view == view)
            it = m_downloads.erase(it);
        else
            ++it;
    }
    updateDownloadButtonToolTip();
    m_uris.removeAt(index);
    m_faviconUrls.removeAt(index);
    {
        const QSignalBlocker blocker(m_tabs);
        delete m_tabs->takeItem(index);
    }
    m_pages->removeWidget(view->widget());
    // Drop the extension bridge before the view is recycled, so a pooled view
    // cannot keep dispatching API calls for a tab that no longer exists.
    m_extensionHost->detachView(view);
    m_browser.closeTab(view);
    if (m_views.isEmpty()) {
        m_pages->setCurrentWidget(m_emptyState);
        m_addressBar->clear();
        updateNavigationButtons();
        return;
    }
    const int current = qBound(0, m_tabs->currentRow(), m_views.size() - 1);
    {
        const QSignalBlocker blocker(m_tabs);
        m_tabs->setCurrentRow(current);
    }
    m_pages->setCurrentWidget(m_views[current]->widget());
    updateNavigationButtons();
}

void MainWindow::navigateToAddress()
{
    const int row = m_tabs->currentRow();
    if (row < 0)
        return;
    // Goes through SettingsStore so a bare word searches and a hostname is
    // treated as a URL, according to the configured template.
    const QString resolved = SettingsStore::instance().resolveAddressInput(
        m_addressBar->text());
    if (!resolved.isEmpty())
        m_browser.navigate(m_views[row], resolved.toStdString());
}

void MainWindow::updateNavigationButtons()
{
    const int row = m_tabs->currentRow();
    const bool hasTab = row >= 0 && row < m_views.size();
    m_backButton->setEnabled(hasTab && m_views[row]->canGoBack());
    m_forwardButton->setEnabled(hasTab && m_views[row]->canGoForward());
    m_reloadButton->setEnabled(hasTab);
    m_addressBar->setEnabled(hasTab);
}

void MainWindow::updateDownload(engine::WebView* view, const engine::DownloadState& state)
{
    DownloadEntry entry;
    entry.view = view;
    entry.state = state;
    m_downloads.insert(state.id, entry);
    pruneDownloadHistory();
    updateDownloadButtonToolTip();
    if (m_downloadsMenu->isVisible())
        m_downloadsRefresh->start();
}

void MainWindow::pruneDownloadHistory()
{
    // Without a cap the map keeps every download for the life of the process.
    // Only completed entries are dropped, and ids increase monotonically, so
    // trimming the smallest ids discards the oldest results while every
    // in-flight download is kept.
    QList<std::uint64_t> completed;
    for (auto it = m_downloads.begin(); it != m_downloads.end(); ++it) {
        if (it->state.finished || it->state.failed)
            completed.append(it.key());
    }
    if (completed.size() <= kMaxCompletedDownloads)
        return;

    std::sort(completed.begin(), completed.end());
    const int excess = completed.size() - kMaxCompletedDownloads;
    for (int index = 0; index < excess; ++index)
        m_downloads.remove(completed.at(index));
}

void MainWindow::openSettings()
{
    // Modeless-on-demand: the dialog is created on first use and kept until the
    // browser window goes away, so the category selection and scroll position
    // survive being closed and reopened.
    //
    // It is deliberately NOT WA_DeleteOnClose. That attribute deletes the dialog
    // on close, and this pointer is not cleared when it happens, so the second
    // call would use a dangling pointer: the cached non-null check would pass
    // and show() would run on freed memory. Parenting it to this window gives
    // the same "no leak" outcome without the stale pointer.
    if (!m_settingsWindow)
        m_settingsWindow = new SettingsWindow(
            *this, *m_extensionRegistry, *m_history, this);
    m_settingsWindow->show();
    m_settingsWindow->raise();
    m_settingsWindow->activateWindow();
}

void MainWindow::updateDownloadButtonToolTip()
{
    int active = 0;
    for (const auto& entry : m_downloads) {
        if (!entry.state.finished && !entry.state.failed)
            ++active;
    }
    m_downloadButton->setToolTip(active > 0
        ? QStringLiteral("Downloads (%1 active)").arg(active)
        : QStringLiteral("Downloads"));
}

void MainWindow::rebuildDownloadsMenu()
{
    m_downloadsMenu->clear();
    if (m_downloads.isEmpty()) {
        auto* empty = m_downloadsMenu->addAction(QStringLiteral("No downloads yet"));
        empty->setEnabled(false);
        return;
    }

    QList<std::uint64_t> ids = m_downloads.keys();
    std::sort(ids.begin(), ids.end(), std::greater<std::uint64_t>());

    for (const std::uint64_t id : ids) {
        const DownloadEntry entry = m_downloads.value(id);
        auto* row = new QWidget(m_downloadsMenu);
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(8, 4, 8, 4);
        layout->setSpacing(8);

        auto* name = new QLabel(row);
        const QString fullName = entry.state.fileName.empty()
            ? QStringLiteral("Download")
            : QString::fromStdString(entry.state.fileName);
        name->setText(name->fontMetrics().elidedText(fullName, Qt::ElideMiddle, 180));
        name->setToolTip(QString::fromStdString(entry.state.destination));
        layout->addWidget(name, 1);

        if (entry.state.failed) {
            auto* status = new QLabel(QStringLiteral("Failed"), row);
            status->setToolTip(QString::fromStdString(entry.state.error));
            layout->addWidget(status);
        } else if (entry.state.finished) {
            auto* open = new QToolButton(row);
            open->setText(QStringLiteral("Open"));
            connect(open, &QToolButton::clicked, this,
                [this, destination = QString::fromStdString(entry.state.destination)] {
                    m_downloadsMenu->hide();
                    QDesktopServices::openUrl(QUrl::fromLocalFile(destination));
                });
            layout->addWidget(open);
        } else {
            auto* progress = new QProgressBar(row);
            progress->setRange(0, 100);
            progress->setValue(static_cast<int>(entry.state.progress * 100.0));
            progress->setFixedWidth(90);
            layout->addWidget(progress);

            auto* cancel = new QToolButton(row);
            cancel->setText(QStringLiteral("Cancel"));
            connect(cancel, &QToolButton::clicked, this,
                [this, id, view = entry.view] {
                    m_downloadsMenu->hide();
                    if (view)
                        view->cancelDownload(id);
                });
            layout->addWidget(cancel);
        }

        auto* action = new QWidgetAction(m_downloadsMenu);
        action->setDefaultWidget(row);
        m_downloadsMenu->addAction(action);
    }

    const bool hasCompleted = std::any_of(m_downloads.begin(), m_downloads.end(),
        [](const DownloadEntry& entry) { return entry.state.finished || entry.state.failed; });
    if (hasCompleted) {
        m_downloadsMenu->addSeparator();
        auto* clear = m_downloadsMenu->addAction(QStringLiteral("Clear finished"));
        connect(clear, &QAction::triggered, this, [this] {
            for (auto it = m_downloads.begin(); it != m_downloads.end();) {
                if (it->state.finished || it->state.failed)
                    it = m_downloads.erase(it);
                else
                    ++it;
            }
            updateDownloadButtonToolTip();
            rebuildDownloadsMenu();
        });
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // The setting says "several tabs", so one tab is not several and is not
    // worth interrupting anyone over.
    if (m_views.size() > 1
        && SettingsStore::instance().confirmBeforeClosingTabs()) {
        const auto answer = QMessageBox::question(this, tr("Close window"),
            tr("Close this window and its %n tab(s)?", "", int(m_views.size())),
            QMessageBox::Close | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Close) {
            // Ignored rather than accepted: the window stays open.
            event->ignore();
            return;
        }
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::updateDecorationState()
{
    for (auto* decoration : m_decorations) {
        decoration->setWindowActive(isActiveWindow());
        decoration->setMaximized(isMaximized());
    }
}

void MainWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange || event->type() == QEvent::ActivationChange)
        updateDecorationState();
}

} // namespace iridium
