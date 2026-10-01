#include "decorationtheme.h"

#include <QDir>
#include <QFile>
#include <QMap>
#include <QSettings>
#include <QStandardPaths>
#include <KSvg/FrameSvg>

namespace {

QString configFile(const QString &name)
{
    const QString user = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                         + QLatin1Char('/') + name;
    if (QFile::exists(user))
        return user;

    return QStringLiteral("/etc/xdg/") + name;
}

QMap<QString, QString> readGroup(const QString &path, const QString &group)
{
    QMap<QString, QString> values;
    QSettings settings(path, QSettings::IniFormat);
    settings.beginGroup(group);
    for (const QString &key : settings.childKeys())
        values.insert(key, settings.value(key).toString());
    return values;
}

} // namespace

const DecorationTheme &DecorationTheme::system()
{
    static const DecorationTheme instance;
    return instance;
}

DecorationTheme::DecorationTheme()
{
    const QMap<QString, QString> decoration =
        readGroup(configFile(QStringLiteral("kwinrc")), QStringLiteral("org.kde.kdecoration2"));

    // KWin's own default layout: a window menu on the left, and
    // minimize/maximize/close on the right.
    m_leftButtons = decoration.value(QStringLiteral("ButtonsOnLeft"), QStringLiteral("M"));
    m_rightButtons = decoration.value(QStringLiteral("ButtonsOnRight"), QStringLiteral("IAX"));

    const QString theme = decoration.value(QStringLiteral("theme"));
    const QString auroraePrefix = QStringLiteral("__aurorae__svg__");
    if (!theme.startsWith(auroraePrefix))
        return;

    const QString name = theme.mid(auroraePrefix.size());
    const QString rcName = name + QStringLiteral("rc");

    const QStringList dataLocations =
        QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    for (const QString &base : dataLocations) {
        const QString dir = base + QStringLiteral("/aurorae/themes/") + name;
        if (QFile::exists(dir + QLatin1Char('/') + rcName)) {
            m_themeDir = dir;
            break;
        }
    }
    if (m_themeDir.isEmpty())
        return;

    const QMap<QString, QString> layout =
        readGroup(m_themeDir + QLatin1Char('/') + rcName, QStringLiteral("Layout"));
    m_buttonWidth = layout.value(QStringLiteral("ButtonWidth"), QStringLiteral("20")).toInt();
    m_minimizeButtonWidth = layout.value(QStringLiteral("ButtonWidthMinimize"),
                                         QString::number(m_buttonWidth)).toInt();
    m_maximizeButtonWidth = layout.value(QStringLiteral("ButtonWidthMaximizeRestore"),
                                         QString::number(m_buttonWidth)).toInt();
    m_closeButtonWidth = layout.value(QStringLiteral("ButtonWidthClose"),
                                      QString::number(m_buttonWidth)).toInt();
    m_buttonHeight = layout.value(QStringLiteral("ButtonHeight"), QStringLiteral("20")).toInt();
    m_buttonSpacing = layout.value(QStringLiteral("ButtonSpacing"), QStringLiteral("5")).toInt();
    m_buttonMarginTop = layout.value(QStringLiteral("ButtonMarginTop"), QStringLiteral("0")).toInt();
    m_buttonMarginTopMaximized = layout.value(QStringLiteral("ButtonMarginTopMaximized"),
                                               QStringLiteral("0")).toInt();
}

QSize DecorationTheme::buttonSize(Button button) const
{
    switch (button) {
    case Button::Minimize:
        return {m_minimizeButtonWidth, m_buttonHeight};
    case Button::Maximize:
        return {m_maximizeButtonWidth, m_buttonHeight};
    case Button::Close:
        return {m_closeButtonWidth, m_buttonHeight};
    }
    return {m_buttonWidth, m_buttonHeight};
}

int DecorationTheme::buttonHeight() const
{
    return m_buttonHeight;
}

int DecorationTheme::buttonSpacing() const
{
    return m_buttonSpacing;
}

int DecorationTheme::buttonMarginTop(bool maximized) const
{
    return maximized ? m_buttonMarginTopMaximized : m_buttonMarginTop;
}

