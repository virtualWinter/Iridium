#define QT_NO_KEYWORDS
#include "engine/webkit/PreferredColorScheme.hpp"

#include <QGuiApplication>
#include <QPalette>
#include <QStyleHints>

#include <wpe/webkit.h>

#include <memory>

namespace iridium::engine::webkit {

namespace {

bool systemPrefersDark()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    switch (QGuiApplication::styleHints()->colorScheme()) {
    case Qt::ColorScheme::Dark:
        return true;
    case Qt::ColorScheme::Light:
        return false;
    case Qt::ColorScheme::Unknown:
        break;
    }
#endif
    // Platforms that do not report a color scheme (e.g. the offscreen test
    // platform) fall back to the application palette.
    return QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
}

// Runs at document start in every frame. matchMedia is wrapped so the queries
// that pages use to detect the color scheme answer with the emulated scheme,
// and media queries of accessible stylesheets are rewritten accordingly. The
// current scheme is baked in at injection time; live changes are applied to
// loaded documents with window.__iridiumSetColorScheme() from the UI process.
QByteArray colorSchemeScript(bool dark)
{
    static const char script[] = R"JS((function() {
    if (window.__iridiumColorScheme)
        return;

    var nativeMatchMedia = window.matchMedia ? window.matchMedia.bind(window) : null;
    var scheme = '@SCHEME@';
    var wrappers = [];
    var originalMediaTexts = new WeakMap();

    window.__iridiumColorScheme = scheme;

    function isDark() {
        return scheme === 'dark';
    }

    function rewriteCondition(text) {
        var dark = isDark();
        return String(text)
            .replace(/\(\s*prefers-color-scheme\s*:\s*dark\s*\)/gi, dark ? '(min-width: 0px)' : '(max-width: 0px)')
            .replace(/\(\s*prefers-color-scheme\s*:\s*light\s*\)/gi, dark ? '(max-width: 0px)' : '(min-width: 0px)');
    }

    class IridiumMediaQueryList extends EventTarget {
        constructor(query) {
            super();
            this._query = query;
            this.onchange = null;
            this._native = nativeMatchMedia(rewriteCondition(query));
            this._matches = this._native.matches;
            this._sync = this._sync.bind(this);
            this._native.addEventListener('change', this._sync);
            wrappers.push(this);
        }
        get matches() { return this._native.matches; }
        get media() { return this._query; }
        addListener(callback) { this.addEventListener('change', callback); }
        removeListener(callback) { this.removeEventListener('change', callback); }
        _sync() {
            var matches = this._native.matches;
            if (matches === this._matches)
                return;
            this._matches = matches;
            var event;
            try {
                event = new MediaQueryListEvent('change', { media: this._query, matches: matches });
            } catch (e) {
                event = new Event('change');
                event.media = this._query;
                event.matches = matches;
            }
            this.dispatchEvent(event);
            if (typeof this.onchange === 'function')
                this.onchange(event);
        }
        _rescheme() {
            this._native.removeEventListener('change', this._sync);
            this._native = nativeMatchMedia(rewriteCondition(this._query));
            this._native.addEventListener('change', this._sync);
            this._sync();
        }
    }

    if (nativeMatchMedia) {
        window.matchMedia = function(query) {
            query = String(query);
            if (/prefers-color-scheme/i.test(query))
                return new IridiumMediaQueryList(query);
            return nativeMatchMedia(query);
        };
    }

    function rewriteRules(rules) {
        if (!rules)
            return;
        for (var i = 0; i < rules.length; ++i) {
            var rule = rules[i];
            try {
                if (rule.media && rule.media.mediaText) {
                    var original = originalMediaTexts.has(rule) ? originalMediaTexts.get(rule) : rule.media.mediaText;
                    if (/prefers-color-scheme/i.test(original)) {
                        originalMediaTexts.set(rule, original);
                        var rewritten = rewriteCondition(original);
                        if (rule.media.mediaText !== rewritten)
                            rule.media.mediaText = rewritten;
                    }
                }
                if (rule.cssRules)
                    rewriteRules(rule.cssRules);
            } catch (e) {
            }
        }
    }

    function rewriteStyleSheets() {
        var sheets = document.styleSheets;
        if (!sheets)
            return;
        for (var i = 0; i < sheets.length; ++i) {
            var rules;
            try {
                rules = sheets[i].cssRules;
            } catch (e) {
                continue; // Cross-origin stylesheet, not readable.
            }
            rewriteRules(rules);
        }
    }

    function applyRootScheme() {
        if (document.documentElement)
            document.documentElement.style.colorScheme = scheme;
        else
            document.addEventListener('DOMContentLoaded', applyRootScheme, { once: true });
    }

    window.__iridiumSetColorScheme = function(next) {
        if (next !== 'dark' && next !== 'light')
            return;
        scheme = next;
        window.__iridiumColorScheme = scheme;
        applyRootScheme();
        rewriteStyleSheets();
        for (var i = 0; i < wrappers.length; ++i)
            wrappers[i]._rescheme();
    };

    applyRootScheme();
    document.addEventListener('DOMContentLoaded', rewriteStyleSheets);
    window.addEventListener('load', rewriteStyleSheets);
    if (typeof MutationObserver === 'function') {
        var scheduled = false;
        var observer = new MutationObserver(function() {
            if (scheduled)
                return;
            scheduled = true;
            setTimeout(function() {
                scheduled = false;
                rewriteStyleSheets();
            }, 50);
        });
        observer.observe(document, { childList: true, subtree: true });
    }
})();
)JS";
    QByteArray source(script);
    source.replace("@SCHEME@", dark ? "dark" : "light");
    return source;
}

