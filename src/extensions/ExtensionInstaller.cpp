#include "extensions/ExtensionInstaller.hpp"

#include "extensions/ExtensionPaths.hpp"
#include "extensions/Manifest.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>
#include <QThread>
#include <QUrl>

#include <utility>

namespace iridium::extensions {

namespace {

bool copyDirectory(const QString& from, const QString& to, QString* error)
{
    QDir source(from);
    if (!source.exists()) {
        *error = QStringLiteral("source directory does not exist");
        return false;
    }

    QDir target(to);
    if (!target.mkpath(QStringLiteral("."))) {
        *error = QStringLiteral("could not create %1").arg(to);
        return false;
    }

    const QStringList files = source.entryList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QString& file : files) {
        if (!QFile::copy(source.filePath(file), target.filePath(file))) {
            *error = QStringLiteral("could not copy %1").arg(file);
            return false;
        }
    }

    const QStringList directories = source.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& directory : directories) {
        if (!copyDirectory(source.filePath(directory), target.filePath(directory), error))
            return false;
    }
    return true;
}

} // namespace

bool ExtensionInstaller::install(const QString& sourceDirectory, QString* idOut,
    QString* error)
{
    // Validate before copying anything: a bad manifest should not create a
    // directory in the user's data home.
    QString parseError;
    if (!Manifest::load(sourceDirectory, &parseError).has_value()) {
        if (error)
            *error = parseError;
        return false;
    }

    // The directory name becomes the id, so two extensions cannot overwrite each
    // other by sharing a name.
    const QString name = QDir(sourceDirectory).dirName();
    if (name.isEmpty() || name == QStringLiteral(".")) {
        if (error)
            *error = QStringLiteral("cannot install from a filesystem root");
        return false;
    }

    const QString destination = ExtensionPaths::userDirectory() + QLatin1Char('/') + name;
    QDir().mkpath(ExtensionPaths::userDirectory());

    // Remove a previous version first so files deleted between versions do not
    // linger and shadow the new manifest.
    QDir existing(destination);
    if (existing.exists() && !existing.removeRecursively()) {
        if (error)
            *error = QStringLiteral("could not replace the existing installation in %1")
                .arg(destination);
        return false;
    }

    QString copyError;
    if (!copyDirectory(sourceDirectory, destination, &copyError)) {
        if (error)
            *error = copyError;
        // Do not leave a half-copied extension behind.
        QDir(destination).removeRecursively();
        return false;
    }

    // Re-validate what actually landed on disk rather than trusting the source.
    QString verifyError;
    if (!Manifest::load(destination, &verifyError).has_value()) {
        if (error)
            *error = QStringLiteral("installed copy is not a valid extension: %1")
                .arg(verifyError);
        QDir(destination).removeRecursively();
        return false;
    }

    if (idOut)
        *idOut = name;
    return true;
}

bool ExtensionInstaller::uninstall(const QString& id, QString* error)
{
    // Only ever remove from the user directory; system directories are
    // read-only and outside this browser's ownership.
    const QString path = ExtensionPaths::userDirectory() + QLatin1Char('/') + id;
    QDir directory(path);
    if (!directory.exists()) {
        if (error)
            *error = QStringLiteral("%1 is not installed in the user directory").arg(id);
        return false;
    }
    if (!directory.removeRecursively()) {
        if (error)
            *error = QStringLiteral("could not remove %1").arg(path);
        return false;
    }

    // Stored data is separate from the extension files and would otherwise be
    // left behind, growing without bound across installs.
    const QString storage = ExtensionPaths::storageDirectory(id);
    if (QDir(storage).exists())
        QDir(storage).removeRecursively();

    return true;
}

