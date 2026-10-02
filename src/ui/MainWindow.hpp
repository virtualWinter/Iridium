#pragma once

#include <QHash>
#include <QMainWindow>
#include <QList>
#include <QStringList>

#include <cstdint>

#include "browser/HistoryStore.hpp"
#include "browser/ProfileManager.hpp"
#include "engine/WebView.hpp"
#include "extensions/ExtensionApi.hpp"
#include "extensions/ExtensionHost.hpp"
#include "extensions/ExtensionRegistry.hpp"

#include <memory>
#include <optional>
#include <vector>

class QListWidget;
class QLineEdit;
class QMenu;
class QStackedWidget;
class QTimer;
class QToolButton;
class QNetworkAccessManager;
class WindowDecoration;
namespace iridium {

class Browser;
class SettingsWindow;
namespace ui {
class FramelessChrome;
}

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(Browser& browser, std::string initialUrl = "https://example.com");
    ~MainWindow() override;

    // Closes every tab this window still holds, so Browser's ownership stays
    // consistent with what Qt owns. Called by the destructor, and by Browser
    // before it releases the views itself, so whichever runs first leaves both
    // sides agreeing.
    void releaseTabs();

protected:
    void changeEvent(QEvent* event) override;
    // Honours the "confirm before closing several tabs" preference. Without it
    // the checkbox in the general pane would persist a value nothing reads.
    void closeEvent(QCloseEvent* event) override;

public:
    // Tab state for the extensions API layer, which Browser forwards to here
    // because MainWindow owns the tab list and the widgets.
    std::vector<extensions::TabSnapshot> tabSnapshots() const;
    std::optional<extensions::TabSnapshot> tabSnapshot(int id) const;
    int rowForView(engine::WebView* view) const;
    int rowForTabId(int id) const;
    int tabIdForRow(int row) const;
    // Selected row, or -1 when no tab is selected.
    int currentTabRow() const;
    int tabCount() const { return static_cast<int>(m_views.size()); }
    // Opens a tab at `url`, selecting it when `active`.
    void openTab(const QString& url, bool active);
    void navigateRow(int row, const QString& uri);
    void selectTab(int row);
    // Closes a tab by row, for the extensions API layer. Browser forwards here
    // rather than reaching into closeTab().
    void closeTabByIndex(int index);

private:

private:
    void newTab();
    void closeTab(int index);
    void updateDecorationState();
    void updateNavigationButtons();
    void navigateToAddress();
    void openSettings();
    void openHistory();
    void applyColorScheme();

    // Records a visit, called from the URI and title handlers. Either half may
    // be supplied; see the definition for why.
    void recordVisit(engine::WebView* view, const QString& url, const QString& title);
    // False for internal pages, which do not belong in a browsing history.
    static bool isHistoryWorthy(const QString& url);
    // History failures are reported once rather than interrupting browsing.
    void reportHistoryProblem(const QString& message);
    void updateDownload(engine::WebView* view, const engine::DownloadState& state);
    void pruneDownloadHistory();
    void rebuildDownloadsMenu();
    void updateDownloadButtonToolTip();

    // Caps the retained download history; in-flight downloads are never
    // dropped, only the oldest finished/failed entries.
    static constexpr int kMaxCompletedDownloads = 100;

    struct DownloadEntry {
        engine::WebView* view { nullptr };
        engine::DownloadState state;
    };

    QListWidget* m_tabs { nullptr };
    QStackedWidget* m_pages { nullptr };
    QWidget* m_emptyState { nullptr };
    QLineEdit* m_addressBar { nullptr };
    QToolButton* m_backButton { nullptr };
    QToolButton* m_forwardButton { nullptr };
    QToolButton* m_reloadButton { nullptr };
    QToolButton* m_downloadButton { nullptr };
    QToolButton* m_menuButton { nullptr };
    QMenu* m_downloadsMenu { nullptr };
    QTimer* m_downloadsRefresh { nullptr };
    QHash<std::uint64_t, DownloadEntry> m_downloads;
    Browser& m_browser;
    std::string m_initialUrl;
    QList<engine::WebView*> m_views;
    QList<QString> m_uris;
    QStringList m_faviconUrls;
    QList<WindowDecoration*> m_decorations;
    // The frameless shell: the translucency, the drag region, edge resizing and
    // the shared stylesheet. Held here so it outlives the widgets it filters
    // events for.
    std::unique_ptr<ui::FramelessChrome> m_chrome;
    QNetworkAccessManager* m_networkManager { nullptr };
    // Owned here so the window controls their lifetime; created in the
    // constructor once the tab list exists.
    std::unique_ptr<extensions::ExtensionRegistry> m_extensionRegistry;
    std::unique_ptr<extensions::ExtensionHost> m_extensionHost;
    // Per-profile browsing history. Null-safe: a failure to open it degrades to
    // no history rather than breaking browsing.
    std::unique_ptr<history::HistoryStore> m_history;
    const Profile* m_profile { nullptr };
    bool m_historyErrorShown { false };
    // Created on first use, then reused; parented to this window and deleted
    // with it.
    SettingsWindow* m_settingsWindow { nullptr };
};

} // namespace iridium
