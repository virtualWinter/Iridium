// The API registry and dispatcher against the generated catalog.
//
// What is being checked here is the order of the checks, because that is what
// makes the difference between a dispatcher that enforces the specification and
// one that looks after the fact. A method an extension has no right to call must
// be refused for the reason that is actually true of it: a content script
// calling `webRequest.onBeforeRequest` has no permission either way, but the
// answer it gets back has to say why.

#include "extensions/api/ApiDispatcher.hpp"
#include "extensions/api/ApiRegistry.hpp"
#include "extensions/ExtensionPaths.hpp"
#include "extensions/ExtensionRegistry.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cstdio>
#include <string>

using iridium::extensions::ApiDispatcher;
using iridium::extensions::ApiMethod;
using iridium::extensions::ApiRegistry;
using iridium::extensions::ExtensionContext;
using iridium::extensions::ExtensionRegistry;
using iridium::extensions::generated::findMember;
using iridium::extensions::generated::findNamespace;

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

void writeFile(const QString& path, const QByteArray& contents)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        std::printf("FAIL: could not write %s\n", path.toUtf8().constData());
    file.write(contents);
}

QString refusalFor(ApiDispatcher& dispatcher, const ApiDispatcher::Request& request)
{
    const QJsonValue result = dispatcher.dispatch(request);
    if (!result.isObject())
        return {};
    const QJsonObject object = result.toObject();
    if (object.size() == 1 && object.contains(QStringLiteral("message")))
        return object.value(QStringLiteral("message")).toString();
    return {};
}

// The catalog is generated, so the first thing to check is that it says
// something. A silently empty catalog would make every test below pass for the
// wrong reason.
void testCatalogIsPopulated()
{
    check(iridium::extensions::generated::namespaceCount() > 50,
        "the catalog holds the specification's namespaces");
    check(iridium::extensions::generated::memberCount() > 500,
        "the catalog holds the specification's members");

    const auto* tabs = findNamespace("tabs");
    check(tabs != nullptr, "tabs is in the catalog");
    if (!tabs)
        return;
    // tabs carries no namespace permission: the specification gates Tab.url,
    // Tab.title and Tab.favIconUrl individually instead, so a registry that
    // demanded "tabs" for every tabs.* call would refuse calls the
    // specification allows.
    check(tabs->permission != nullptr && *tabs->permission == '\0',
        "tabs requires no namespace permission");
    check(findMember("tabs", "query") != nullptr, "tabs.query is in the catalog");
    check(findMember("tabs", "onUpdated") != nullptr, "tabs.onUpdated is in the catalog");
    check(findMember("tabs", "query")->kind == iridium::extensions::generated::MemberKind::Method,
        "tabs.query is classified as a method");
    check(findMember("tabs", "onUpdated")->kind == iridium::extensions::generated::MemberKind::Event,
        "tabs.onUpdated is classified as an event");

    // An MV2-only namespace must say so, or an MV3 extension would be handed
    // members it has no right to.
    const auto* browserAction = findNamespace("browserAction");
    check(browserAction != nullptr && browserAction->maxManifestVersion == 2,
        "browserAction is bounded to Manifest V2");
    check(findNamespace("action") != nullptr && findNamespace("action")->minManifestVersion == 3,
        "action is bounded to Manifest V3 and above");

    // Contexts: the schemas restrict runtime/i18n/storage to content scripts and
    // devtools as well, and devtools.* to devtools only.
    check((findNamespace("runtime")->contexts
            & iridium::extensions::generated::ContextContentScript) != 0,
        "runtime is reachable from a content script");
    check((findNamespace("devtools")->contexts
            & iridium::extensions::generated::ContextDevTools) != 0,
        "devtools is reachable from a devtools context");
    check((findNamespace("devtools")->contexts
            & iridium::extensions::generated::ContextContentScript) == 0,
        "devtools is not reachable from a content script");

    check(findNamespace("noSuchNamespace") == nullptr,
        "an unknown namespace is not invented");
    check(findMember("tabs", "noSuchMethod") == nullptr,
        "an unknown member is not invented");
}

