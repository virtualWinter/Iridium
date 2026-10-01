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

    // "system", "light" or "dark".
    QString colorScheme() const;
    void setColorScheme(const QString& scheme);

    // nullopt means follow the system hint. Broadcast to every view so a change
    // applies to tabs already open, not only new ones.
    void setForcedColorScheme(std::optional<bool> dark);
    std::optional<bool> forcedColorScheme() const { return m_forcedColorScheme; }

    bool confirmBeforeClosingTabs() const;
    void setConfirmBeforeClosingTabs(bool enabled);

    // URL opened in a new tab. Empty means the browser default.
    QString homePage() const;
    void setHomePage(const QString& url);

    // Template used when the address bar holds something that is not a URL,
    // e.g. "https://duckduckgo.com/?q=%1". %1 is replaced with the query.
    QString searchTemplate() const;
    void setSearchTemplate(const QString& templateText);

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
    void homePageChanged(const QString& url);
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