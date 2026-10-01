#include "browser/ProfileManager.hpp"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSettings>

#include <algorithm>

namespace iridium {

namespace {

constexpr char kDefaultProfile[] = "default";
constexpr char kManifestFile[] = "profile.json";
constexpr char kCurrentProfileKey[] = "profiles/current";

QString appDataDirectory()
{
    const QString dataHome = QProcessEnvironment::systemEnvironment()
        .value(QStringLiteral("XDG_DATA_HOME"));
    const QString base = !dataHome.isEmpty()
        ? dataHome : QDir::homePath() + QStringLiteral("/.local/share");
    return base + QStringLiteral("/iridium");
}

} // namespace

ProfileManager::ProfileManager()
{
    // Not in the member initialiser: loadCurrent() is const and reads no
    // members, but calling it before m_current exists reads as a use-before-init
    // to the compiler, and a profile is the one thing the rest of startup needs.
    m_current = loadCurrent();

    // The default profile always exists, so a first run has somewhere to write.
    if (!m_current.isValid()) {
        Profile fallback;
        fallback.name = QLatin1String(kDefaultProfile);
        fallback.path = profilesRoot() + QLatin1Char('/')
            + QLatin1String(kDefaultProfile);
        fallback.builtin = true;
        QDir().mkpath(fallback.path);
        writeProfile(fallback);
        saveCurrent(fallback.name);
        m_current = fallback;
    }
}

ProfileManager& ProfileManager::instance()
{
    static ProfileManager manager;
    return manager;
}

QString ProfileManager::profilesRoot() const
{
    return appDataDirectory() + QStringLiteral("/profiles");
}

QString ProfileManager::globalSettingsPath() const
{
    return appDataDirectory() + QStringLiteral("/settings.ini");
}

Profile ProfileManager::readProfile(const QString& directory, const QString& name) const
{
    QFile file(directory + QLatin1Char('/') + QLatin1String(kManifestFile));
    if (!file.open(QIODevice::ReadOnly))
        return {};

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
        return {};

    const QJsonObject root = document.object();
    Profile profile;
    // The directory name is authoritative: it is what every path is built from,
    // so a mismatched field inside the file must not redirect it.
    profile.name = name;
    profile.path = directory;
    profile.builtin = root.value(QStringLiteral("builtin")).toBool()
        || name == QLatin1String(kDefaultProfile);
    return profile;
}

void ProfileManager::writeProfile(const Profile& profile) const
{
    QDir().mkpath(profile.path);
    QJsonObject root;
    root.insert(QStringLiteral("name"), profile.name);
    root.insert(QStringLiteral("builtin"), profile.builtin);
    // Written atomically: a truncated file would make the profile invisible.
    QSaveFile file(profile.path + QLatin1Char('/') + QLatin1String(kManifestFile));
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.commit();
}

Profile ProfileManager::loadCurrent() const
{
    QSettings settings(globalSettingsPath(), QSettings::IniFormat);
    const QString name = settings.value(QLatin1String(kCurrentProfileKey),
        QLatin1String(kDefaultProfile)).toString();

    QString error;
    if (!isValidName(name, &error))
        return {};

    const QString directory = profilesRoot() + QLatin1Char('/') + name;
    const Profile profile = readProfile(directory, name);
    return profile.isValid() ? profile : Profile {};
}

void ProfileManager::saveCurrent(const QString& name) const
{
    QDir().mkpath(appDataDirectory());
    QSettings settings(globalSettingsPath(), QSettings::IniFormat);
    settings.setValue(QLatin1String(kCurrentProfileKey), name);
    settings.sync();
}

QVector<Profile> ProfileManager::profiles() const
{
    QVector<Profile> result;
    QStringList names;

    QDir root(profilesRoot());
    if (root.exists()) {
        names = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    }

    // The current profile is always listed, even if its directory listing is
    // somehow stale.
    if (!names.contains(m_current.name))
        names.prepend(m_current.name);

    for (const QString& name : names) {
        const Profile profile = readProfile(root.filePath(name), name);
        if (profile.isValid())
            result.append(profile);
    }

    // Default first, then alphabetical, so the list order is stable.
    std::sort(result.begin(), result.end(), [](const Profile& left, const Profile& right) {
        if (left.builtin != right.builtin)
            return left.builtin;
        return left.name.compare(right.name, Qt::CaseInsensitive) < 0;
    });
    return result;
}

bool ProfileManager::exists(const QString& name) const
{
    return byName(name).isValid();
}

Profile ProfileManager::byName(const QString& name) const
{
    QString error;
    if (!isValidName(name, &error))
        return {};
    return readProfile(profilesRoot() + QLatin1Char('/') + name, name);
}

bool ProfileManager::isValidName(const QString& name, QString* error)
{
    const auto fail = [error](const QString& reason) {
        if (error)
            *error = reason;
        return false;
    };

    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return fail(QStringLiteral("a profile name cannot be empty"));
    if (trimmed != name)
        return fail(QStringLiteral("a profile name cannot start or end with a space"));
    // The name becomes a directory name, so anything that could escape it or
    // collide with a path is refused outright.
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')))
        return fail(QStringLiteral("a profile name cannot contain a path separator"));
    if (name.contains(QLatin1String("..")))
        return fail(QStringLiteral("a profile name cannot contain '..'"));
    if (name == QLatin1String(".") || name == QLatin1String(".."))
        return fail(QStringLiteral("that is not a usable profile name"));
    static const QString reserved[] = { QStringLiteral(":") };
    for (const QString& character : reserved) {
        if (name.contains(character))
            return fail(QStringLiteral("a profile name cannot contain ':'"));
    }
    if (name.length() > 64)
        return fail(QStringLiteral("a profile name cannot be longer than 64 characters"));
    return true;
}

bool ProfileManager::create(const QString& name, QString* error)
{
    if (!isValidName(name, error))
        return false;
    if (exists(name)) {
        if (error)
            *error = QStringLiteral("a profile called \"%1\" already exists").arg(name);
        return false;
    }

    Profile profile;
    profile.name = name;
    profile.path = profilesRoot() + QLatin1Char('/') + name;
    profile.builtin = false;
    if (!QDir().mkpath(profile.path)) {
        if (error)
            *error = QStringLiteral("could not create %1").arg(profile.path);
        return false;
    }
    writeProfile(profile);

    // Read it back: a profile that cannot be listed is not usable.
    if (!readProfile(profile.path, name).isValid()) {
        QDir(profile.path).removeRecursively();
        if (error)
            *error = QStringLiteral("the new profile could not be written");
        return false;
    }
    return true;
}

bool ProfileManager::remove(const QString& name, QString* error)
{
    const Profile profile = byName(name);
    if (!profile.isValid()) {
        if (error)
            *error = QStringLiteral("there is no profile called \"%1\"").arg(name);
        return false;
    }
    if (profile.builtin) {
        if (error)
            *error = QStringLiteral("the default profile cannot be removed");
        return false;
    }
    if (!QDir(profile.path).removeRecursively()) {
        if (error)
            *error = QStringLiteral("could not remove %1").arg(profile.path);
        return false;
    }
    // Never leave the current profile pointing at a directory that is gone.
    if (m_current.name == name)
        switchTo(QLatin1String(kDefaultProfile), error);
    return true;
}

bool ProfileManager::switchTo(const QString& name, QString* error)
{
    const Profile profile = byName(name);
    if (!profile.isValid()) {
        if (error)
            *error = QStringLiteral("there is no profile called \"%1\"").arg(name);
        return false;
    }
    m_current = profile;
    saveCurrent(name);
    return true;
}

} // namespace iridium