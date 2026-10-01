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
    // With no profile there is nothing configured, but a caller still gets
    // something usable: homePage() reports the built-in default rather than an
    // empty string the caller would have to handle separately.
    check(!store.hasCustomHomePage(), "no profile means no configured homepage");
    check(store.homePage() == SettingsStore::defaultHomePage(),
        "no profile means the default homepage");
    check(store.searchTemplate().contains(QLatin1String("%1")),
        "no profile means a usable search template");
    check(store.downloadDirectory().isEmpty(), "no profile means no download override");
    // Writing before a profile exists must be dropped, not crash.
    store.setHomePage(QStringLiteral("https://ignored.test/"));
    check(!store.hasCustomHomePage(), "a write with no profile is dropped");

    // Profile A.
    store.pointAtProfile(a.path());
    check(!store.hasCustomHomePage(), "a fresh profile starts with no homepage");
    check(store.homePage() == SettingsStore::defaultHomePage(),
        "a fresh profile uses the default homepage");
    store.setHomePage(QStringLiteral("https://a.test/"));
    store.setColorScheme(QStringLiteral("dark"));
    store.setDownloadDirectory(QStringLiteral("/tmp/downloads-a"));
    check(store.homePage() == QStringLiteral("https://a.test/"), "profile A homepage");
    check(store.forcedColorScheme().value_or(false), "profile A is dark");

    // Profile B must not see A's values.
    store.pointAtProfile(b.path());
    check(!store.hasCustomHomePage(), "profile B starts empty");
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

    // The rule the old code got wrong, which is why these are pinned. It asked
    // whether the parsed host contained a dot, and QUrl::fromUserInput invents a
    // host for a bare word, so "monad" resolved to a host and was navigated to.
    // Worse, fromUserInput("localhost:8080").host() is "localhost", which has no
    // dot, so a development server was searched for instead of opened. These are
    // the cases that distinguish the two rules.
    check(store.resolveAddressInput(QStringLiteral("localhost:8080"))
        == QStringLiteral("https://localhost:8080"),
        "a localhost address with a port is opened, not searched for");
    check(store.resolveAddressInput(QStringLiteral("http://localhost:3000/x"))
        == QStringLiteral("http://localhost:3000/x"),
        "a localhost URL with a scheme is used verbatim");
    check(store.resolveAddressInput(QStringLiteral("127.0.0.1:8080"))
        == QStringLiteral("https://127.0.0.1:8080"),
        "an IPv4 address with a port is opened");
    check(store.resolveAddressInput(QStringLiteral("sub.domain.co.uk/a?b=c#d"))
        == QStringLiteral("https://sub.domain.co.uk/a?b=c#d"),
        "a multi-label host with a path, query and fragment is opened");
    check(store.resolveAddressInput(QStringLiteral("example.com:8443"))
        == QStringLiteral("https://example.com:8443"),
        "a host with a port is opened");
    // A bare word is a search, not a host. The old dot test called this a URL.
    check(store.resolveAddressInput(QStringLiteral("monad"))
        == QStringLiteral("https://search.test/?q=monad"),
        "a single word is searched for, not treated as a host");
    check(store.resolveAddressInput(QStringLiteral("3.14"))
        == QStringLiteral("https://search.test/?q=3.14"),
        "a bare number is searched for");
    // A single label with no dot is ambiguous, and far more likely to be a
    // phrase, so it is searched for. The "/" is not escaped: a search query does
    // not need it to be, and encoding it would be a gratuitous difference from
    // what the user typed.
    check(store.resolveAddressInput(QStringLiteral("intranet/page"))
        == QStringLiteral("https://search.test/?q=intranet/page"),
        "a single-label host with a path is searched for");
    // A scheme the engine cannot display is still a URL: saying "I cannot load
    // this" beats silently searching for the text.
    check(store.resolveAddressInput(QStringLiteral("ftp://host/file"))
        == QStringLiteral("ftp://host/file"),
        "an unhandled scheme is passed to the engine rather than searched for");
    check(store.resolveAddressInput(QStringLiteral("iridium-extension://abc/x"))
        == QStringLiteral("iridium-extension://abc/x"),
        "an extension URL is passed through");
    // A colon in a phrase is not a scheme.
    check(store.resolveAddressInput(QStringLiteral("note: remember this"))
        == QStringLiteral("https://search.test/?q=note:+remember+this"),
        "a colon in a phrase does not make it a URL");

    // looksLikeAddress is what the general pane uses to warn about a homepage that
    // would only be searched for. It has to agree with the rule above: if it
    // disagreed, the warning would be about a different decision from the one
    // actually made, which is worse than no warning. Asserted as "the resolved
    // value is a URL, not a search", so the two cannot drift.
    for (const char* address : { "https://example.com", "http://localhost:3000/x",
             "example.com/page", "localhost:8080", "localhost", "127.0.0.1",
             "about:blank", "sub.example.co.uk/a", "example.com:8443",
             "iridium-extension://abc/x" }) {
        const QString input = QString::fromLatin1(address);
        const QString resolved = store.resolveAddressInput(input);
        check(SettingsStore::looksLikeAddress(input),
            std::string("looksLikeAddress accepts ") + address);
        check(!resolved.contains(QLatin1String("search.test")),
            std::string("and it is not searched for: ") + address);
    }
    for (const char* phrase : { "monad", "how do i cook rice", "intranet/page",
             "3.14", "not a url at all", "note: remember this" }) {
        check(!SettingsStore::looksLikeAddress(QString::fromLatin1(phrase)),
            std::string("looksLikeAddress rejects the phrase ") + phrase);
        check(store.resolveAddressInput(QString::fromLatin1(phrase))
                .contains(QLatin1String("search.test")),
            std::string("and it really is searched for: ") + phrase);
    }
    // Empty input is neither: it resolves to nothing, so there is no URL to open
    // and no search to run. Asserted separately because folding it into the
    // phrases above would assert a search that does not happen.
    check(!SettingsStore::looksLikeAddress(QString()),
        "looksLikeAddress rejects empty input");
    check(store.resolveAddressInput(QString()).isEmpty(),
        "empty input still resolves to nothing");

    if (g_failures == 0)
        std::printf("PASS: profile-scoped settings store\n");
    else
        std::printf("%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}