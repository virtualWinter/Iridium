#pragma once

#include <QHash>
#include <QList>
#include <QPixmap>
#include <QSize>
#include <QString>

// Window decoration data read from the running KDE setup, so a client-side
// decorated (frameless) window can match the system's real title bar.
//
// Sources, in order:
//   * ~/.config/kwinrc  [org.kde.kdecoration2]  -> theme + button layout
//   * <theme>rc         [Layout]                -> button size, spacing, margin
//   * <theme>/*.svg     -> Aurorae FrameSvg button elements
//
// Aurorae buttons are FrameSvg files. Each state is named by a prefix, such as
// "active-center" or "hover-inactive-center". This follows KWin's fallback
// order rather than treating the entire SVG as a horizontal image strip.
class DecorationTheme
{
public:
    enum class Side { Left, Right };
    enum class Button { Minimize, Maximize, Close };
    enum class State { Active, Hover, Pressed, Inactive, Deactivated };

    static const DecorationTheme &system();

    QList<Button> buttons(Side side) const;

    QSize buttonSize(Button button) const;
    int buttonHeight() const;
    int buttonSpacing() const;
    int buttonMarginTop(bool maximized) const;

    // Returns the FrameSvg center element selected with KWin's state fallback
    // order, or a null pixmap when the system theme provides no artwork.
    QPixmap pixmap(Button button, bool maximized, bool active, State state,
                   const QSize &logicalSize, qreal devicePixelRatio) const;

private:
    DecorationTheme();

    static QString assetName(Button button, bool maximized);
    QString assetPath(const QString &asset) const;
    static QString elementPrefix(const QString &path, bool active, State state);

    QString m_themeDir;
    QString m_leftButtons;
    QString m_rightButtons;
    int m_buttonWidth = 18;
    int m_minimizeButtonWidth = 18;
    int m_maximizeButtonWidth = 18;
    int m_closeButtonWidth = 18;
    int m_buttonHeight = 18;
    int m_buttonSpacing = 4;
    int m_buttonMarginTop = 0;
    int m_buttonMarginTopMaximized = 0;

    mutable QHash<QString, QPixmap> m_cache;
};
