#include "ui/settings/AppearancePage.hpp"

#include "ui/settings/SettingsStore.hpp"

#include <QComboBox>
#include <QFormLayout>
#include <QFont>
#include <QLabel>
#include <QVBoxLayout>

namespace iridium {

namespace {

QLatin1String systemScheme() { return QLatin1String("system"); }
QLatin1String lightScheme() { return QLatin1String("light"); }
QLatin1String darkScheme() { return QLatin1String("dark"); }

} // namespace

AppearancePage::AppearancePage(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("appearancePage"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(12);

    auto* heading = new QLabel(tr("Appearance"), this);
    heading->setObjectName(QStringLiteral("settingsHeading"));
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

    // One combo, not a combo plus a checkbox. The previous layout had both, and
    // the checkbox was permanently checked and disabled: it looked like a
    // preference the user could change and could not be. "Follow system" in the
    // combo says the same thing and can be selected.
    m_scheme = new QComboBox(this);
    m_scheme->setObjectName(QStringLiteral("settingsCombo"));
    m_scheme->setToolTip(tr("Sets the colour scheme requested by websites"));
    m_scheme->addItem(tr("Follow system"), systemScheme());
    m_scheme->addItem(tr("Light"), lightScheme());
    m_scheme->addItem(tr("Dark"), darkScheme());
    form->addRow(tr("Colour scheme"), m_scheme);

    root->addLayout(form);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("settingsStatus"));
    m_status->setWordWrap(true);
    m_status->setVisible(false);
    root->addWidget(m_status);

    root->addStretch(1);

    showValues();

    connect(m_scheme, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_showing)
            return;
        // setColorScheme owns both the stored value and the in-memory override,
        // and emits forcedColorSchemeChanged, which is what applies the change to
        // the views that are already open. So there is nothing to apply here
        // separately: doing so would set the same value twice.
        SettingsStore::instance().setColorScheme(m_scheme->currentData().toString());
    });
}

void AppearancePage::showValues()
{
    m_showing = true;
    const int index = m_scheme->findData(SettingsStore::instance().colorScheme());
    m_scheme->setCurrentIndex(index >= 0 ? index : 0);
    m_showing = false;
}

void AppearancePage::refresh()
{
    showValues();
}

} // namespace iridium
