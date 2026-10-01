#include "extensions/Manifest.hpp"

#include "extensions/MatchPattern.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>

namespace iridium::extensions {

namespace {

QStringList stringList(const QJsonValue& value)
{
    QStringList result;
    if (value.isArray()) {
        for (const QJsonValue& entry : value.toArray()) {
            if (entry.isString())
                result.append(entry.toString());
        }
    } else if (value.isString()) {
        result.append(value.toString());
    }
    return result;
}

// A permission is a host pattern when it parses as one; otherwise it is an API
// name. MV3 splits these into two keys, MV2 mixes them in "permissions".
bool looksLikeMatchPattern(const QString& text)
{
    if (text == QLatin1String("<all_urls>"))
        return true;
    return text.contains(QLatin1String("://"));
}

} // namespace

std::optional<Manifest> Manifest::fromJson(const QByteArray& json, QString* error)
{
    QJsonParseError parseError {};
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error)
            *error = QStringLiteral("manifest.json is not valid JSON: %1")
                         .arg(parseError.errorString());
        return std::nullopt;
    }
    if (!document.isObject()) {
        if (error)
            *error = QStringLiteral("manifest.json must contain a JSON object");
        return std::nullopt;
    }
    return parse(document.object(), error);
}

std::optional<Manifest> Manifest::load(const QString& directory, QString* error)
{
    const QString path = directory + QStringLiteral("/manifest.json");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("cannot read %1").arg(path);
        return std::nullopt;
    }
    return fromJson(file.readAll(), error);
}

std::optional<Manifest> Manifest::parse(const QJsonObject& object, QString* error)
{
    const auto fail = [error](const QString& reason) {
        if (error)
            *error = reason;
        return std::nullopt;
    };

    Manifest manifest;
    manifest.m_raw = object;

    const QJsonValue versionValue = object.value(QStringLiteral("manifest_version"));
    if (!versionValue.isDouble()) {
        return fail(QStringLiteral("manifest_version is required and must be a number"));
    }
    manifest.m_manifestVersion = versionValue.toInt();
    if (manifest.m_manifestVersion != 2 && manifest.m_manifestVersion != 3) {
        return fail(QStringLiteral("unsupported manifest_version %1; expected 2 or 3")
                        .arg(manifest.m_manifestVersion));
    }

    const QString name = object.value(QStringLiteral("name")).toString().trimmed();
    if (name.isEmpty())
        return fail(QStringLiteral("name is required"));
    manifest.m_name = name;

    const QString version = object.value(QStringLiteral("version")).toString().trimmed();
    if (version.isEmpty())
        return fail(QStringLiteral("version is required"));
    manifest.m_version = version;

    manifest.m_description = object.value(QStringLiteral("description")).toString();

    // Split the declared permissions into API names and host patterns. MV3
    // keeps hosts in host_permissions; MV2 allows them inline in permissions.
    QStringList declared = stringList(object.value(QStringLiteral("permissions")));
    for (const QString& entry : stringList(object.value(QStringLiteral("host_permissions"))))
        declared.append(entry);

    for (const QString& entry : declared) {
        if (looksLikeMatchPattern(entry))
            manifest.m_hostPermissions.append(entry);
        else
            manifest.m_permissions.append(entry);
    }

    // Optional permissions follow the same split, but stay ungranted until the
    // user enables them in settings.
    for (const QString& entry : stringList(object.value(QStringLiteral("optional_permissions")))) {
        if (looksLikeMatchPattern(entry))
            manifest.m_optionalHostPermissions.append(entry);
        else
            manifest.m_optionalPermissions.append(entry);
    }
    for (const QString& entry :
        stringList(object.value(QStringLiteral("optional_host_permissions")))) {
        manifest.m_optionalHostPermissions.append(entry);
    }

    // A permission declared as both required and optional is ambiguous; treating
    // it as required matches how the major browsers resolve the conflict.
    // Element-wise, not removeAll(QList): the latter erases nothing useful here and
// the per-item form states the intent directly.
    QStringList optionalPermissions;
    for (const QString& entry : manifest.m_optionalPermissions) {
        if (!manifest.m_permissions.contains(entry))
            optionalPermissions.append(entry);
    }
    manifest.m_optionalPermissions = optionalPermissions;

    QStringList optionalHosts;
    for (const QString& entry : manifest.m_optionalHostPermissions) {
        if (!manifest.m_hostPermissions.contains(entry))
            optionalHosts.append(entry);
    }
    manifest.m_optionalHostPermissions = optionalHosts;

    // Validate host patterns up front so a broken pattern is reported at load
    // time rather than when a page fails to match.
    QStringList allHostPatterns = manifest.m_hostPermissions;
    allHostPatterns.append(manifest.m_optionalHostPermissions);
    for (const QString& entry : allHostPatterns) {
        MatchPattern pattern;
        QString patternError;
        if (!MatchPattern::parse(entry, &pattern, &patternError)) {
            return fail(QStringLiteral("invalid host permission: %1").arg(patternError));
        }
    }

    const QJsonArray scripts = object.value(QStringLiteral("content_scripts")).toArray();
    for (const QJsonValue& entry : scripts) {
        if (!entry.isObject())
            continue;
        const QJsonObject scriptObject = entry.toObject();
        ContentScript script;
        script.matches = stringList(scriptObject.value(QStringLiteral("matches")));
        script.excludeMatches = stringList(scriptObject.value(QStringLiteral("exclude_matches")));
        script.js = stringList(scriptObject.value(QStringLiteral("js")));
        script.css = stringList(scriptObject.value(QStringLiteral("css")));
        script.allFrames = scriptObject.value(QStringLiteral("all_frames")).toBool(false);

        const QString runAt = scriptObject.value(QStringLiteral("run_at")).toString();
        if (!runAt.isEmpty()) {
            if (runAt != QLatin1String("document_start")
                && runAt != QLatin1String("document_end")
                && runAt != QLatin1String("document_idle")) {
                return fail(QStringLiteral("unknown run_at '%1'").arg(runAt));
            }
            script.runAt = runAt;
        }

        if (script.matches.isEmpty() && script.js.isEmpty() && script.css.isEmpty())
            continue;
        manifest.m_contentScripts.push_back(std::move(script));
    }

    const QJsonObject background = object.value(QStringLiteral("background")).toObject();
    manifest.m_background.scripts = stringList(background.value(QStringLiteral("scripts")));
    manifest.m_background.serviceWorker =
        background.value(QStringLiteral("service_worker")).toString();
    manifest.m_background.persistent = background.value(QStringLiteral("persistent")).toBool(false);

    if (object.contains(QStringLiteral("action"))) {
        const QJsonObject action = object.value(QStringLiteral("action")).toObject();
        manifest.m_action.present = true;
        manifest.m_action.defaultTitle =
            action.value(QStringLiteral("default_title")).toString();
        manifest.m_action.defaultPopup =
            action.value(QStringLiteral("default_popup")).toString();
        manifest.m_action.icons = action.value(QStringLiteral("icons")).toObject();
    }

    manifest.m_optionsPage = object.value(QStringLiteral("options_ui")).toObject()
        .value(QStringLiteral("page")).toString();
    if (manifest.m_optionsPage.isEmpty())
        manifest.m_optionsPage = object.value(QStringLiteral("options_page")).toString();

    manifest.m_dependencies = stringList(object.value(QStringLiteral("dependencies")));

    return manifest;
}

} // namespace iridium::extensions
