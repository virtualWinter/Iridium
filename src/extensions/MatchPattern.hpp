#pragma once

#include <QString>

namespace iridium::extensions {

// A WebExtensions match pattern, e.g. "*://*.example.com/*" or "<all_urls>".
//
// WebKit exposes WebKitWebExtensionMatchPattern for this, but in the installed
// WPE 2.52 build its constructors return NULL for every input, including
// new_all_urls(), so matching is implemented here.
//
// Grammar: <scheme>://<host><path>, or the single token "<all_urls>".
//   scheme  "*" (http/https), or a literal scheme
//   host    "*", "*.foo.com" (matches foo.com and subdomains), or a literal
//   path    a "/"-prefixed glob, defaulting to "/*"
// "*" in the path matches any run of characters including "/".
class MatchPattern final {
public:
    // Returns false and leaves `error` set for a malformed pattern.
    static bool parse(const QString& text, MatchPattern* out, QString* error);

    bool matches(const QString& url) const;

    // True when the pattern grants access to every URL ("<all_urls>").
    bool matchesAllUrls() const { return m_allUrls; }
    QString text() const { return m_text; }

private:
    bool matchesHost(const QString& host) const;
    bool matchPath(const QString& path) const;

    QString m_text;
    QString m_scheme;
    QString m_host;
    QString m_path;
    bool m_allUrls { false };
    // Scheme was "*", so only http and https are in scope.
    bool m_anyScheme { false };
    // Host was "*.foo.com"; the bare domain matches too.
    bool m_subdomains { false };
    bool m_hostIsWildcard { false };
};

} // namespace iridium::extensions