QList<DecorationTheme::Button> DecorationTheme::buttons(Side side) const
{
    const QString spec = (side == Side::Left) ? m_leftButtons : m_rightButtons;

    QList<Button> result;
    for (const QChar letter : spec) {
        switch (letter.toUpper().unicode()) {
        case 'I':
            result.append(Button::Minimize);
            break;
        case 'A':
            result.append(Button::Maximize);
            break;
        case 'X':
            result.append(Button::Close);
            break;
        default:
            // Menu, help, shade, ... are not part of this browser's chrome.
            break;
        }
    }
    return result;
}

QString DecorationTheme::assetName(Button button, bool maximized)
{
    switch (button) {
    case Button::Close:
        return QStringLiteral("close");
    case Button::Minimize:
        return QStringLiteral("minimize");
    case Button::Maximize:
        return maximized ? QStringLiteral("restore") : QStringLiteral("maximize");
    }
    return {};
}

QString DecorationTheme::assetPath(const QString &asset) const
{
    if (m_themeDir.isEmpty())
        return {};

    const QString svg = m_themeDir + QLatin1Char('/') + asset + QStringLiteral(".svg");
    if (QFile::exists(svg))
        return svg;

    const QString svgz = svg + QLatin1Char('z');
    return QFile::exists(svgz) ? svgz : QString();
}

QString DecorationTheme::elementPrefix(const QString &path, bool active, State state)
{
    KSvg::FrameSvg frame;
    frame.setImagePath(path);
    if (!frame.hasElementPrefix(QStringLiteral("active")))
        return {};

    QStringList candidates;
    if (active) {
        switch (state) {
        case State::Pressed:
            candidates << QStringLiteral("pressed");
            break;
        case State::Hover:
            candidates << QStringLiteral("hover");
            break;
        case State::Deactivated:
            candidates << QStringLiteral("deactivated");
            break;
        default:
            break;
        }
        candidates << QStringLiteral("active");
    } else {
        switch (state) {
        case State::Pressed:
            candidates << QStringLiteral("pressed-inactive") << QStringLiteral("pressed");
            break;
        case State::Hover:
            candidates << QStringLiteral("hover-inactive") << QStringLiteral("hover");
            break;
        case State::Deactivated:
            candidates << QStringLiteral("deactivated-inactive")
                       << QStringLiteral("deactivated");
            break;
        default:
            break;
        }
        candidates << QStringLiteral("inactive") << QStringLiteral("active");
    }

    for (const QString &candidate : candidates) {
        if (frame.hasElementPrefix(candidate))
            return candidate;
    }
    return {};
}

QPixmap DecorationTheme::pixmap(Button button, bool maximized, bool active, State state,
                                const QSize &logicalSize, qreal devicePixelRatio) const
{
    const QString path = assetPath(assetName(button, maximized));
    if (path.isEmpty())
        return {};

    const QString prefix = elementPrefix(path, active, state);
    if (prefix.isEmpty())
        return {};

    const QSize pixelSize(qRound(logicalSize.width() * devicePixelRatio),
                          qRound(logicalSize.height() * devicePixelRatio));
    if (pixelSize.isEmpty())
        return {};

    const QString key = path + QLatin1Char('#') + prefix + QLatin1Char('#')
                        + QString::number(pixelSize.width()) + QLatin1Char('x')
                        + QString::number(pixelSize.height()) + QLatin1Char('@')
                        + QString::number(devicePixelRatio, 'g', 8);

    const auto cached = m_cache.constFind(key);
    if (cached != m_cache.constEnd())
        return cached.value();

    QPixmap result;
    KSvg::FrameSvg frame;
    frame.setImagePath(path);
    frame.setElementPrefix(prefix);
    frame.setEnabledBorders(KSvg::FrameSvg::NoBorder);
    frame.setDevicePixelRatio(devicePixelRatio);
    frame.resizeFrame(logicalSize);
    result = frame.framePixmap();

    m_cache.insert(key, result);
    return result;
}
