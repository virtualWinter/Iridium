#include "extensions/ExtensionPaths.hpp"

#include <QDir>
#include <QProcessEnvironment>
#include <QStandardPaths>

namespace iridium::extensions {

namespace {

constexpr char kAppFolder[] = "iridium";
constexpr char kExtensionsFolder[] = "extensions";

QString envPath(const char* variable)
{
    const QString value = QProcessEnvironment::systemEnvironment()
        .value(QString::fromLatin1(variable));
    return value.isEmpty() ? QString() : QDir::cleanPath(value);
}

} // namespace

QString ExtensionPaths::userDirectory()
{
    const QString dataHome = envPath("XDG_DATA_HOME");
    const QString base = !dataHome.isEmpty()
        ? dataHome
        : QDir::homePath() + QStringLiteral("/.local/share");
    return base + QLatin1Char('/') + QLatin1String(kAppFolder)
        + QLatin1Char('/') + QLatin1String(kExtensionsFolder);
}

QStringList ExtensionPaths::systemDirectories()
{
    QStringList result;

    // XDG_DATA_DIRS is colon-separated and defaults to /usr/local/share:/usr/share.
    const QString dataDirs = envPath("XDG_DATA_DIRS");
    const QStringList bases = dataDirs.isEmpty()
        ? QStringList { QStringLiteral("/usr/local/share"), QStringLiteral("/usr/share") }
        : dataDirs.split(QLatin1Char(':'), Qt::SkipEmptyParts);

    for (const QString& base : bases) {
        const QString path = QDir(base).filePath(
            QStringLiteral("%1/%2").arg(QLatin1String(kAppFolder), QLatin1String(kExtensionsFolder)));
        if (QDir(path).exists())
            result.append(path);
    }
    return result;
}

QStringList ExtensionPaths::searchPaths()
{
    QStringList paths;
    const QString user = userDirectory();
    if (QDir(user).exists())
        paths.append(user);
    // User extensions win over system ones with the same id.
    for (const QString& system : systemDirectories())
        paths.append(system);
    return paths;
}

QString ExtensionPaths::profileDirectory(const QString& profile)
{
    const QString dataHome = envPath("XDG_DATA_HOME");
    const QString base = !dataHome.isEmpty()
        ? dataHome
        : QDir::homePath() + QStringLiteral("/.local/share");

    const QString name = profile.isEmpty() ? QStringLiteral("default") : profile;
    return QStringLiteral("%1/%2/profiles/%3").arg(base, QLatin1String(kAppFolder), name);
}

QString ExtensionPaths::storageDirectory(const QString& extensionId, const QString& profile)
{
    // Extension ids come from manifests and may contain separators, so keep
    // them inside the storage root rather than letting them escape it.
    QString safe = extensionId;
    safe.replace(QLatin1Char('/'), QLatin1Char('_'));
    safe.replace(QLatin1Char('\\'), QLatin1Char('_'));
    // Then collapse any remaining ".." so the id cannot climb out of the root.
    while (safe.contains(QStringLiteral("..")))
        safe.replace(QStringLiteral(".."), QStringLiteral("_"));
    if (safe.isEmpty())
        safe = QStringLiteral("unknown");

    return profileDirectory(profile) + QStringLiteral("/storage/") + safe;
}

} // namespace iridium::extensions