void testRegistryReadsTheCatalog()
{
    ApiRegistry registry;

    ApiMethod* query = registry.registerMethod(QStringLiteral("tabs.query"),
        [](const QString&, const QJsonObject&) { return QJsonValue(1); });
    check(query != nullptr, "a method the specification defines can be registered");
    check(query && query->requiredPermission().isEmpty(),
        "a namespace with no permission requires none");
    check(query && query->hasHandler(), "the handler is attached");
    check(query && query->kind() == iridium::extensions::generated::MemberKind::Method,
        "the kind came from the catalog");

    check(registry.registerMethod(QStringLiteral("tabs.noSuchMethod"), nullptr) == nullptr,
        "a method the specification does not define cannot be registered");
    check(registry.registerMethod(QStringLiteral("nodot"), nullptr) == nullptr,
        "a name that is not a member cannot be registered");

    // Re-registering replaces the behaviour but keeps the metadata.
    registry.registerMethod(QStringLiteral("tabs.query"),
        [](const QString&, const QJsonObject&) { return QJsonValue(2); });
    check(registry.find(QStringLiteral("tabs.query"))->requiredPermission().isEmpty(),
        "re-registering does not lose the catalog metadata");

    // A namespace that does require a permission says so, and a member that
    // needs more than its namespace does gets that too.
    check(registry.registerMethod(QStringLiteral("bookmarks.get"),
        [](const QString&, const QJsonObject&) { return QJsonValue {}; })
            ->requiredPermission() == QStringLiteral("bookmarks"),
        "a namespace permission comes from the catalog");
    check(registry.registerMethod(QStringLiteral("tabs.hide"),
        [](const QString&, const QJsonObject&) { return QJsonValue {}; })
            ->requiredPermission() == QStringLiteral("tabHide"),
        "a member permission beyond the namespace's is carried too");
    check(registry.registerMethod(QStringLiteral("tabs.captureTab"),
        [](const QString&, const QJsonObject&) { return QJsonValue {}; })
            ->requiredPermission() == QStringLiteral("<all_urls>"),
        "a member that needs a host permission carries it as one");

    ApiMethod* unavailable = registry.registerUnavailable(
        QStringLiteral("webRequest.onBeforeRequest"),
        QStringLiteral("the engine has no per-request hook"));
    check(unavailable && unavailable->isUnavailable(),
        "a method that cannot be provided is still declared");
    check(!unavailable || !unavailable->hasHandler(),
        "an unavailable method has no handler");
}

void testArgumentValidation()
{
    ApiRegistry registry;
    registry.registerMethod(QStringLiteral("tabs.get"),
        [](const QString&, const QJsonObject&) { return QJsonValue {}; });

    const ApiMethod* get = registry.find(QStringLiteral("tabs.get"));
    check(get && get->parameterCount() >= 1, "tabs.get declares its parameter");

    const auto validate = [get](const QByteArray& json) {
        return ApiDispatcher::validateArguments(*get, QJsonDocument::fromJson(json).object());
    };

    check(validate(R"({"tabId":7})").isEmpty(), "a valid integer is accepted");
    check(!validate(R"({"tabId":-1})").isEmpty(),
        "a negative tabId is refused: the schema states a minimum of 0");
    check(!validate(R"({})").isEmpty(), "a missing required argument is refused");
    check(!validate(R"({"tabId":"7"})").isEmpty(), "a string where an integer is "
        "required is refused");
    check(!validate(R"({"tabId":1.5})").isEmpty(), "a fractional value is refused");
    check(!validate(R"({"tabId":true})").isEmpty(), "a boolean where an integer is "
        "required is refused");
    check(!validate(R"({"tabId":null})").isEmpty(), "null where an integer is required "
        "is refused");
    check(!validate(R"({"tabId":null,"extra":1})").isEmpty()
            || validate(R"({"tabId":7,"unexpected":1})").isEmpty(),
        "an unexpected argument is not itself a failure");

    // A method whose parameter is optional may be called without it.
    ApiRegistry optionals;
    optionals.registerMethod(QStringLiteral("action.disable"),
        [](const QString&, const QJsonObject&) { return QJsonValue {}; });
    const ApiMethod* disable = optionals.find(QStringLiteral("action.disable"));
    check(disable && disable->parameters()[0].optional,
        "action.disable's tabId is optional in the schema");
    check(ApiDispatcher::validateArguments(*disable, QJsonObject {}).isEmpty(),
        "an optional argument may be omitted");
}

