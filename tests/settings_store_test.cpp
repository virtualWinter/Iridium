// Covers the profile-scoped settings store: defaults before a profile is
// chosen, per-profile isolation, and that switching profiles re-reads the
// preferences rather than keeping the previous profile's values in memory.

#include "ui/settings/SettingsStore.hpp"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QDir>

#include <QTemporaryDir>

#include <cstdio>
#include <string>

using iridium::SettingsStore;

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
    QTemporaryDir a;
    QTemporaryDir b;
    if (!a.isValid() || !b.isValid()) {
        std::printf("FAIL: could not create temp dirs\n");
        return 1;
    }

    SettingsStore& store = SettingsStore::instance();

    // Before a profile is chosen, accessors must be safe and return defaults
    // rather than reading through nothing.
    check(store.colorScheme() == QStringLiteral("system"),
        "no profile means the default colour scheme");
    check(!store.forcedColorScheme().has_value(),
        "no profile means no forced override");
    check(store.homePage().isEmpty(), "no profile means no homepage");
    check(store.searchTemplate().contains(QLatin1String("%1")),
        "no profile means a usable search template");
    check(store.downloadDirectory().isEmpty(), "no profile means no download override");
    // Writing before a profile exists must be dropped, not crash.
    store.setHomePage(QStringLiteral("https://ignored.test/"));
    check(store.homePage().isEmpty(), "a write with no profile is dropped");

    // Profile A.
    store.pointAtProfile(a.path());
    check(store.homePage().isEmpty(), "a fresh profile starts with no homepage");
    store.setHomePage(QStringLiteral("https://a.test/"));
    store.setColorScheme(QStringLiteral("dark"));
    store.setDownloadDirectory(QStringLiteral("/tmp/downloads-a"));
    check(store.homePage() == QStringLiteral("https://a.test/"), "profile A homepage");
    check(store.forcedColorScheme().value_or(false), "profile A is dark");

    // Profile B must not see A's values.
    store.pointAtProfile(b.path());
    check(store.homePage().isEmpty(), "profile B starts empty");
    check(store.colorScheme() == QStringLiteral("system"),
        "profile B has its own colour scheme");
    check(!store.forcedColorScheme().has_value(),
        "profile B has no forced override, even though A was dark");
    check(store.downloadDirectory().isEmpty(),
        "profile B has no download override");

    store.setHomePage(QStringLiteral("https://b.test/"));
    check(store.homePage() == QStringLiteral("https://b.test/"), "profile B homepage");

    // Back to A: its values are intact.
    store.pointAtProfile(a.path());
    check(store.homePage() == QStringLiteral("https://a.test/"),
        "profile A kept its own homepage");
    check(store.forcedColorScheme().value_or(false),
        "profile A kept its dark scheme");
    check(store.downloadDirectory() == QStringLiteral("/tmp/downloads-a"),
        "profile A kept its download directory");

    // A fresh store reading the same directory sees the same values, which is
    // what surviving a restart means.
    store.pointAtProfile(b.path());
    check(store.homePage() == QStringLiteral("https://b.test/"),
        "profile B persisted across the switch away and back");

    // Switching announces the new values, so open views and panes follow.
    {
        QSignalSpy schemeSpy(&store, &SettingsStore::forcedColorSchemeChanged);
        QSignalSpy homeSpy(&store, &SettingsStore::homePageChanged);
        store.pointAtProfile(a.path());
        check(schemeSpy.count() > 0,
            "switching a profile announces the colour scheme");
        check(homeSpy.count() > 0, "switching a profile announces the homepage");
        check(homeSpy.last().at(0).toString() == QStringLiteral("https://a.test/"),
            "the announced homepage is the new profile's");
    }

    // Search template validation: a template without the placeholder would silently
    // discard the query, so it is replaced by the default rather than stored.
    store.setSearchTemplate(QStringLiteral("https://search.test/no-placeholder"));
    check(store.searchTemplate().contains(QLatin1String("%1")),
        "a template without the placeholder is replaced by the default");
    check(!store.searchTemplate().contains(QStringLiteral("no-placeholder")),
        "the unusable template did not survive");
    store.setSearchTemplate(QStringLiteral("https://ok.test/?q=%1"));
    check(store.searchTemplate() == QStringLiteral("https://ok.test/?q=%1"),
        "a template with the placeholder round-trips");

    // Address resolution is where the search template actually takes effect.
    store.setSearchTemplate(QStringLiteral("https://search.test/?q=%1"));
    check(store.resolveAddressInput(QStringLiteral("https://example.com/x"))
        == QStringLiteral("https://example.com/x"),
        "an explicit URL is used verbatim");
    check(store.resolveAddressInput(QStringLiteral("about:blank"))
        == QStringLiteral("about:blank"),
        "an internal page is used verbatim");
    check(store.resolveAddressInput(QStringLiteral("example.com/page"))
        == QStringLiteral("https://example.com/page"),
        "a bare host becomes an https URL");
    check(store.resolveAddressInput(QStringLiteral("how do i cook rice"))
        == QStringLiteral("https://search.test/?q=how+do+i+cook+rice"),
        "a phrase is searched for");
    check(store.resolveAddressInput(QString()).isEmpty(),
        "empty input resolves to nothing");
    // A query containing a percent must be escaped, or it corrupts the URL.
    check(store.resolveAddressInput(QStringLiteral("100% cotton"))
        == QStringLiteral("https://search.test/?q=100%25+cotton"),
        "a percent in the query is escaped");

    if (g_failures == 0)
        std::printf("PASS: profile-scoped settings store\n");
    else
        std::printf("%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}