constexpr char darkRootStyle[] = ":root { color-scheme: dark; }";

} // namespace

PreferredColorScheme::PreferredColorScheme(WebKitWebView* webView)
    : m_webView(webView)
    , m_manager(webView ? webkit_web_view_get_user_content_manager(webView) : nullptr)
    , m_dark(systemPrefersDark())
{
    install(m_dark);

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    m_schemeChanged = QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
        [this](Qt::ColorScheme scheme) {
            if (scheme == Qt::ColorScheme::Unknown)
                return;
            setDark(scheme == Qt::ColorScheme::Dark);
        });
#endif
}

PreferredColorScheme::~PreferredColorScheme()
{
    QObject::disconnect(m_schemeChanged);
    if (!m_manager)
        return;
    if (m_script) {
        webkit_user_content_manager_remove_script(m_manager, m_script);
        webkit_user_script_unref(m_script);
    }
    if (m_styleSheet) {
        webkit_user_content_manager_remove_style_sheet(m_manager, m_styleSheet);
        webkit_user_style_sheet_unref(m_styleSheet);
    }
}

void PreferredColorScheme::setDark(bool dark)
{
    if (dark == m_dark)
        return;
    m_dark = dark;
    install(dark);
    applyToCurrentDocument();
}

void PreferredColorScheme::install(bool dark)
{
    if (!m_manager)
        return;

    if (m_script) {
        webkit_user_content_manager_remove_script(m_manager, m_script);
        webkit_user_script_unref(m_script);
        m_script = nullptr;
    }
    if (m_styleSheet) {
        webkit_user_content_manager_remove_style_sheet(m_manager, m_styleSheet);
        webkit_user_style_sheet_unref(m_styleSheet);
        m_styleSheet = nullptr;
    }

    const QByteArray script = colorSchemeScript(dark);
    m_script = webkit_user_script_new(script.constData(),
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
        nullptr, nullptr);
    webkit_user_content_manager_add_script(m_manager, m_script);

    if (dark) {
        m_styleSheet = webkit_user_style_sheet_new(darkRootStyle,
            WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_STYLE_LEVEL_USER,
            nullptr, nullptr);
        webkit_user_content_manager_add_style_sheet(m_manager, m_styleSheet);
    }
}

void PreferredColorScheme::applyToCurrentDocument()
{
    if (!m_webView)
        return;
    const QByteArray script = "window.__iridiumSetColorScheme && window.__iridiumSetColorScheme('"
        + QByteArray(m_dark ? "dark" : "light") + "');";
    webkit_web_view_evaluate_javascript(m_webView, script.constData(), -1,
        nullptr, nullptr, nullptr, nullptr, nullptr);
}

} // namespace iridium::engine::webkit
