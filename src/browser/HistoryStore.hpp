#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

#include <QSqlDatabase>

#include <optional>
#include <vector>

namespace iridium {
class Profile;
}

namespace iridium::history {

// One visit, as history records it. A page visited several times is one entry
// with a visit count and the most recent visit time, matching what a browser
// history shows rather than one row per visit.
struct Visit {
    QString url;
    QString title;
    QDateTime lastVisit;
    int visitCount { 0 };

    bool isValid() const { return !url.isEmpty(); }
};

// A search request. Empty fields are ignored, so the default matches everything.
struct Query {
    // Substring of the URL or title, case-insensitive.
    QString text;
    // Newest first. An unset limit means unbounded, which is fine for a
    // deliberate "search everything" but not for the default listing.
    int limit { 200 };
    std::optional<QDateTime> since;
    std::optional<QDateTime> until;
};

// Browsing history, stored per profile in SQLite.
//
// SQLite rather than a flat file because history grows without bound and needs
// substring search and time-range queries; a JSON file would have to be read
// and rewritten in full for every visit.
class HistoryStore final {
public:
    explicit HistoryStore(const Profile& profile);
    ~HistoryStore();

    HistoryStore(const HistoryStore&) = delete;
    HistoryStore& operator=(const HistoryStore&) = delete;

    // Opens (and creates) the database for the profile. Returns false and sets
    // `error` if the profile's directory is unusable.
    bool open(QString* error);
    bool isOpen() const { return m_open; }
    void close();

    // Records a visit. A repeated visit to the same URL updates the entry rather
    // than adding one. Returns false on failure.
    bool recordVisit(const QString& url, const QString& title, QString* error = nullptr);

    // Newest first.
    std::vector<Visit> search(const Query& query, QString* error = nullptr) const;

    // Removes one entry by URL. Returns false when it was not there.
    bool forget(const QString& url, QString* error = nullptr);
    // Removes everything, or only entries older than `before`.
    bool clear(std::optional<QDateTime> before = std::nullopt, QString* error = nullptr);

    int entryCount(QString* error = nullptr) const;
    // Entries older than `before`, i.e. what a retention sweep would remove.
    int countOlderThan(const QDateTime& before, QString* error = nullptr) const;

    // URLs visited most recently, for autocomplete. Ordered by recency.
    QStringList recentUrls(int limit, QString* error = nullptr) const;

    // Drops entries older than `retention` and, if `maximumEntries` is positive,
    // the oldest beyond that count. Keeps the table from growing forever.
    // Returns the number removed.
    int prune(int maximumEntries, int retentionDays, QString* error = nullptr);

    // Removes every trace of `url` from anywhere in the profile, used when a
    // profile is deleted. Best effort.
    static void removeDatabase(const Profile& profile);

private:
    bool createSchema(QString* error);
    QString databasePath() const;

    QString m_path;
    QSqlDatabase m_database;
    bool m_open { false };
    // QSqlDatabase is a value type keyed by name; the name is kept so close()
    // can remove the connection and avoid leaking it across profiles.
    QString m_connectionName;
};

} // namespace iridium::history