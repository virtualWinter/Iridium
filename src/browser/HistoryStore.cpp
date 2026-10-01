#include "browser/HistoryStore.hpp"

#include "browser/ProfileManager.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

namespace iridium::history {

namespace {

// Every value crossing into SQL is bound, never interpolated, so a title
// containing a quote cannot alter the statement.
// One statement per element: the SQLite driver cannot execute several
// statements in a single exec() call.
constexpr char kCreateTable[] =
    "CREATE TABLE IF NOT EXISTS visits ("
    " url TEXT PRIMARY KEY NOT NULL,"
    " title TEXT NOT NULL DEFAULT '',"
    " last_visit INTEGER NOT NULL,"
    " visit_count INTEGER NOT NULL DEFAULT 1)";
// Descending matches the default ordering, so the index is used as written.
constexpr char kCreateIndex[] =
    "CREATE INDEX IF NOT EXISTS visits_last_visit ON visits(last_visit DESC)";

QDateTime fromMillis(const QVariant& value)
{
    const qint64 millis = value.toLongLong();
    return millis > 0 ? QDateTime::fromMSecsSinceEpoch(millis) : QDateTime();
}

// A default-constructed QString binds as SQL NULL, which the NOT NULL title
// column rejects. Qt distinguishes a null string from an empty one, so the
// empty case is spelled out here.
//
// This is not an edge case: a visit is recorded on navigation, when the URL is
// known but the title usually has not arrived yet, so the title is empty for
// most visits. The title is filled in by a second recordVisit when it does.
QString boundText(const QString& text)
{
    return text.isEmpty() ? QString::fromLatin1("") : text;
}

} // namespace

HistoryStore::HistoryStore(const Profile& profile)
{
    m_path = profile.path + QStringLiteral("/history.sqlite");
}

HistoryStore::~HistoryStore()
{
    close();
}

QString HistoryStore::databasePath() const
{
    return m_path;
}

bool HistoryStore::open(QString* error)
{
    const auto fail = [error](const QString& reason) {
        if (error)
            *error = reason;
        return false;
    };

    if (m_open)
        return true;

    if (!QDir().mkpath(QFileInfo(m_path).absolutePath()))
        return fail(QStringLiteral("could not create the profile directory for history"));

    // A per-instance connection name, so two stores (two profiles open at once
    // during a switch) do not collide on the default connection.
    m_connectionName = QStringLiteral("iridium-history-%1")
        .arg(QUuid::createUuid().toString(QUuid::Id128));
    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    if (!m_database.isValid())
        return fail(QStringLiteral("no SQLite driver is available"));

    m_database.setDatabaseName(m_path);
    if (!m_database.open())
        return fail(QStringLiteral("could not open history: %1")
            .arg(m_database.lastError().text()));

    // Write-ahead logging keeps a visit from blocking on a reader, and foreign
    // keys are off by default in SQLite.
    QSqlQuery pragma(m_database);
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));

    if (!createSchema(error)) {
        m_database.close();
        return false;
    }

    m_open = true;
    return true;
}

bool HistoryStore::createSchema(QString* error)
{
    QSqlQuery query(m_database);
    for (const char* statement : { kCreateTable, kCreateIndex }) {
        if (!query.exec(QString::fromLatin1(statement))) {
            if (error)
                *error = QStringLiteral("could not prepare history: %1")
                    .arg(query.lastError().text());
            return false;
        }
    }
    return true;
}

void HistoryStore::close()
{
    if (!m_open) {
        if (!m_connectionName.isEmpty()) {
            QSqlDatabase::removeDatabase(m_connectionName);
            m_connectionName.clear();
        }
        return;
    }

    m_database.close();
    // The connection must be removed before the QSqlDatabase copy goes away,
    // or Qt warns about a dangling connection at shutdown.
    QSqlDatabase::removeDatabase(m_connectionName);
    m_connectionName.clear();
    m_open = false;
}

