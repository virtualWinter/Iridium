#include "browser/Browser.hpp"

#include "engine/webkit/WebKitView.hpp"

#include <algorithm>
#include <utility>

namespace iridium {

Browser::Browser(std::string initialUrl)
    : m_window(*this, std::move(initialUrl))
{
}

Browser::~Browser()
{
    // A MainWindow owns the widgets its views are parented into (addWidget
    // reparents), so Qt would delete a view along with the window while the
    // unique_ptrs here still own it. Hand the views back first: MainWindow
    // closes its own tabs, which empties both lists consistently whichever of
    // the two paths runs first.
    m_window.releaseTabs();

    // Anything still held here (a pool entry has no MainWindow to close it)
    // must be detached before the unique_ptrs destroy it.
    const auto detach = [](const std::vector<std::unique_ptr<engine::WebView>>& views) {
        for (const auto& view : views) {
            if (view) {
                if (auto* page = view->widget())
                    page->setParent(nullptr);
            }
        }
    };
    detach(m_tabs);
    detach(m_pool);

    m_tabs.clear();
    m_pool.clear();
}

engine::WebView* Browser::newTab()
{
    std::unique_ptr<engine::WebView> tab;
    if (!m_pool.empty()) {
        tab = std::move(m_pool.back());
        m_pool.pop_back();
    } else {
        tab = std::make_unique<engine::webkit::WebKitView>();
    }
    auto* result = tab.get();
    m_tabs.push_back(std::move(tab));
    return result;
}

void Browser::closeTab(engine::WebView* tab)
{
    if (!tab)
        return;
    const auto found = std::find_if(m_tabs.begin(), m_tabs.end(),
        [tab](const auto& candidate) { return candidate.get() == tab; });
    if (found == m_tabs.end())
        return;

    // Hand the document back to the web process so the closed page's memory is
    // released; the view object itself survives for reuse.
    tab->load("about:blank");

    std::unique_ptr<engine::WebView> owned = std::move(*found);
    m_tabs.erase(found);

    if (m_pool.size() < kMaxPooledViews) {
        // Detach from the window that displayed it. removeWidget() only takes it
        // out of the stack, so the widget would stay a child and Qt would delete
        // it along with that window, leaving the pool holding a dead view.
        if (auto* page = owned->widget())
            page->setParent(nullptr);
        m_pool.push_back(std::move(owned));
        return;
    }

    // The pool is full. WebKit/FDO can still have frame callbacks queued, so
    // hand widget destruction back to Qt's event loop instead of tearing down
    // the engine from inside QTabBar's close signal.
    QWidget* page = owned->widget();
    owned.release();
    page->deleteLater();
}

void Browser::moveTab(int from, int to)
{
    if (from < 0 || to < 0 || from >= tabCount() || to >= tabCount() || from == to)
        return;
    auto tab = std::move(m_tabs[static_cast<std::size_t>(from)]);
    m_tabs.erase(m_tabs.begin() + from);
    m_tabs.insert(m_tabs.begin() + to, std::move(tab));
}

void Browser::navigate(engine::WebView* tab, const std::string& uri)
{
    if (tab)
        tab->load(uri);
}

void Browser::goBack(engine::WebView* tab)
{
    if (tab && tab->canGoBack())
        tab->goBack();
}

void Browser::goForward(engine::WebView* tab)
{
    if (tab && tab->canGoForward())
        tab->goForward();
}

void Browser::reload(engine::WebView* tab)
{
    if (tab)
        tab->reload();
}

// The extensions API reads and mutates tab state, which lives in MainWindow.
// Browser forwards rather than duplicating the tab list, so there is one owner.

std::vector<extensions::TabSnapshot> Browser::tabs() const
{
    return m_window.tabSnapshots();
}

std::optional<extensions::TabSnapshot> Browser::activeTab() const
{
    const int row = m_window.currentTabRow();
    return row >= 0 ? m_window.tabSnapshot(m_window.tabIdForRow(row)) : std::nullopt;
}

std::optional<extensions::TabSnapshot> Browser::tab(int id) const
{
    return m_window.tabSnapshot(id);
}

int Browser::createTab(const QString& url, bool active)
{
    // Opening through MainWindow keeps one path for tab creation, so extensions
    // and the UI cannot drift apart.
    m_window.openTab(url, active);
    return m_window.tabIdForRow(m_window.tabCount() - 1);
}

void Browser::removeTab(int id)
{
    const int row = m_window.rowForTabId(id);
    if (row >= 0)
        m_window.closeTabByIndex(row);
}

void Browser::updateTab(int id, const QJsonObject& properties)
{
    const int row = m_window.rowForTabId(id);
    if (row < 0)
        return;
    if (properties.contains(QStringLiteral("url"))) {
        m_window.navigateRow(row,
            properties.value(QStringLiteral("url")).toString());
    }
    if (properties.contains(QStringLiteral("active"))
        && properties.value(QStringLiteral("active")).toBool()) {
        m_window.selectTab(row);
    }
}

void Browser::activateTab(int id)
{
    const int row = m_window.rowForTabId(id);
    if (row >= 0)
        m_window.selectTab(row);
}

std::vector<int> Browser::windowIds() const
{
    // Iridium currently has a single window.
    return { 1 };
}

int Browser::currentWindow() const
{
    return 1;
}

} // namespace iridium
