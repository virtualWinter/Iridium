#include "ui/settings/SettingsStore.hpp"

#include <QDir>
#include <QRegularExpression>
#include <QUrl>

namespace iridium {

namespace {
constexpr char kColorSchemeKey[] = "appearance/colorScheme";
constexpr char kConfirmCloseKey[] = "tabs/confirmBeforeClosing";
constexpr char kHomePageKey[] = "general/homePage";
constexpr char kSearchKey[] = "general/searchTemplate";
constexpr char kDownloadDirKey[] = "downloads/directory";

// The start page used when none is configured, and the search used when no
// template is. Both are named in one place because the settings UI offers them
// as "reset to default" and a reset that disagrees with the built-in behaviour
// would be worse than no reset at all.
constexpr char kDefaultHomePage[] = "https://example.com";
constexpr char kDefaultSearchTemplate[] = "https://duckduckgo.com/?q=%1";

// A scheme, with or without the "//" separator. Matched rather than tested with
// QUrl::scheme() because QUrl accepts almost anything as a scheme, including
// things that are really a search like "javascript:1" or a bare word with a
// colon in it.
const QRegularExpression& schemePattern()
{
    static const QRegularExpression pattern(
        QStringLiteral("^([A-Za-z][A-Za-z0-9+.\\-]*)://"));
    return pattern;
}

// A dotted host, a bare IPv4 address, or "localhost", each with an optional
// port and an optional path, query or fragment.
//
// A single label without a dot is deliberately not a host: "intranet/page" and
// "monad/lambda" are indistinguishable, and a phrase is far more likely to be
// what was typed. "localhost" is the one single label worth recognising, because
// nobody searches for it.
const QRegularExpression& hostPattern()
{
    static const QRegularExpression pattern(QStringLiteral(
        "^(?:(?<ipv4>\\d{1,3}(?:\\.\\d{1,3}){3})"
        "|(?<host>localhost"
        "|(?:[A-Za-z0-9](?:[A-Za-z0-9\\-]*[A-Za-z0-9])?\\.)+[A-Za-z][A-Za-z0-9\\-]*))"
        "(?::(?<port>\\d+))?"
        "(?<rest>[/?#].*)?$"));
    return pattern;
}

} // namespace

QString SettingsStore::defaultHomePage()
{
    return QString::fromLatin1(kDefaultHomePage);
}

QString SettingsStore::defaultSearchTemplate()
{
    return QString::fromLatin1(kDefaultSearchTemplate);
}

SettingsStore::SettingsStore()
    : QObject(nullptr)
{
    // Nothing is read here: there is no profile directory until
    // pointAtProfile() is called.
}

void SettingsStore::pointAtProfile(const QString& profileDirectory)
{
    QDir().mkpath(profileDirectory);
    m_settings = std::make_unique<QSettings>(
        profileDirectory + QStringLiteral("/settings.ini"), QSettings::IniFormat);

    // Re-seed the derived override from whatever this profile stored.
    m_forcedColorScheme.reset();
    if (colorScheme() == QLatin1String("dark"))
        m_forcedColorScheme = true;
    else if (colorScheme() == QLatin1String("light"))
        m_forcedColorScheme = false;

    // Announce the new values: open views and the settings panes were built
    // against the previous profile.
    emit forcedColorSchemeChanged(m_forcedColorScheme);
    emit colorSchemeChanged(colorScheme());
    emit confirmBeforeClosingTabsChanged(confirmBeforeClosingTabs());
    emit homePageChanged(homePage());
    emit searchTemplateChanged(searchTemplate());
    emit downloadDirectoryChanged(downloadDirectory());
}

SettingsStore& SettingsStore::instance()
{
    static SettingsStore store;
    return store;
}

QVariant SettingsStore::value(const QString& key, const QVariant& fallback) const
{
    // No profile chosen yet: behave as if everything were unset rather than
    // reading through a null store.
    return m_settings ? m_settings->value(key, fallback) : fallback;
}

void SettingsStore::setValue(const QString& key, const QVariant& newValue)
{
    // Dropped rather than queued: a preference set before a profile exists has
    // nowhere meaningful to go, and writing it to the next profile would be
    // surprising.
    if (!m_settings)
        return;
    m_settings->setValue(key, newValue);
    // QSettings buffers and only writes on sync or destruction. The store is a
    // process-wide singleton, so "on destruction" is whenever the process exits
    // -- which a crash or a kill skips. Flushing here means a preference the user
    // saw accepted is on disk before they can do anything else to lose it.
    m_settings->sync();
}

QString SettingsStore::colorScheme() const
{
    const QString scheme = value(QLatin1String(kColorSchemeKey),
        QStringLiteral("system")).toString();
    // Guard against a hand-edited or corrupted config: an unknown value would
    // leave the colour scheme unset.
    if (scheme == QLatin1String("system") || scheme == QLatin1String("light")
        || scheme == QLatin1String("dark")) {
        return scheme;
    }
    return QStringLiteral("system");
}

void SettingsStore::setColorScheme(const QString& scheme)
{
    const QString normalized = scheme == QLatin1String("light") ? QStringLiteral("light")
        : scheme == QLatin1String("dark") ? QStringLiteral("dark")
        : QStringLiteral("system");

    // The persisted value is the single source of truth for this preference, so
    // it also drives the in-memory override. Deriving the override from the
    // stored scheme keeps the two from drifting apart, which they did when each
    // setter updated only its own copy.
    const std::optional<bool> derived = normalized == QLatin1String("dark")
        ? std::optional<bool>(true)
        : normalized == QLatin1String("light") ? std::optional<bool>(false)
        : std::nullopt;

    const bool changed = normalized != colorScheme();
    if (changed)
        setValue(QLatin1String(kColorSchemeKey), normalized);

    if (m_forcedColorScheme != derived) {
        m_forcedColorScheme = derived;
        emit forcedColorSchemeChanged(derived);
    }
    if (changed)
        emit colorSchemeChanged(normalized);
}

void SettingsStore::setForcedColorScheme(std::optional<bool> dark)
{
    // Routed through setColorScheme, which owns the persisted value and the
    // in-memory override together.
    setColorScheme(dark.has_value()
        ? (*dark ? QStringLiteral("dark") : QStringLiteral("light"))
        : QStringLiteral("system"));
}

bool SettingsStore::confirmBeforeClosingTabs() const
{
    return value(QLatin1String(kConfirmCloseKey), false).toBool();
}

void SettingsStore::setConfirmBeforeClosingTabs(bool enabled)
{
    if (enabled == confirmBeforeClosingTabs())
        return;
    setValue(QLatin1String(kConfirmCloseKey), enabled);
    emit confirmBeforeClosingTabsChanged(enabled);
}

QString SettingsStore::homePage() const
{
    const QString stored = value(QLatin1String(kHomePageKey)).toString().trimmed();
    return stored.isEmpty() ? defaultHomePage() : stored;
}

void SettingsStore::setHomePage(const QString& url)
{
    // Empty is stored as empty rather than as the default written out, so
    // clearing the field really does return to following the built-in start
    // page and does not pin a value that later changes with the default.
    const QString trimmed = url.trimmed();
    if (trimmed == value(QLatin1String(kHomePageKey)).toString().trimmed())
        return;
    setValue(QLatin1String(kHomePageKey), trimmed);
    emit homePageChanged(homePage());
}

bool SettingsStore::hasCustomHomePage() const
{
    return !value(QLatin1String(kHomePageKey)).toString().trimmed().isEmpty();
}

QString SettingsStore::storedHomePage() const
{
    return value(QLatin1String(kHomePageKey)).toString().trimmed();
}

QString SettingsStore::searchTemplate() const
{
    const QString stored = value(QLatin1String(kSearchKey)).toString().trimmed();
    // A template without the placeholder would discard the user's query, which
    // is worse than falling back to the default.
    if (stored.isEmpty() || !stored.contains(QLatin1String("%1")))
        return defaultSearchTemplate();
    return stored;
}

void SettingsStore::setSearchTemplate(const QString& templateText)
{
    const QString trimmed = templateText.trimmed();
    if (trimmed == searchTemplate())
        return;
    setValue(QLatin1String(kSearchKey), trimmed);
    emit searchTemplateChanged(searchTemplate());
}

bool SettingsStore::hasCustomSearchTemplate() const
{
    // A stored template the reader would reject is not a custom one either: the
    // effective value is the default, so the UI must show the default.
    return !storedSearchTemplate().isEmpty();
}

QString SettingsStore::storedSearchTemplate() const
{
    const QString stored = value(QLatin1String(kSearchKey)).toString().trimmed();
    // A template without the placeholder is stored but never used, so it is not
    // reported as the user's value either: the field keeps showing what they
    // typed so they can fix it, but the Reset button means "discard this".
    return stored.contains(QLatin1String("%1")) ? stored : QString();
}

bool SettingsStore::looksLikeAddress(const QString& input)
{
    const QString trimmed = input.trimmed();
    if (trimmed.isEmpty())
        return false;
    if (schemePattern().match(trimmed).hasMatch())
        return true;
    if (trimmed.startsWith(QLatin1String("about:"), Qt::CaseInsensitive))
        return true;
    // A space cannot appear in a host, so anything with one is a phrase.
    if (trimmed.contains(QLatin1Char(' ')))
        return false;
    return hostPattern().match(trimmed).hasMatch();
}

QString SettingsStore::resolveAddressInput(const QString& input) const
{
    const QString trimmed = input.trimmed();
    if (trimmed.isEmpty())
        return {};

    // An explicit scheme is a URL whatever follows it, including a scheme the
    // engine cannot display: reporting "I cannot load ftp://" is better than
    // silently searching for the text instead. So is "about:page", which has no
    // "//" but is still an address.
    if (schemePattern().match(trimmed).hasMatch()
        || trimmed.startsWith(QLatin1String("about:"), Qt::CaseInsensitive)) {
        return trimmed;
    }

    // A dotted host, an IPv4 address or localhost, with an optional port and
    // path. Anything else is a phrase: "monad" and "3.14" are searches, and
    // "intranet/page" is a phrase too, because a single label with no dot in it
    // is far more likely to be two words.
    if (!trimmed.contains(QLatin1Char(' '))
        && hostPattern().match(trimmed).hasMatch()) {
        return QStringLiteral("https://") + trimmed;
    }

    // A search. The escaping is what makes the result a usable URL: a space
    // would truncate the query at the first one, and a literal "%" would start a
    // percent-escape that the user did not ask for. The order matters: the "%"
    // is escaped first, so the "+" substitutions below cannot introduce one.
    QString escaped = trimmed;
    escaped.replace(QLatin1Char('%'), QLatin1String("%25"));
    escaped.replace(QLatin1Char(' '), QLatin1Char('+'));
    return searchTemplate().arg(escaped);
}

QString SettingsStore::downloadDirectory() const
{
    return value(QLatin1String(kDownloadDirKey)).toString().trimmed();
}

void SettingsStore::setDownloadDirectory(const QString& path)
{
    const QString trimmed = path.trimmed();
    if (trimmed == downloadDirectory())
        return;
    setValue(QLatin1String(kDownloadDirKey), trimmed);
    emit downloadDirectoryChanged(trimmed);
}

} // namespace iridium