bool HistoryStore::recordVisit(const QString& url, const QString& title, QString* error)
{
    if (!m_open) {
        if (error)
            *error = QStringLiteral("history is not open");
        return false;
    }
    const QString trimmed = url.trimmed();
    if (trimmed.isEmpty())
        return true;

    // A redirect landing on the same URL, or a reload, should not add a row.
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO visits(url, title, last_visit, visit_count) VALUES(?, ?, ?, 1) "
        "ON CONFLICT(url) DO UPDATE SET "
        "  last_visit = excluded.last_visit, "
        "  visit_count = visits.visit_count + 1, "
        // Keep a real title rather than blanking one that already arrived.
        "  title = CASE WHEN excluded.title = '' THEN visits.title ELSE excluded.title END"));
    query.addBindValue(trimmed);
    query.addBindValue(boundText(title.trimmed()));
    query.addBindValue(QDateTime::currentDateTimeUtc().toMSecsSinceEpoch());

    if (!query.exec()) {
        if (error)
            *error = QStringLiteral("could not record the visit: %1")
                .arg(query.lastError().text());
        return false;
    }
    return true;
}

std::vector<Visit> HistoryStore::search(const Query& query, QString* error) const
{
    std::vector<Visit> results;
    if (!m_open) {
        if (error)
            *error = QStringLiteral("history is not open");
        return results;
    }

    QSqlQuery statement(m_database);
    QString sql = QStringLiteral(
        "SELECT url, title, last_visit, visit_count FROM visits WHERE 1 = 1");
    QVariantList bindings;

    if (!query.text.trimmed().isEmpty()) {
        // Escaped so a search for "50%" is not a wildcard pattern.
        QString pattern = query.text.trimmed();
        pattern.replace(QLatin1String("\\"), QLatin1String("\\\\"));
        pattern.replace(QLatin1String("%"), QLatin1String("\\%"));
        pattern.replace(QLatin1String("_"), QLatin1String("\\_"));
        sql += QStringLiteral(
            " AND (url LIKE ? ESCAPE '\\' OR title LIKE ? ESCAPE '\\')");
        bindings.append(QStringLiteral("%%%1%%").arg(pattern));
        bindings.append(QStringLiteral("%%%1%%").arg(pattern));
    }
    if (query.since) {
        sql += QStringLiteral(" AND last_visit >= ?");
        bindings.append(query.since->toMSecsSinceEpoch());
    }
    if (query.until) {
        sql += QStringLiteral(" AND last_visit <= ?");
        bindings.append(query.until->toMSecsSinceEpoch());
    }

    // Newest first. Two visits can land in the same millisecond, so the tiebreak
    // falls back to rowid, which SQLite assigns in insertion order; without it
    // pruning could discard the entries added last.
    sql += QStringLiteral(" ORDER BY last_visit DESC, rowid DESC");
    if (query.limit > 0) {
        sql += QStringLiteral(" LIMIT ?");
        bindings.append(query.limit);
    }

    statement.prepare(sql);
    for (const QVariant& binding : bindings)
        statement.addBindValue(binding);
    if (!statement.exec()) {
        if (error)
            *error = QStringLiteral("history search failed: %1")
                .arg(statement.lastError().text());
        return results;
    }

    while (statement.next()) {
        Visit visit;
        visit.url = statement.value(0).toString();
        visit.title = statement.value(1).toString();
        visit.lastVisit = fromMillis(statement.value(2));
        visit.visitCount = statement.value(3).toInt();
        results.push_back(std::move(visit));
    }
    return results;
}

