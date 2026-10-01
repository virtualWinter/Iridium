// Covers XDG path resolution and install/uninstall. XDG_DATA_HOME and
// XDG_DATA_DIRS are redirected into temp dirs so the test never touches the
// developer's real data home.

#include "extensions/ExtensionInstaller.hpp"
#include "extensions/ExtensionPaths.hpp"
#include "extensions/ExtensionRegistry.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include <cstdio>
#include <string>

using iridium::extensions::ExtensionInstaller;
using iridium::extensions::ExtensionPaths;
using iridium::extensions::ExtensionRegistry;

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

bool writeFile(const QString& path, const QByteArray& contents)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    return file.write(contents) == contents.size();
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir xdg;
    QTemporaryDir systemShare;
    if (!xdg.isValid() || !systemShare.isValid()) {
        std::printf("FAIL: could not create temp dirs\n");
        return 1;
    }

    qputenv("XDG_DATA_HOME", xdg.path().toUtf8());
    qputenv("XDG_DATA_DIRS", systemShare.path().toUtf8());

    // userDirectory is XDG_DATA_HOME/iridium/extensions.
    check(ExtensionPaths::userDirectory()
        == xdg.path() + QStringLiteral("/iridium/extensions"),
        "user directory follows XDG_DATA_HOME");

    // A directory that does not exist is not offered for scanning.
    check(!ExtensionPaths::searchPaths().contains(ExtensionPaths::userDirectory()),
        "absent user directory is not scanned");

    QDir().mkpath(ExtensionPaths::userDirectory());
    check(ExtensionPaths::searchPaths().contains(ExtensionPaths::userDirectory()),
        "user directory appears once created");

    // System directory only counts when the app folder exists under it.
    check(!ExtensionPaths::systemDirectories().contains(
        systemShare.path() + QStringLiteral("/iridium/extensions")),
        "absent system directory is skipped");
    QDir().mkpath(systemShare.path() + QStringLiteral("/iridium/extensions"));
    check(ExtensionPaths::systemDirectories().contains(
        systemShare.path() + QStringLiteral("/iridium/extensions")),
        "present system directory is found");
    // User entries must precede system ones so they win on duplicate ids.
    check(ExtensionPaths::searchPaths().first()
        == ExtensionPaths::userDirectory(), "user directory searched first");

    // Storage paths are confined to the profile.
    const QString storage = ExtensionPaths::storageDirectory(QStringLiteral("my-ext"));
    check(storage.startsWith(xdg.path()), "storage lives under XDG_DATA_HOME");
    check(storage.contains(QStringLiteral("my-ext")), "storage is per extension");

    // An id with separators must not escape the storage root.
    const QString hostile = ExtensionPaths::storageDirectory(
        QStringLiteral("../../etc/passwd"));
    check(!hostile.contains(QStringLiteral("../..")),
        "hostile extension id is neutralised");
    check(hostile.startsWith(xdg.path()), "hostile id stays under the data home");

    // Install: a valid extension is copied into the user directory.
    QTemporaryDir source;
    writeFile(source.path() + QStringLiteral("/hello/manifest.json"),
        R"({"manifest_version":3,"name":"Hello","version":"1.0",
            "permissions":["storage"]})");
    writeFile(source.path() + QStringLiteral("/hello/content.js"),
        "console.log('hi');");
    writeFile(source.path() + QStringLiteral("/hello/lib/nested.js"), "// nested");

    QString error;
    QString id;
    check(ExtensionInstaller::install(source.path() + QStringLiteral("/hello"), &id, &error),
        "install succeeds: " + error.toStdString());
    check(id == QStringLiteral("hello"), "id is the directory name");
    check(QFile::exists(ExtensionPaths::userDirectory()
        + QStringLiteral("/hello/manifest.json")), "manifest copied");
    check(QFile::exists(ExtensionPaths::userDirectory()
        + QStringLiteral("/hello/content.js")), "script copied");
    check(QFile::exists(ExtensionPaths::userDirectory()
        + QStringLiteral("/hello/lib/nested.js")), "nested file copied");

    // A bad manifest must not create anything in the user directory.
    QTemporaryDir badSource;
    writeFile(badSource.path() + QStringLiteral("/broken/manifest.json"),
        "{ not json");
    check(!ExtensionInstaller::install(badSource.path() + QStringLiteral("/broken"),
        nullptr, &error), "malformed manifest is refused");
    check(!QDir(ExtensionPaths::userDirectory() + QStringLiteral("/broken")).exists(),
        "refused install left nothing behind");

    // The installed copy is discoverable through the registry.
    ExtensionRegistry registry(ExtensionPaths::profileDirectory());
    registry.loadFrom(ExtensionPaths::userDirectory());
    check(registry.extensions().size() == 1, "installed extension is discovered");

    // Storage round-trips through the profile.
    check(registry.loadStorage(QStringLiteral("hello")).isEmpty(), "storage starts empty");
    QJsonObject written;
    written.insert(QStringLiteral("token"), QStringLiteral("abc123"));
    registry.saveStorage(QStringLiteral("hello"), written);
    check(registry.loadStorage(QStringLiteral("hello"))
        .value(QStringLiteral("token")).toString() == QStringLiteral("abc123"),
        "storage value survives");

    // Uninstall removes the files and the stored data.
    check(ExtensionInstaller::uninstall(QStringLiteral("hello"), &error),
        "uninstall succeeds: " + error.toStdString());
    check(!QDir(ExtensionPaths::userDirectory() + QStringLiteral("/hello")).exists(),
        "extension directory removed");
    check(!QDir(ExtensionPaths::storageDirectory(QStringLiteral("hello"))).exists(),
        "stored data removed");

    check(!ExtensionInstaller::uninstall(QStringLiteral("never-installed"), &error),
        "uninstalling an unknown id fails");

    if (g_failures == 0)
        std::printf("PASS: extensions XDG paths and installer\n");
    else
        std::printf("%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}