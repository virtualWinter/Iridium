#include "ui/settings/AppearancePage.hpp"

#include "ui/settings/SettingsStore.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace iridium {

namespace {
QLatin1String systemScheme() { return QLatin1String("system"); }
QLatin1String lightScheme() { return QLatin1String("light"); }
QLatin1String darkScheme() { return QLatin1String("dark"); }
}

AppearancePage::AppearancePage(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("appearancePage"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(12);

    auto* heading = new QLabel(tr("Appearance"), this);
    QFont headingFont = heading->font();
    headingFont.setPointSizeF(headingFont.pointSizeF() * 1.6);
    headingFont.setBold(true);
    heading->setFont(headingFont);
    root->addWidget(heading);

    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(10);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_scheme = new QComboBox(this);
    m_scheme->setObjectName(QStringLiteral("settingsCombo"));
    m_scheme->addItem(tr("Follow system"), systemScheme());
    m_scheme->addItem(tr("Light"), lightScheme());
    m_scheme->addItem(tr("Dark"), darkScheme());
    form->addRow(tr("Colour scheme"), m_scheme);

    m_followSystem = new QCheckBox(tr("Use the system setting for window colours"), this);
    m_followSystem->setObjectName(QStringLiteral("settingsCheck"));
    form->addRow(QString(), m_followSystem);

    root->addLayout(form);

    m_note = new QLabel(this);
    m_note->setObjectName(QStringLiteral("settingsFootnote"));
    m_note->setWordWrap(true);
    m_note->setText(tr(
        "Pages are told the same scheme through prefers-color-scheme. Choosing "
        "Light or Dark overrides the system hint for every tab."));
    root->addWidget(m_note);

    root->addStretch(1);

    load();

    connect(m_scheme, &QComboBox::currentIndexChanged, this, [this] {
        save();
        apply();
    });
    // The checkbox is always on: the scheme combo above is how the system is
    // followed. It is shown for clarity and disabled, rather than hidden, so the
    // dependency is visible instead of surprising.
    m_followSystem->setChecked(true);
    m_followSystem->setEnabled(false);
}

void AppearancePage::load()
{
    const int index = m_scheme->findData(SettingsStore::instance().colorScheme());
    m_scheme->setCurrentIndex(index >= 0 ? index : 0);
}

void AppearancePage::save()
{
    SettingsStore::instance().setColorScheme(m_scheme->currentData().toString());
}

void AppearancePage::apply()
{
    // An explicit choice overrides the system hint; "system" clears the override
    // so the engine resumes following it.
    const QVariant chosen = m_scheme->currentData();
    if (chosen == QVariant(darkScheme()))
        SettingsStore::instance().setForcedColorScheme(true);
    else if (chosen == QVariant(lightScheme()))
        SettingsStore::instance().setForcedColorScheme(false);
    else
        SettingsStore::instance().setForcedColorScheme(std::nullopt);
}

} // namespace iridium