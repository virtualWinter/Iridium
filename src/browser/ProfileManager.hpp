#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace iridium {

// A named profile: a separate set of extensions, extension storage, settings,
// history and bookmarks.
//
// Profiles are directories under XDG_DATA_HOME/iridium/profiles/<name>. The
// current profile is remembered in the global settings file, which lives above
// the profiles so it is readable before one is chosen.
struct Profile {
    QString name;
    QString path;
    // True only for "default", which is never deleted.
    bool builtin { false };

    bool isValid() const { return !name.isEmpty(); }
};

// Enumerates and manages profiles.
class ProfileManager final {
public:
    static ProfileManager& instance();

    // The profile in use, creating the default if none exists yet.
    const Profile& current() const { return m_current; }

    // All profiles, default first, then alphabetical. A directory without a
    // profile.json is skipped rather than offered, so a stray folder does not
    // become a profile.
    QVector<Profile> profiles() const;

    bool exists(const QString& name) const;
    Profile byName(const QString& name) const;

    // Creates an empty profile. Fails on an invalid or already-taken name.
    bool create(const QString& name, QString* error);
    // Deletes a profile and everything in it. Refuses the default profile.
    bool remove(const QString& name, QString* error);
    // Switches the current profile. Callers must rebuild per-profile state.
    bool switchTo(const QString& name, QString* error);

    // True when `name` is safe to use as a directory name: no separators, no
    // traversal, no reserved name.
    static bool isValidName(const QString& name, QString* error);

private:
    ProfileManager();

    // Reads the remembered current profile, falling back to default.
    Profile loadCurrent() const;
    void saveCurrent(const QString& name) const;

    // Reads profile.json, or an invalid Profile when absent or malformed.
    Profile readProfile(const QString& directory, const QString& name) const;
    void writeProfile(const Profile& profile) const;
    QString profilesRoot() const;
    // Where the "which profile am I" preference is kept, outside any profile.
    QString globalSettingsPath() const;

    Profile m_current;
};

} // namespace iridium