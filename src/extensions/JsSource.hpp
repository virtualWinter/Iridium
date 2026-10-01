#pragma once

#include <QString>

namespace iridium::extensions {

// Quotes `value` as a JS string literal, including the surrounding quotes.
// Extension ids come from manifests and are therefore untrusted: an id
// containing a quote must not be able to break out of generated script source.
// Shared because the registry, the API layer and the injector all emit JS.
inline QString jsStringLiteral(const QString& value)
{
    QString escaped;
    escaped.reserve(value.size() + 2);
    escaped += QLatin1Char('"');
    for (const QChar character : value) {
        switch (character.unicode()) {
        case '"': escaped += QLatin1String("\\\""); break;
        case '\\': escaped += QLatin1String("\\\\"); break;
        case '\n': escaped += QLatin1String("\\n"); break;
        case '\r': escaped += QLatin1String("\\r"); break;
        case '\t': escaped += QLatin1String("\\t"); break;
        default: escaped += character; break;
        }
    }
    escaped += QLatin1Char('"');
    return escaped;
}

} // namespace iridium::extensions