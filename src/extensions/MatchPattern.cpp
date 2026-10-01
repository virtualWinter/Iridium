#include "extensions/MatchPattern.hpp"

#include <QUrl>

namespace iridium::extensions {

namespace {

// Glob match where '*' spans any characters. Iterative with backtracking so a
// long path cannot blow the stack.
bool globMatch(const QString& text, const QString& pattern)
{
    if (pattern.isEmpty())
        return text.isEmpty();

    int t = 0;
    int p = 0;
    int star = -1;
    int mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern.at(p) == QLatin1Char('*')
                || pattern.at(p) == text.at(t))) {
            if (pattern.at(p) == QLatin1Char('*')) {
                star = p++;
                mark = t;
            } else {
                ++p;
                ++t;
            }
        } else if (star >= 0) {
            // Backtrack: let the last '*' absorb one more character.
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern.at(p) == QLatin1Char('*'))
        ++p;
    return p == pattern.size();
}

} // namespace

bool MatchPattern::parse(const QString& text, MatchPattern* out, QString* error)
{
    const auto fail = [error](const QString& reason) {
        if (error)
            *error = reason;
        return false;
    };

    if (!out)
        return false;

    QString pattern = text.trimmed();
    if (pattern.isEmpty())
        return fail(QStringLiteral("match pattern is empty"));

    if (pattern == QLatin1String("<all_urls>")) {
        *out = MatchPattern {};
        out->m_text = pattern;
        out->m_allUrls = true;
        return true;
    }

    const int schemeEnd = pattern.indexOf(QLatin1String("://"));
    if (schemeEnd <= 0)
        return fail(QStringLiteral("match pattern '%1' has no scheme").arg(pattern));

    MatchPattern result;
    result.m_text = pattern;
    result.m_scheme = pattern.left(schemeEnd);
    if (result.m_scheme.contains(QLatin1Char('*')))
        result.m_anyScheme = true;

    // The spec admits a fixed set of schemes; anything else is a typo or an
    // attempt to claim a scheme the browser does not expose.
    static const QStringList kSchemes = { QStringLiteral("http"), QStringLiteral("https"),
        QStringLiteral("ws"), QStringLiteral("wss"), QStringLiteral("ftp"),
        QStringLiteral("file") };
    if (!result.m_anyScheme && !kSchemes.contains(result.m_scheme, Qt::CaseInsensitive)) {
        return fail(QStringLiteral("match pattern '%1' uses unsupported scheme '%2'")
                        .arg(pattern, result.m_scheme));
    }

    QString rest = pattern.mid(schemeEnd + 3);
    if (rest.isEmpty())
        return fail(QStringLiteral("match pattern '%1' has no host").arg(pattern));

    const int pathStart = rest.indexOf(QLatin1Char('/'));
    if (pathStart < 0) {
        // A path is optional; it defaults to "/*".
        result.m_host = rest;
        result.m_path = QStringLiteral("/*");
    } else {
        result.m_host = rest.left(pathStart);
        result.m_path = rest.mid(pathStart);
    }

    // file:// URLs have no authority component, so an empty host is only
    // meaningful for them. For http(s) it is always a mistake.
    const bool hostOptional = result.m_scheme.compare(
        QLatin1String("file"), Qt::CaseInsensitive) == 0;
    if (result.m_host.isEmpty() && !hostOptional)
        return fail(QStringLiteral("match pattern '%1' has no host").arg(pattern));

    if (result.m_host == QLatin1String("*")) {
        result.m_hostIsWildcard = true;
    } else if (result.m_host.startsWith(QLatin1String("*."))) {
        // "*.foo.com" covers foo.com itself as well as its subdomains.
        result.m_subdomains = true;
        result.m_host = result.m_host.mid(2);
        if (result.m_host.isEmpty())
            return fail(QStringLiteral("match pattern '%1' has an empty host").arg(pattern));
    } else if (result.m_host.contains(QLatin1Char('*'))) {
        return fail(QStringLiteral("match pattern '%1' may only use '*' as a whole host "
                                   "or a leading '*.' prefix").arg(pattern));
    }

    *out = result;
    return true;
}

bool MatchPattern::matchesHost(const QString& host) const
{
    if (m_hostIsWildcard)
        return true;
    if (m_subdomains) {
        if (host.compare(m_host, Qt::CaseInsensitive) == 0)
            return true;
        return host.endsWith(QLatin1Char('.') + m_host, Qt::CaseInsensitive);
    }
    return host.compare(m_host, Qt::CaseInsensitive) == 0;
}

bool MatchPattern::matchPath(const QString& path) const
{
    return globMatch(path, m_path);
}

bool MatchPattern::matches(const QString& url) const
{
    if (m_allUrls) {
        // <all_urls> covers the web schemes, not file: or data:.
        const QUrl parsed(url);
        const QString scheme = parsed.scheme();
        return scheme == QLatin1String("http") || scheme == QLatin1String("https")
            || scheme == QLatin1String("ws") || scheme == QLatin1String("wss")
            || scheme == QLatin1String("ftp") || scheme == QLatin1String("file");
    }

    const QUrl parsed(url);
    if (!parsed.isValid())
        return false;

    if (m_anyScheme) {
        // "*" means the web schemes specifically.
        const QString scheme = parsed.scheme();
        if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
            return false;
    } else if (parsed.scheme().compare(m_scheme, Qt::CaseInsensitive) != 0) {
        return false;
    }

    if (!matchesHost(parsed.host()))
        return false;

    QString path = parsed.path();
    if (path.isEmpty())
        path = QStringLiteral("/");
    return matchPath(path);
}

} // namespace iridium::extensions
