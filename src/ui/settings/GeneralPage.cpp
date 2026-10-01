#include "ui/settings/GeneralPage.hpp"

#include "ui/settings/SettingsStore.hpp"

#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QStandardPaths>
#include <QToolButton>
#include <QVBoxLayout>

namespace iridium {

GeneralPage::GeneralPage(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("generalPage"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(12);

    auto* heading = new QLabel(tr("General"), this);
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

    m_homePage = new QLineEdit(this);
    m_homePage->setObjectName(QStringLiteral("settingsSearch"));
    m_homePage->setPlaceholderText(tr("Leave empty for the default start page"));
    form->addRow(tr("Homepage"), m_homePage);

    m_searchTemplate = new QLineEdit(this);
    m_searchTemplate->setObjectName(QStringLiteral("settingsSearch"));
    m_searchTemplate->setPlaceholderText(
        tr("Use %1 where the query should go, e.g. https://example.com/?q=%1"));
    form->addRow(tr("Search address"), m_searchTemplate);

    // Downloads row: a read-only field plus a browse button.
    auto* downloadRow = new QWidget(this);
    auto* downloadLayout = new QHBoxLayout(downloadRow);
    downloadLayout->setContentsMargins(0, 0, 0, 0);
    downloadLayout->setSpacing(8);
    m_downloadDirectory = new QLineEdit(downloadRow);
    m_downloadDirectory->setObjectName(QStringLiteral("settingsSearch"));
    m_downloadDirectory->setPlaceholderText(
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    m_downloadDirectory->setToolTip(
        tr("Leave empty to use the Downloads folder"));
    downloadLayout->addWidget(m_downloadDirectory, 1);
    m_browseDownloads = new QToolButton(downloadRow);
    m_browseDownloads->setObjectName(QStringLiteral("settingsButton"));
    m_browseDownloads->setText(tr("Browse"));
    m_browseDownloads->setCursor(Qt::PointingHandCursor);
    downloadLayout->addWidget(m_browseDownloads);
    form->addRow(tr("Save files to"), downloadRow);

    m_confirmClose = new QCheckBox(tr("Confirm before closing several tabs"), this);
    m_confirmClose->setObjectName(QStringLiteral("settingsCheck"));
    form->addRow(QString(), m_confirmClose);

    root->addLayout(form);

    auto* note = new QLabel(tr("An address that is not a URL is searched for "
                               "using the template above."), this);
    note->setObjectName(QStringLiteral("settingsFootnote"));
    note->setWordWrap(true);
    root->addWidget(note);

    root->addStretch(1);

    load();
    SettingsStore& store = SettingsStore::instance();

    // Writes are immediate, so there is no Apply to forget.
    connect(m_homePage, &QLineEdit::editingFinished, this, [this, &store] {
        store.setHomePage(m_homePage->text());
    });
    connect(m_searchTemplate, &QLineEdit::editingFinished, this, [this, &store] {
        store.setSearchTemplate(m_searchTemplate->text());
    });
    connect(m_downloadDirectory, &QLineEdit::editingFinished, this, [this, &store] {
        store.setDownloadDirectory(m_downloadDirectory->text());
    });
    connect(m_confirmClose, &QCheckBox::toggled, this, [this, &store] {
        store.setConfirmBeforeClosingTabs(m_confirmClose->isChecked());
    });
    connect(m_browseDownloads, &QToolButton::clicked, this, [this, &store] {
        const QString start = m_downloadDirectory->text().isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
            : m_downloadDirectory->text();
        const QString chosen = QFileDialog::getExistingDirectory(
            this, tr("Choose a download folder"), start);
        if (chosen.isEmpty())
            return;
        m_downloadDirectory->setText(chosen);
        store.setDownloadDirectory(chosen);
    });
}

void GeneralPage::load()
{
    const SettingsStore& store = SettingsStore::instance();
    m_homePage->setText(store.homePage());
    m_searchTemplate->setText(store.searchTemplate());
    m_downloadDirectory->setText(store.downloadDirectory());
    m_confirmClose->setChecked(store.confirmBeforeClosingTabs());
}

} // namespace iridium