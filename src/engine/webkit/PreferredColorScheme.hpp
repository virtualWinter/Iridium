#pragma once

#include <QMetaObject>

struct _WebKitUserContentManager;
struct _WebKitUserScript;
struct _WebKitUserStyleSheet;
struct _WebKitWebView;
using WebKitWebView = _WebKitWebView;

namespace iridium::engine::webkit {

// WebKit receives the system color scheme from the WPE Platform settings,
// which the legacy libwpe embedding path does not use (and touching the WPE
// Platform API would disable that backend entirely). This class emulates
// prefers-color-scheme for pages instead: it patches matchMedia, rewrites the
// media queries of accessible stylesheets and applies color-scheme to the root
// element, following the system color scheme reported by Qt.
class PreferredColorScheme final {
public:
    explicit PreferredColorScheme(WebKitWebView* webView);
    ~PreferredColorScheme();

    void setDark(bool dark);
    bool isDark() const { return m_dark; }

private:
    void install(bool dark);
    void applyToCurrentDocument();

    WebKitWebView* m_webView { nullptr };
    _WebKitUserContentManager* m_manager { nullptr };
    _WebKitUserScript* m_script { nullptr };
    _WebKitUserStyleSheet* m_styleSheet { nullptr };
    QMetaObject::Connection m_schemeChanged;
    bool m_dark { false };
};

} // namespace iridium::engine::webkit
