#include "ui/settings/SettingsWindow.hpp"

#include "extensions/ExtensionRegistry.hpp"
#include "ui/MainWindow.hpp"
#include "ui/settings/AppearancePage.hpp"
#include "ui/settings/ExtensionsPage.hpp"
#include "ui/settings/GeneralPage.hpp"
#include "ui/settings/HistoryPage.hpp"
#include "ui/settings/ProfilesPage.hpp"

#include <QDialogButtonBox>
#include <QFont>
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
}

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

    buildUi();
    selectCategoryByIndex(0);
}

void SettingsWindow::buildUi()
{
    // Scoped to this dialog rather than the application: object names keep the
    // rules from leaking into the browser window and vice versa.
    setStyleSheet(QString::fromLatin1(R"(
        QDialog#settingsWindow { background: %1; }
        QListWidget#settingsCategories { background: transparent; border: 0; outline: 0; }
        QListWidget#settingsCategories::item { color: palette(text); border-radius: 8px;
            padding: 9px 12px; margin: 2px 0; }
        QListWidget#settingsCategories::item:hover { background: palette(midlight); }
        QListWidget#settingsCategories::item:selected { background: palette(highlight);
            color: palette(highlighted-text); }
        QStackedWidget#settingsPanes { background: transparent; }
        QWidget { background: transparent; }
        QLabel#settingsSubtitle, QLabel#settingsFootnote { color: palette(mid); }
        QLabel#settingsCaption { color: palette(mid); font-weight: 600; }
        QLabel#settingsValue { color: palette(text); }
        QLabel#settingsEmpty { color: palette(mid); padding: 24px; }
        QLabel#settingsStatus { color: palette(mid); padding: 6px 8px;
            border-radius: 6px; background: palette(base); }
        QLabel#settingsStatus[error="true"] { color: palette(brightText);
            background: #b3261e; border: 1px solid #8c1d18; }
        QListWidget#extensionsList { background: transparent; border: 0; outline: 0; }
        QListWidget#extensionsList::item { border-radius: 8px; padding: 7px 8px;
            color: palette(text); }
        QListWidget#extensionsList::item:hover { background: palette(midlight); }
        QListWidget#extensionsList::item:selected { background: palette(highlight);
            color: palette(highlighted-text); }
        QScrollArea#settingsDetailsScroll { background: transparent; border: 0; }
        QWidget#settingsDetails { background: palette(base); border-radius: 10px; }
        QLineEdit#settingsSearch { background: palette(base); border: 1px solid palette(mid);
            border-radius: 8px; padding: 6px 10px; color: palette(text); }
        QLineEdit#settingsSearch:focus { border-color: palette(highlight); }
        QComboBox#settingsCombo { background: palette(base); border: 1px solid palette(mid);
            border-radius: 8px; padding: 5px 10px; color: palette(text); }
        QToolButton#settingsButton { background: palette(base); color: palette(button-text);
            border: 1px solid palette(mid); border-radius: 8px; padding: 6px 14px; }
        QToolButton#settingsButton:hover { background: palette(midlight); }
        QToolButton#settingsButton:disabled { color: palette(disabled, palette(text)); }
    )").arg(palette().color(QPalette::Window).name()));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 12);
    root->setSpacing(12);

    auto* title = new QLabel(tr("Settings"), this);
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.5);
    titleFont.setBold(true);
    title->setFont(titleFont);
    root->addWidget(title);

    auto* body = new QHBoxLayout;
    body->setSpacing(16);

    m_categories = new QListWidget(this);
    m_categories->setObjectName(QStringLiteral("settingsCategories"));
    m_categories->setFixedWidth(190);
    m_categories->setSelectionMode(QAbstractItemView::SingleSelection);
    m_categories->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    body->addWidget(m_categories);

    // Connected once, before any row exists, so selecting a category cannot
    // arrive before there is a pane to show.
    connect(m_categories, &QListWidget::currentRowChanged,
        this, &SettingsWindow::selectCategoryByIndex);

    m_panes = new QStackedWidget(this);
    m_panes->setObjectName(QStringLiteral("settingsPanes"));
    body->addWidget(m_panes, 1);
    root->addLayout(body, 1);

    const auto addCategory = [this](const QString& id, const QString& label,
        QStyle::StandardPixmap icon) {
        auto* item = new QListWidgetItem(style()->standardIcon(icon), label, m_categories);
        item->setData(kCategoryRole, id);
        item->setSizeHint(QSize(0, 34));
        return item;
    };

    addCategory(generalCategory(), tr("General"), QStyle::SP_FileDialogContentsView);
    addCategory(historyCategory(), tr("History"), QStyle::SP_FileDialogInfoView);
    addCategory(extensionsCategory(), tr("Extensions"),
        QStyle::SP_FileDialogDetailedView);
    addCategory(profilesCategory(), tr("Profiles"), QStyle::SP_DirIcon);
    addCategory(appearanceCategory(), tr("Appearance"), QStyle::SP_DesktopIcon);

    auto* general = new GeneralPage(m_panes);
    m_paneForCategory.insert(generalCategory(), general);
    m_panes->addWidget(general);

    // Visited while this window was closed are recorded by the tabs, and the pane
    // holds no copy of the data, so it is refreshed each time the window opens.
    m_historyPage = new history::HistoryPage(m_history,
        [this](const QString& url) { m_window.openTab(url, true); }, m_panes);
    m_paneForCategory.insert(historyCategory(), m_historyPage);
    m_panes->addWidget(m_historyPage);

    auto* extensions = new ExtensionsPage(m_registry, m_panes);
    m_paneForCategory.insert(extensionsCategory(), extensions);
    m_panes->addWidget(extensions);

    auto* appearance = new AppearancePage(m_panes);
    m_paneForCategory.insert(appearanceCategory(), appearance);
    m_panes->addWidget(appearance);

    m_profilesPage = new ProfilesPage(m_panes);
    m_paneForCategory.insert(profilesCategory(), m_profilesPage);
    m_panes->addWidget(m_profilesPage);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    root->addWidget(buttons);

    // Ctrl+W and Escape both close, matching the rest of the browser.
    auto* closeShortcut = new QShortcut(QKeySequence::Close, this);
    connect(closeShortcut, &QShortcut::activated, this, &QDialog::accept);
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
    // Both pages read live state that the tabs change while this window is
    // closed, and neither holds a copy of it.
    if (m_historyPage)
        m_historyPage->refresh();
    if (m_profilesPage)
        m_profilesPage->refresh();
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