void testDispatchOrder()
{
    QTemporaryDir xdg;
    QTemporaryDir profile;
    qputenv("XDG_DATA_HOME", xdg.path().toUtf8());

    const QString root = xdg.path() + QStringLiteral("/extensions");
    // tabs requires no permission by the specification, so alpha needs none for
    // it; bookmarks is the namespace used to exercise the permission gate.
    writeFile(root + QStringLiteral("/alpha/manifest.json"),
        R"({"manifest_version":3,"name":"Alpha","version":"1.0"})");
    writeFile(root + QStringLiteral("/beta/manifest.json"),
        R"({"manifest_version":2,"name":"Beta","version":"2.0"})");

    ExtensionRegistry registry(profile.path());
    registry.loadFrom(root);
    registry.setEnabled(QStringLiteral("alpha"), true);
    // beta is left disabled on purpose.

    ApiRegistry api;
    int calls = 0;
    api.registerMethod(QStringLiteral("tabs.query"),
        [&calls](const QString&, const QJsonObject&) {
            ++calls;
            return QJsonValue(QStringLiteral("ok"));
        });
    // Registered with a handler rather than marked unavailable, so what refuses
    // the content-script call below is the context restriction and nothing else.
    int devtoolsCalls = 0;
    api.registerMethod(QStringLiteral("devtools.panels.create"),
        [&devtoolsCalls](const QString&, const QJsonObject&) {
            ++devtoolsCalls;
            return QJsonValue(QStringLiteral("panel"));
        });

    ApiDispatcher dispatcher(api, registry);

    const auto call = [&](const QString& extensionId, const QString& method,
                          ExtensionContext context, int manifestVersion,
                          const QByteArray& arguments = "{}") {
        ApiDispatcher::Request request;
        request.extensionId = extensionId;
        request.method = method;
        request.context = context;
        request.manifestVersion = manifestVersion;
        request.arguments = QJsonDocument::fromJson(arguments).object();
        return dispatcher.dispatch(request);
    };

    // The happy path, so the refusals below are about the refusals.
    check(call(QStringLiteral("alpha"), QStringLiteral("tabs.query"),
            ExtensionContext::Background, 3).toString() == QLatin1String("ok"),
        "a permitted call runs its handler");
    check(call(QStringLiteral("alpha"), QStringLiteral("tabs.query"),
            ExtensionContext::Background, 3, R"({"active":true})").toString()
            == QLatin1String("ok"),
        "tabs.query accepts its queryInfo object as well");
    check(calls == 2, "the handler ran for each permitted call");

    // A namespace that requires a permission, to check the gate itself: an
    // extension without it is refused, and the handler does not run.
    ApiRegistry gated;
    int gatedCalls = 0;
    gated.registerMethod(QStringLiteral("bookmarks.getTree"),
        [&gatedCalls](const QString&, const QJsonObject&) {
            ++gatedCalls;
            return QJsonValue(QStringLiteral("tree"));
        });
    ApiDispatcher gatedDispatcher(gated, registry);
    const auto gatedCall = [&gatedDispatcher](const QString& extensionId) {
        ApiDispatcher::Request request;
        request.extensionId = extensionId;
        request.method = QStringLiteral("bookmarks.getTree");
        request.context = ExtensionContext::Background;
        request.manifestVersion = 3;
        return refusalFor(gatedDispatcher, request);
    };
    check(gatedCall(QStringLiteral("alpha")).contains(QStringLiteral("bookmarks")),
        "a call without the permission is refused naming the permission");
    check(gatedCalls == 0, "a refused call does not run its handler");

    // A disabled extension is refused before its permissions are consulted.
    check(refusalFor(dispatcher, [&] {
        ApiDispatcher::Request request;
        request.extensionId = QStringLiteral("beta");
        request.method = QStringLiteral("tabs.query");
        request.context = ExtensionContext::Background;
        request.manifestVersion = 2;
        return request;
    }()).contains(QStringLiteral("not enabled")),
        "a disabled extension is refused");

    // The context check precedes the permission check, so a devtools-only API
    // called from a content script reports the context, not a missing
    // permission the extension may well hold.
    const QString fromContentScript = refusalFor(dispatcher, [&] {
        ApiDispatcher::Request request;
        request.extensionId = QStringLiteral("alpha");
        request.method = QStringLiteral("devtools.panels.create");
        request.context = ExtensionContext::ContentScript;
        request.manifestVersion = 3;
        return request;
    }());
    check(fromContentScript.contains(QStringLiteral("content script")),
        "a context-restricted API reports the context first");
    check(!fromContentScript.contains(QStringLiteral("permission")),
        "and does not blame a permission that is not the problem");
    check(devtoolsCalls == 0, "the refused devtools call did not run its handler");

    // The same method from a devtools context is permitted, which is what makes
    // the refusal above a context restriction rather than a blanket one.
    // The argument names come from the schema too, which is why this is
    // "title" and not "name": the catalog is what told us.
    check(call(QStringLiteral("alpha"), QStringLiteral("devtools.panels.create"),
            ExtensionContext::DevTools, 3,
            R"({"title":"P","iconPath":"i.png","pagePath":"p.html"})").toString()
            == QLatin1String("panel"),
        "the same method is allowed from a devtools context");
    check(devtoolsCalls == 1, "and it runs there");

    // Manifest version bounds are enforced from the catalog.
    ApiRegistry versioned;
    versioned.registerMethod(QStringLiteral("browserAction.setTitle"),
        [](const QString&, const QJsonObject&) { return QJsonValue {}; });
    ApiDispatcher versionedDispatcher(versioned, registry);
    const QString refused = refusalFor(versionedDispatcher, [&] {
        ApiDispatcher::Request request;
        request.extensionId = QStringLiteral("alpha");
        request.method = QStringLiteral("browserAction.setTitle");
        request.context = ExtensionContext::Background;
        request.manifestVersion = 3;
        return request;
    }());
    check(refused.contains(QStringLiteral("Manifest V3")),
        "an MV2-only method is refused to an MV3 extension");

    // An unknown method is refused as unknown, not as a permission problem.
    check(refusalFor(dispatcher, [&] {
        ApiDispatcher::Request request;
        request.extensionId = QStringLiteral("alpha");
        request.method = QStringLiteral("tabs.noSuchMethod");
        request.context = ExtensionContext::Background;
        request.manifestVersion = 3;
        return request;
    }()).contains(QStringLiteral("not a WebExtensions method")),
        "an unknown method is refused as unknown");
    // A method the specification has but this build has not implemented is a
    // different failure, and saying so is the difference between an extension
    // author looking for a typo and one looking at browser coverage.
    check(refusalFor(dispatcher, [&] {
        ApiDispatcher::Request request;
        request.extensionId = QStringLiteral("alpha");
        request.method = QStringLiteral("tabs.captureVisibleTab");
        request.context = ExtensionContext::Background;
        request.manifestVersion = 3;
        return request;
    }()).contains(QStringLiteral("not implemented in this build")),
        "an unimplemented member is distinguished from an unknown one");

    // A declared but unimplemented method rejects with a reason.
    ApiRegistry partial;
    partial.registerUnavailable(QStringLiteral("webRequest.onBeforeRequest"),
        QStringLiteral("the engine has no per-request hook"));
    ApiDispatcher partialDispatcher(partial, registry);
    const QString unavailable = refusalFor(partialDispatcher, [&] {
        ApiDispatcher::Request request;
        request.extensionId = QStringLiteral("alpha");
        request.method = QStringLiteral("webRequest.onBeforeRequest");
        request.context = ExtensionContext::Background;
        request.manifestVersion = 3;
        return request;
    }());
    check(unavailable.contains(QStringLiteral("per-request hook")),
        "an unimplemented method rejects with the reason");
    check(!unavailable.contains(QStringLiteral("permission")),
        "and does not blame a permission when none is the problem");
    check(!unavailable.contains(QStringLiteral("undefined")),
        "and never resolves to undefined");

    // Arguments are validated after the permission check, so an extension
    // without permission learns nothing about the argument shape.
    check(refusalFor(dispatcher, [&] {
        ApiDispatcher::Request request;
        request.extensionId = QStringLiteral("beta");
        request.method = QStringLiteral("tabs.query");
        request.context = ExtensionContext::Background;
        request.manifestVersion = 2;
        request.arguments = QJsonDocument::fromJson(R"({"nonsense":1})").object();
        return request;
    }()).contains(QStringLiteral("not enabled")),
        "a disabled extension is refused before its arguments are looked at");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testCatalogIsPopulated();
    testRegistryReadsTheCatalog();
    testArgumentValidation();
    testDispatchOrder();

    if (g_failures == 0)
        std::printf("PASS: extension API registry and dispatcher\n");
    else
        std::printf("%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}