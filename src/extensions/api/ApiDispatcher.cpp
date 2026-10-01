#include "extensions/api/ApiDispatcher.hpp"

#include "extensions/ExtensionRegistry.hpp"

#include <QJsonArray>
#include <QJsonValue>

#include <limits>

namespace iridium::extensions {

namespace {

QJsonValue refuse(const QString& message)
{
    QJsonObject error;
    error.insert(QStringLiteral("message"), message);
    return error;
}

// The specification's integer bounds, used when a parameter declares none.
// A schema that states no bound is stored with these sentinels, so a value
// outside them is not something the specification can produce.
constexpr int kMinInt = std::numeric_limits<int>::min();
constexpr int kMaxInt = std::numeric_limits<int>::max();

bool isIntegral(const QJsonValue& value)
{
    // QJsonValue::isDouble is also true for whole numbers that arrived as
    // integers, which is exactly what a caller sending tabId means, so the
    // check is "has no fractional part" rather than "was stored as an integer".
    if (!value.isDouble())
        return false;
    const double number = value.toDouble();
    return number == static_cast<double>(static_cast<qlonglong>(number));
}

} // namespace

ApiDispatcher::ApiDispatcher(ApiRegistry& registry, ExtensionRegistry& extensions)
    : m_registry(registry)
    , m_extensions(extensions)
{
}

QString ApiDispatcher::validateArguments(const ApiMethod& method,
    const QJsonObject& arguments)
{
    for (int i = 0; i < method.parameterCount(); ++i) {
        const generated::ParameterSpec& spec = method.parameters()[i];
        const QString name = QString::fromUtf8(spec.name ? spec.name : "");
        const bool present = !name.isEmpty() && arguments.contains(name);

        if (!present) {
            if (spec.optional || name.isEmpty())
                continue;
            // A missing required object argument is treated as an empty object
            // rather than refused. The Gecko schemas mark queryInfo-style
            // parameters as required, but every browser accepts tabs.query()
            // with no argument at all, and refusing it would break extensions
            // that are not doing anything wrong. A missing required scalar is
            // still refused: there is no sensible reading of tabs.get() with no
            // tab id.
            const QString type = QString::fromUtf8(spec.type ? spec.type : "");
            if (type == QLatin1String("object"))
                continue;
            return QStringLiteral("missing required argument \"%1\"").arg(name);
        }

        const QJsonValue value = arguments.value(name);
        if (value.isUndefined())
            continue;

        const QString type = QString::fromUtf8(spec.type ? spec.type : "");
        if (type == QLatin1String("integer")) {
            if (!isIntegral(value)) {
                return QStringLiteral("argument \"%1\" must be an integer").arg(name);
            }
            const qlonglong number = value.toInteger();
            if (number < kMinInt || number > kMaxInt)
                return QStringLiteral("argument \"%1\" is out of range").arg(name);
            if (number < spec.minimum || number > spec.maximum) {
                return QStringLiteral("argument \"%1\" must be between %2 and %3")
                    .arg(name).arg(spec.minimum).arg(spec.maximum);
            }
        } else if (type == QLatin1String("boolean")) {
            if (!value.isBool())
                return QStringLiteral("argument \"%1\" must be a boolean").arg(name);
        } else if (type == QLatin1String("string")) {
            if (!value.isString())
                return QStringLiteral("argument \"%1\" must be a string").arg(name);
        } else if (type == QLatin1String("array")) {
            if (!value.isArray())
                return QStringLiteral("argument \"%1\" must be an array").arg(name);
        } else if (type == QLatin1String("object")) {
            if (!value.isObject())
                return QStringLiteral("argument \"%1\" must be an object").arg(name);
        }

        if (spec.enumValues) {
            const QString wanted = value.toString();
            bool allowed = false;
            for (const char* const* option = spec.enumValues; *option; ++option) {
                if (wanted == QString::fromUtf8(*option)) {
                    allowed = true;
                    break;
                }
            }
            if (!allowed) {
                return QStringLiteral("argument \"%1\" does not accept \"%2\"")
                    .arg(name, wanted);
            }
        }
    }
    return {};
}

QString ApiDispatcher::refusalReason(const Request& request) const
{
    const ApiMethod* method = m_registry.find(request.method);
    if (!method) {
        // Two different failures that must not be reported as one: a method the
        // specification does not have, and a method it has that this build has
        // not implemented. Telling an extension the second is the first would
        // send it looking for a typo instead of at the browser's coverage.
        QString space;
        QString member;
        const int dot = request.method.lastIndexOf(QLatin1Char('.'));
        if (dot > 0) {
            space = request.method.left(dot);
            member = request.method.mid(dot + 1);
            const std::string spaceKey = space.toStdString();
            if (generated::findNamespace(std::string_view(spaceKey))
                && generated::findMember(std::string_view(spaceKey),
                                          std::string_view(member.toStdString()))) {
                return QStringLiteral("browser.%1 is part of the WebExtensions "
                                      "specification but is not implemented in this build")
                    .arg(request.method);
            }
        }
        return QStringLiteral("browser.%1 is not a WebExtensions method")
            .arg(request.method);
    }

    // Availability is answered before permissions. A method this build cannot
    // provide will refuse however the extension is permissioned, so saying
    // "permission" would send the caller to the wrong place.
    if (method->isUnavailable()) {
        return QStringLiteral("browser.%1 is not available in this build: %2")
            .arg(request.method, method->unavailableReason());
    }

    // The context check comes before the permission check so that a call an
    // extension had no right to make anyway is refused for the reason that is
    // actually true of it.
    const generated::ContextFlag flag = contextFlag(request.context);
    if (!(method->contexts() & flag)) {
        return QStringLiteral("browser.%1 is not available in a %2")
            .arg(request.method, QString::fromUtf8(contextName(request.context)));
    }

    if (!m_extensions.isEnabled(request.extensionId))
        return QStringLiteral("extension %1 is not enabled").arg(request.extensionId);

    if (request.manifestVersion < method->minManifestVersion()
        || (method->maxManifestVersion() != 0
            && request.manifestVersion > method->maxManifestVersion())) {
        return QStringLiteral("browser.%1 is not available in Manifest V%2")
            .arg(request.method).arg(request.manifestVersion);
    }

    // A method may need more than one permission. All of them are checked, and
    // the message names the one that is actually missing rather than the whole
    // list, so the caller is told what to ask for.
    const QStringList required = method->requiredPermission().split(QLatin1Char('+'),
        Qt::SkipEmptyParts);
    QStringList missing;
    for (const QString& permission : required) {
        if (!m_extensions.hasPermission(request.extensionId, permission))
            missing << permission;
    }
    if (!missing.isEmpty()) {
        return QStringLiteral("browser.%1 requires the \"%2\" permission, which is "
                              "not granted for %3")
            .arg(request.method, missing.join(QStringLiteral("\", \"")), request.extensionId);
    }
    return {};
}

QJsonValue ApiDispatcher::dispatch(const Request& request)
{
    const QString reason = refusalReason(request);
    if (!reason.isEmpty())
        return refuse(reason);

    const ApiMethod* method = m_registry.find(request.method);
    if (!method->hasHandler()) {
        return refuse(QStringLiteral("browser.%1 is declared but has no implementation "
                                     "in this build")
            .arg(request.method));
    }

    const QString invalid = validateArguments(*method, request.arguments);
    if (!invalid.isEmpty()) {
        return refuse(QStringLiteral("browser.%1: %2").arg(request.method, invalid));
    }

    return method->handler()(request.extensionId, request.arguments);
}

} // namespace iridium::extensions