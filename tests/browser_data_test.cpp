// Covers profiles and history: name validation, per-profile isolation, the
// default profile being undeletable, and history recording, search, dedup,
// range queries, retention pruning and deletion.

#include "browser/HistoryStore.hpp"
#include "browser/ProfileManager.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include <cstdio>
#include <string>

using iridium::Profile;
using iridium::ProfileManager;
using iridium::history::HistoryStore;
using iridium::history::Query;
using iridium::history::Visit;

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    QTemporaryDir xdg;
    if (!xdg.isValid()) {
        std::printf("FAIL: could not create a temp data home\n");
        return 1;
    }
    qputenv("XDG_DATA_HOME", xdg.path().toUtf8());

    // --- profile names -------------------------------------------------
    {
        QString error;
        check(ProfileManager::isValidName(QStringLiteral("work"), &error),
            "a plain name is valid");
        check(!ProfileManager::isValidName(QString(), &error),
            "an empty name is refused");
        check(!ProfileManager::isValidName(QStringLiteral("../escape"), &error),
            "a traversal name is refused");
        check(!ProfileManager::isValidName(QStringLiteral("a/b"), &error),
            "a separator is refused");
        check(!ProfileManager::isValidName(QStringLiteral(" leading"), &error),
            "a leading space is refused");
        check(!ProfileManager::isValidName(QStringLiteral("bad\\name"), &error),
            "a backslash is refused");
        check(!ProfileManager::isValidName(QString(70, QLatin1Char('x')), &error),
            "an over-long name is refused");
    }

    // --- creation and listing -------------------------------------------
    ProfileManager& profiles = ProfileManager::instance();
    check(profiles.current().isValid(), "a current profile exists on first run");
    check(profiles.current().builtin, "the first run lands on the default profile");
    check(profiles.current().name == QStringLiteral("default"), "named default");
    check(QDir(profiles.current().path).exists(),
        "the profile directory was created");

    QString error;
    check(profiles.create(QStringLiteral("work"), &error),
        "a profile can be created: " + error.toStdString());
    check(profiles.exists(QStringLiteral("work")), "the new profile is listed");
    check(!profiles.create(QStringLiteral("work"), &error),
        "a duplicate name is refused");
    check(!profiles.create(QStringLiteral("bad/name"), &error),
        "an invalid name is refused");

    const QVector<Profile> all = profiles.profiles();
    check(all.size() == 2, "two profiles exist, got "
        + std::to_string(all.size()));
    check(all.first().builtin, "the default profile is listed first");

    // A directory without profile.json is not a profile.
    QDir().mkpath(xdg.path() + QStringLiteral("/iridium/profiles/not-a-profile"));
    check(profiles.profiles().size() == 2,
        "a stray directory is not offered as a profile");

    // --- the default profile is protected -------------------------------
    check(!profiles.remove(QStringLiteral("default"), &error),
        "the default profile cannot be removed");
    check(!profiles.remove(QStringLiteral("absent"), &error),
        "removing an unknown profile fails");
    check(profiles.exists(QStringLiteral("default")),
        "the default profile survived the attempt");

    // --- history per profile --------------------------------------------
    check(profiles.switchTo(QStringLiteral("work"), &error),
        "switched to the work profile: " + error.toStdString());
    check(profiles.current().name == QStringLiteral("work"), "the switch took effect");

    Profile workProfile = profiles.current();
    Profile defaultProfile = profiles.byName(QStringLiteral("default"));

    HistoryStore work(workProfile);
    // Evaluated into a separate statement: argument evaluation order is
    // unspecified, so folding open() and reading `error` into one call can read
    // the previous value.
    error.clear();
    const bool opened = work.open(&error);
    check(opened, "history opens for the work profile: " + error.toStdString());
    if (!opened) {
        std::printf("  path was %s\n", qPrintable(workProfile.path));
        return 1;
    }

    check(work.recordVisit(QStringLiteral("https://example.com/a"), QStringLiteral("Example A")),
        "a visit is recorded");
    check(work.recordVisit(QStringLiteral("https://example.com/b"), QStringLiteral("Example B")),
        "a second visit is recorded");
    check(work.entryCount() == 2, "two entries exist");

    // A visit recorded before its title has arrived. This is the common path:
    // navigation knows the URL first, and the title follows. It must not be
    // rejected as a null title.
    check(work.recordVisit(QStringLiteral("https://example.com/untitled"),
        QStringLiteral()),
        "a visit with no title yet is recorded");
    {
        Query query;
        query.text = QStringLiteral("untitled");
        const std::vector<Visit> found = work.search(query, &error);
        check(found.size() == 1, "the untitled entry exists");
        check(!found.empty() && found.front().title.isEmpty(),
            "and it has an empty title, not a null one");
        check(!found.empty() && found.front().visitCount == 1,
            "counted once");
        // The title arriving later fills it in, still without adding an entry.
        check(work.recordVisit(QStringLiteral("https://example.com/untitled"),
            QStringLiteral("Now titled")),
            "the later title is recorded");
        query.text = QStringLiteral("Now titled");
        const std::vector<Visit> titled = work.search(query, &error);
        check(titled.size() == 1, "the entry was titled rather than duplicated");
        check(!titled.empty() && titled.front().visitCount == 2,
            "and both visits counted");
        work.forget(QStringLiteral("https://example.com/untitled"), &error);
    }

    // A repeat visit updates rather than duplicating.
    check(work.recordVisit(QStringLiteral("https://example.com/a"),
        QStringLiteral("Example A renamed")),
        "a repeat visit is recorded");
    check(work.entryCount() == 2, "a repeat visit did not add a row");

    {
        Query query;
        const std::vector<Visit> found = work.search(query, &error);
        check(found.size() == 2, "search returns both entries");
        for (const Visit& visit : found) {
            if (visit.url == QStringLiteral("https://example.com/a")) {
                check(visit.visitCount == 2, "the repeat visit counted");
                check(visit.title == QStringLiteral("Example A renamed"),
                    "a later title replaces the earlier one");
            }
        }
        // Newest first.
        check(!found.empty()
            && found.front().lastVisit >= found.back().lastVisit,
            "results are newest first");
    }

    // Substring search across URL and title, case-insensitive.
    {
        Query query;
        query.text = QStringLiteral("renamed");
        check(work.search(query).size() == 1, "search matches the title");
        query.text = QStringLiteral("example.com");
        check(work.search(query).size() == 2, "search matches the url");
        query.text = QStringLiteral("EXAMPLE.COM");
        check(work.search(query).size() == 2, "search is case-insensitive");
        query.text = QStringLiteral("nothing matches this");
        check(work.search(query).empty(), "a non-matching search is empty");
    }

    // LIKE wildcards in the query are literal, not wildcards.
    {
        Query query;
        query.text = QStringLiteral("%");
        check(work.search(query).empty(),
            "a % in the search text is literal, not a wildcard");
        query.text = QStringLiteral("_xample");
        check(work.search(query).empty(), "an _ in the search text is literal");
    }

    // A quote in the search text must not break the statement.
    {
        Query query;
        query.text = QStringLiteral("'; DROP TABLE visits; --");
        work.search(query, &error);
        check(error.isEmpty(), "a quote in the search text is handled safely");
        check(work.entryCount() == 2, "the table survived it");
    }

    // Time-range queries. Entries are written with the current time, so an
    // upper bound just before it excludes everything and a lower bound includes
    // everything.
    {
        Query query;
        query.until = QDateTime::currentDateTimeUtc().addSecs(-60);
        check(work.search(query).empty(), "a range in the future excludes the entries");
        query.since = QDateTime::currentDateTimeUtc().addSecs(-3600);
        query.until = QDateTime::currentDateTimeUtc().addSecs(3600);
        check(work.search(query).size() == 2, "a range around now includes them");
    }

    // Limit.
    {
        Query query;
        query.limit = 1;
        check(work.search(query).size() == 1, "the limit is honoured");
    }

    // Forgetting one entry.
    check(work.forget(QStringLiteral("https://example.com/b"), &error),
        "an entry can be forgotten");
    check(!work.forget(QStringLiteral("https://example.com/b"), &error),
        "forgetting it again reports nothing removed");
    check(work.entryCount() == 1, "one entry remains");

    // Pruning by count keeps the newest.
    for (int index = 0; index < 10; ++index) {
        work.recordVisit(QStringLiteral("https://bulk.test/%1").arg(index),
            QStringLiteral("Bulk %1").arg(index));
    }
    check(work.entryCount() == 11, "eleven entries before pruning");
    const int removed = work.prune(5, 0, &error);
    check(removed == 6, "pruning removed the overflow, got "
        + std::to_string(removed));
    check(work.entryCount() == 5, "five entries remain");
    {
        Query query;
        query.limit = 50;
        const std::vector<Visit> newest = work.search(query);
        check(!newest.empty()
            && newest.front().url.contains(QStringLiteral("bulk.test/9")),
            "pruning kept the most recent entries");
    }

    // Retention by age. Nothing is old enough yet, so nothing is removed.
    check(work.prune(0, 365, &error) == 0,
        "nothing is pruned while every entry is recent");

    // An age cutoff that excludes everything must clear the table, which is the
    // path the startup sweep takes when a profile has not been opened in months.
    check(work.prune(0, 0, &error) == 0,
        "a zero retention keeps everything rather than deleting it all");
    {
        // Every visit is stamped with the current time, so only a cutoff in the
        // future can exclude them. A cutoff in the past would match nothing,
        // which is the case above.
        const QDateTime tomorrow = QDateTime::currentDateTimeUtc().addDays(1);
        check(work.countOlderThan(tomorrow) == work.entryCount(),
            "every entry counts as older than tomorrow, got "
                + std::to_string(work.countOlderThan(tomorrow)));
        check(work.clear(tomorrow, &error),
            "clearing everything older than tomorrow empties the table");
        check(work.entryCount() == 0, "and the table is empty");
    }

    // Clearing.
    check(work.clear(std::nullopt, &error), "history can be cleared");
    check(work.entryCount() == 0, "clearing emptied the table");

    // --- isolation between profiles ------------------------------------
    check(work.recordVisit(QStringLiteral("https://work-only.test/"), QStringLiteral("Work")),
        "a visit is recorded in the work profile");
    work.close();

    {
        HistoryStore defaultHistory(defaultProfile);
        check(defaultHistory.open(&error),
            "history opens for the default profile: " + error.toStdString());
        check(defaultHistory.entryCount() == 0,
            "the default profile's history is unaffected by the work profile");
        defaultHistory.recordVisit(QStringLiteral("https://default-only.test/"),
            QStringLiteral("Default"));
        check(defaultHistory.search(Query {}).size() == 1,
            "the default profile has only its own entry");
        defaultHistory.close();
    }

    {
        // Reopening the work profile must see its own entries, so the database
        // is really per profile rather than shared.
        HistoryStore reopened(workProfile);
        check(reopened.open(&error), "the work history reopens");
        check(reopened.entryCount() == 1,
            "the work profile kept its own entry");
        const std::vector<Visit> entries = reopened.search(Query {});
        check(entries.size() == 1
            && entries.front().url == QStringLiteral("https://work-only.test/"),
            "and it is the right entry");
        reopened.close();
    }

    // --- switching and deleting profiles --------------------------------
    // Deleting the current profile must not leave it selected.
    check(profiles.switchTo(QStringLiteral("default"), &error), "switched back");
    check(profiles.remove(QStringLiteral("work"), &error),
        "the work profile is removed: " + error.toStdString());
    check(!profiles.exists(QStringLiteral("work")), "it is gone from the list");
    check(profiles.current().name == QStringLiteral("default"),
        "the current profile fell back to default");

    if (g_failures == 0)
        std::printf("PASS: profiles and history\n");
    else
        std::printf("%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}