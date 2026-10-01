#pragma once

#include <QObject>
#include <QSettings>
#include <QString>
#include <QVariant>

#include <memory>
#include <optional>

namespace iridium {

// Persisted browser settings, scoped to the current profile.
//
// One QSettings-backed place for preferences that must survive a restart. Kept
// separate from the registry's own extension state so a corrupt settings file
// cannot lose extension enablement, or the reverse.
//
// Settings are per profile: each profile has its own file, so switching
// profiles switches preferences along with history and extensions. The file
// path is re-read after a switch, which is why this is not a plain QSettings
// member held for the process lifetime.
class SettingsStore final : public QObject {
    Q_OBJECT
public:
    // Process-wide, created on first use. The settings behind it are not
    // global: see pointAtProfile.
    static SettingsStore& instance();

    // Re-reads settings from `profileDirectory`. Called on startup and whenever
    // the profile changes, so every accessor reflects the new profile.
    void pointAtProfile(const QString& profileDirectory);

    // The built-in defaults, exposed so the settings UI can offer "reset to
    // default" without restating them. A reset that disagreed with the value the
    // store actually falls back to would be worse than no reset.
    static QString defaultHomePage();
    static QString defaultSearchTemplate();

    // "system", "light" or "dark".
    QString colorScheme() const;
    void setColorScheme(const QString& scheme);

    // nullopt means follow the system hint. Broadcast to every view so a change
    // applies to tabs already open, not only new ones.
    void setForcedColorScheme(std::optional<bool> dark);
    std::optional<bool> forcedColorScheme() const { return m_forcedColorScheme; }

    // Asks before closing the window with more than one tab open. Read by
    // MainWindow on close, so the checkbox is not decorative.
    bool confirmBeforeClosingTabs() const;
    void setConfirmBeforeClosingTabs(bool enabled);

    // URL opened in a new tab. Returns defaultHomePage() when unset, so a caller
    // never has to decide what an empty preference means. setHomePage("") clears
    // it and returns to the default rather than storing a pinned copy.
    QString homePage() const;
    void setHomePage(const QString& url);
    // True when a homepage is configured, i.e. homePage() is not the default.
    bool hasCustomHomePage() const;
    // What the user actually typed, which is "" when unset. The settings field
    // shows this rather than the effective value: showing the default there
    // would make an unconfigured field look configured, and Reset would have
    // nothing to reset.
    QString storedHomePage() const;

    // Template used when the address bar holds something that is not a URL,
    // e.g. "https://duckduckgo.com/?q=%1". %1 is replaced with the query. A
    // template without the placeholder falls back to the default, because
    // substituting into one would discard the query.
    QString searchTemplate() const;
    void setSearchTemplate(const QString& templateText);
    bool hasCustomSearchTemplate() const;
    // What the user actually typed, "" when unset or unusable. See
    // storedHomePage for why the field shows this rather than the default.
    QString storedSearchTemplate() const;

    // True when `input` would be navigated to rather than searched for: it has
    // an explicit scheme, or it is a dotted host, an IPv4 address or localhost,
    // optionally with a port and path. Exposed so a settings field can warn
    // about a homepage that would only ever be searched, using exactly the rule
    // the address bar will apply.
    static bool looksLikeAddress(const QString& input);

    // Turns address-bar input into a URL: a real URL is used as-is, anything
    // else goes through searchTemplate(). This is where the setting takes effect.
    QString resolveAddressInput(const QString& input) const;

    // Where downloads are saved. Empty means the XDG downloads directory.
    QString downloadDirectory() const;
    void setDownloadDirectory(const QString& path);

signals:
    void colorSchemeChanged(const QString& scheme);
    void forcedColorSchemeChanged(std::optional<bool> dark);
    // Emitted when a preference the browser acts on changes, so open views can
    // pick it up without polling.
    void confirmBeforeClosingTabsChanged(bool enabled);
    void homePageChanged(const QString& url);
    void searchTemplateChanged(const QString& searchTemplate);
    void downloadDirectoryChanged(const QString& path);

private:
    SettingsStore();

    // Reads and writes go through these so the store is usable before a profile
    // has been chosen: with no profile there is nothing to read or write, and
    // every accessor returns its default rather than dereferencing nothing.
    QVariant value(const QString& key, const QVariant& fallback = {}) const;
    void setValue(const QString& key, const QVariant& value);

    // The persisted colourScheme is the single source of truth; this is derived
    // from it rather than stored separately, so the two cannot disagree.
    std::optional<bool> m_forcedColorScheme;
    // Held by pointer because the file is reopened when the profile changes; a
    // QSettings value could not be repointed at a different file.
    std::unique_ptr<QSettings> m_settings;
};

} // namespace iridium