bool HistoryStore::forget(const QString& url, QString* error)
{
    if (!m_open) {
        if (error)
            *error = QStringLiteral("history is not open");
        return false;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM visits WHERE url = ?"));
    query.addBindValue(url);
    if (!query.exec()) {
        if (error)
            *error = QStringLiteral("could not remove the entry: %1")
                .arg(query.lastError().text());
        return false;
    }
    return query.numRowsAffected() > 0;
}

bool HistoryStore::clear(std::optional<QDateTime> before, QString* error)
{
    if (!m_open) {
        if (error)
            *error = QStringLiteral("history is not open");
        return false;
    }
    QSqlQuery query(m_database);
    if (before) {
        query.prepare(QStringLiteral("DELETE FROM visits WHERE last_visit < ?"));
        query.addBindValue(before->toMSecsSinceEpoch());
    } else {
        query.prepare(QStringLiteral("DELETE FROM visits"));
    }
    if (!query.exec()) {
        if (error)
            *error = QStringLiteral("could not clear history: %1")
                .arg(query.lastError().text());
        return false;
    }
    return true;
}

int HistoryStore::entryCount(QString* error) const
{
    if (!m_open) {
        if (error)
            *error = QStringLiteral("history is not open");
        return 0;
    }
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM visits"))
        || !query.next()) {
        if (error)
            *error = query.lastError().text();
        return 0;
    }
    return query.value(0).toInt();
}

int HistoryStore::countOlderThan(const QDateTime& before, QString* error) const
{
    if (!m_open) {
        if (error)
            *error = QStringLiteral("history is not open");
        return 0;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM visits WHERE last_visit < ?"));
    query.addBindValue(before.toMSecsSinceEpoch());
    if (!query.exec() || !query.next()) {
        if (error)
            *error = query.lastError().text();
        return 0;
    }
    return query.value(0).toInt();
}

QStringList HistoryStore::recentUrls(int limit, QString* error) const
{
    QStringList result;
    if (!m_open) {
        if (error)
            *error = QStringLiteral("history is not open");
        return result;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT url FROM visits ORDER BY last_visit DESC LIMIT ?"));
    query.addBindValue(limit > 0 ? limit : 50);
    if (!query.exec()) {
        if (error)
            *error = query.lastError().text();
        return result;
    }
    while (query.next())
        result.append(query.value(0).toString());
    return result;
}

int HistoryStore::prune(int maximumEntries, int retentionDays, QString* error)
{
    if (!m_open) {
        if (error)
            *error = QStringLiteral("history is not open");
        return 0;
    }

    QSqlQuery query(m_database);
    int removed = 0;

    // Age first, so an old table shrinks even when the cap is generous.
    if (retentionDays > 0) {
        const QDateTime cutoff = QDateTime::currentDateTimeUtc()
            .addDays(-retentionDays);
        query.prepare(QStringLiteral("DELETE FROM visits WHERE last_visit < ?"));
        query.addBindValue(cutoff.toMSecsSinceEpoch());
        if (query.exec()) {
            removed += query.numRowsAffected();
        } else if (error) {
            *error = query.lastError().text();
        }
    }

    if (maximumEntries > 0) {
        const int total = entryCount();
        if (total > maximumEntries) {
            // Delete everything past the cap, keeping the most recent. Done in
            // one statement so it cannot race with concurrent visits into a
            // half-applied state.
            query.prepare(QStringLiteral(
                "DELETE FROM visits WHERE url IN ("
                "  SELECT url FROM visits ORDER BY last_visit DESC, rowid DESC LIMIT -1 OFFSET ?"
                ")"));
            query.addBindValue(maximumEntries);
            if (query.exec()) {
                removed += query.numRowsAffected();
            } else if (error) {
                *error = query.lastError().text();
            }
        }
    }

    return removed;
}

void HistoryStore::removeDatabase(const Profile& profile)
{
    const QString path = profile.path + QStringLiteral("/history.sqlite");
    // The database may be open in this process; removing the files is still the
    // right cleanup, since a profile being deleted is no longer referenced.
    QFile::remove(path);
    QFile::remove(path + QStringLiteral("-wal"));
    QFile::remove(path + QStringLiteral("-shm"));
}

} // namespace iridium::history