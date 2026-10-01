#include "ui/settings/SettingsStore.hpp"

#include <QDir>
#include <QUrl>

namespace iridium {

namespace {
constexpr char kColorSchemeKey[] = "appearance/colorScheme";
constexpr char kConfirmCloseKey[] = "tabs/confirmBeforeClosing";
constexpr char kHomePageKey[] = "general/homePage";
constexpr char kSearchKey[] = "general/searchTemplate";
constexpr char kDownloadDirKey[] = "downloads/directory";
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
    emit homePageChanged(homePage());
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
    setValue(QLatin1String(kConfirmCloseKey), enabled);
}

QString SettingsStore::homePage() const
{
    return value(QLatin1String(kHomePageKey)).toString().trimmed();
}

void SettingsStore::setHomePage(const QString& url)
{
    const QString trimmed = url.trimmed();
    if (trimmed == homePage())
        return;
    setValue(QLatin1String(kHomePageKey), trimmed);
    emit homePageChanged(trimmed);
}

QString SettingsStore::searchTemplate() const
{
    const QString stored = value(QLatin1String(kSearchKey),
        QStringLiteral("https://duckduckgo.com/?q=%1")).toString();
    // A template without the placeholder would discard the user's query, which
    // is worse than falling back to the default.
    return stored.contains(QLatin1String("%1"))
        ? stored : QStringLiteral("https://duckduckgo.com/?q=%1");
}

void SettingsStore::setSearchTemplate(const QString& templateText)
{
    setValue(QLatin1String(kSearchKey), templateText.trimmed());
}

QString SettingsStore::resolveAddressInput(const QString& input) const
{
    const QString trimmed = input.trimmed();
    if (trimmed.isEmpty())
        return {};

    // Explicit schemes pass through untouched, including internal ones the user may
    // type to reach a page the browser can display.
    if (trimmed.startsWith(QLatin1String("http://"))
        || trimmed.startsWith(QLatin1String("https://"))
        || trimmed.startsWith(QLatin1String("file://"))
        || trimmed.startsWith(QLatin1String("about:"))) {
        return trimmed;
    }

    // A bare host like "example.com/page" is a URL; anything with spaces is a
    // search. That distinction is what a user expects from an address bar.
    if (!trimmed.contains(QLatin1Char(' '))
        && QUrl::fromUserInput(trimmed).host().contains(QLatin1Char('.'))) {
        return QStringLiteral("https://") + trimmed;
    }

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