int compareVersions(const QString& left, const QString& right)
{
    const QStringList leftParts = left.split(QLatin1Char('.'));
    const QStringList rightParts = right.split(QLatin1Char('.'));

    // Compare component by component; the shorter version is treated as having
    // trailing zeros, so "1.2" equals "1.2.0".
    const int count = qMax(leftParts.size(), rightParts.size());
    for (int index = 0; index < count; ++index) {
        const QString leftPart = index < leftParts.size() ? leftParts.at(index)
            : QStringLiteral("0");
        const QString rightPart = index < rightParts.size() ? rightParts.at(index)
            : QStringLiteral("0");

        bool leftIsNumber = false;
        bool rightIsNumber = false;
        const qlonglong leftNumber = leftPart.toLongLong(&leftIsNumber);
        const qlonglong rightNumber = rightPart.toLongLong(&rightIsNumber);

        if (leftIsNumber && rightIsNumber) {
            if (leftNumber != rightNumber)
                return leftNumber < rightNumber ? -1 : 1;
            continue;
        }
        // A non-numeric component ("beta", "rc1") is compared as text only after
        // the numeric parts agree, so 1.0.0 sorts below 1.0.1 regardless.
        const int comparison = QString::compare(leftPart, rightPart);
        if (comparison != 0)
            return comparison < 0 ? -1 : 1;
    }
    return 0;
}

UpdateCheckResult ExtensionInstaller::compareAgainstUpdateManifest(
    const QString& installedDirectory, const QByteArray& updateManifestJson)
{
    UpdateCheckResult result;

    QString error;
    auto installed = Manifest::load(installedDirectory, &error);
    if (!installed) {
        result.error = QStringLiteral("could not read the installed manifest: %1").arg(error);
        return result;
    }
    result.currentVersion = installed->version();

    QJsonParseError parseError {};
    const QJsonDocument document = QJsonDocument::fromJson(updateManifestJson, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        result.error = QStringLiteral("the update manifest is not a JSON object");
        return result;
    }

    const QString latest = document.object()
        .value(QStringLiteral("version")).toString().trimmed();
    if (latest.isEmpty()) {
        result.error = QStringLiteral("the update manifest has no version");
        return result;
    }
    result.latestVersion = latest;
    result.updateAvailable = compareVersions(latest, result.currentVersion) > 0;
    result.ok = true;
    return result;
}

UpdateCheckResult ExtensionInstaller::checkForUpdate(const QString& installedDirectory,
    const QString& source)
{
    if (source.isEmpty()) {
        UpdateCheckResult result;
        result.error = QStringLiteral("no update location is configured");
        return result;
    }

    QByteArray json;
    if (source.startsWith(QLatin1String("http://"))
        || source.startsWith(QLatin1String("https://"))) {
        const auto fetch = [&source]() -> std::pair<QByteArray, QString> {
            QNetworkAccessManager manager;
            QNetworkRequest request{ QUrl(source) };
            request.setTransferTimeout(15000);
            QNetworkReply* reply = manager.get(request);
            // Bounded spin rather than waitForFinished, which is not available on
            // QNetworkReply in Qt 6. Runs the event loop until the reply settles
            // or the deadline passes.
            QElapsedTimer timer;
            timer.start();
            while (!reply->isFinished() && timer.elapsed() < 20000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                QThread::msleep(10);
            }
            if (!reply->isFinished()) {
                reply->abort();
                reply->deleteLater();
                return { QByteArray(), QStringLiteral("the update check timed out") };
            }
            if (reply->error() != QNetworkReply::NoError) {
                const QString message = reply->errorString();
                reply->deleteLater();
                return { QByteArray(), message };
            }
            const QByteArray body = reply->readAll();
            reply->deleteLater();
            return { body, QString() };
        };

        const std::pair<QByteArray, QString> fetched = fetch();
        if (!fetched.second.isEmpty()) {
            UpdateCheckResult result;
            result.error = fetched.second;
            return result;
        }
        json = fetched.first;
    } else {
        QFile file(source);
        if (!file.open(QIODevice::ReadOnly)) {
            UpdateCheckResult result;
            result.error = QStringLiteral("could not read %1").arg(source);
            return result;
        }
        json = file.readAll();
    }

    return compareAgainstUpdateManifest(installedDirectory, json);
}

} // namespace iridium::extensions