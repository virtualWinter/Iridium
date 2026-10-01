#pragma once

#include "engine/WebView.hpp"
#include "extensions/ExtensionApi.hpp"
#include "ui/MainWindow.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace iridium {

// Supplies live tab and window state to the extensions API layer. MainWindow
// implements this, since it already owns the tab list.
class Browser final : public extensions::ExtensionApiHost {
    // Browser forwards to MainWindow, which owns the tab list and the widgets.
    friend class MainWindow;
public:
    explicit Browser(std::string initialUrl = "https://example.com");
    ~Browser() override;
    MainWindow& window() { return m_window; }
    engine::WebView* newTab();
    void closeTab(engine::WebView* tab);
    void moveTab(int from, int to);
    void navigate(engine::WebView* tab, const std::string& uri);
    void goBack(engine::WebView* tab);
    void goForward(engine::WebView* tab);
    void reload(engine::WebView* tab);
    int tabCount() const { return static_cast<int>(m_tabs.size()); }

    // ExtensionApiHost: tab and window state as the extensions layer sees it.
    std::vector<extensions::TabSnapshot> tabs() const override;
    std::optional<extensions::TabSnapshot> activeTab() const override;
    std::optional<extensions::TabSnapshot> tab(int id) const override;
    int createTab(const QString& url, bool active) override;
    void removeTab(int id) override;
    void updateTab(int id, const QJsonObject& properties) override;
    void activateTab(int id) override;
    std::vector<int> windowIds() const override;
    int currentWindow() const override;

private:
    // Closed tabs are kept for reuse. Building and tearing down a WebKit view
    // per tab retains several megabytes per page load inside the engine that
    // is never returned, so a small pool reuses one view instead. The pool is
    // deliberately small: it is a reuse cache, not storage.
    static constexpr std::size_t kMaxPooledViews = 4;

    std::vector<std::unique_ptr<engine::WebView>> m_tabs;
    std::vector<std::unique_ptr<engine::WebView>> m_pool;
    MainWindow m_window;
};

} // namespace iridium
