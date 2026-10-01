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
#include <QPushButton>
#include <QStandardPaths>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

namespace iridium {

namespace {

// The heading every pane uses. One helper so the panes cannot drift apart in how
// their titles look, which is exactly the sort of difference that makes a set of
// pages look assembled rather than designed.
QLabel* makeHeading(const QString& text, QWidget* parent)
{
    auto* heading = new QLabel(text, parent);
    heading->setObjectName(QStringLiteral("settingsHeading"));
    QFont font = heading->font();
    font.setPointSizeF(font.pointSizeF() * 1.6);
    font.setBold(true);
    heading->setFont(font);
    return heading;
}

} // namespace

GeneralPage::GeneralPage(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("generalPage"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(12);

    root->addWidget(makeHeading(tr("General"), this));

    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(10);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    // A field plus a reset control. The reset appears only when the value
    // differs from the default, so a pane that is already on its defaults shows
    // no button that would do nothing.
    const auto addReset = [this] {
        auto* button = new QToolButton(this);
        button->setObjectName(QStringLiteral("settingsButton"));
        button->setText(tr("Reset"));
        button->setToolTip(tr("Use the default value"));
        button->setCursor(Qt::PointingHandCursor);
        button->setVisible(false);
        return button;
    };

    auto* homeRow = new QWidget(this);
    auto* homeLayout = new QHBoxLayout(homeRow);
    homeLayout->setContentsMargins(0, 0, 0, 0);
    homeLayout->setSpacing(8);
    m_homePage = new QLineEdit(homeRow);
    m_homePage->setObjectName(QStringLiteral("settingsSearch"));
    m_homePage->setPlaceholderText(SettingsStore::defaultHomePage());
    m_homePage->setToolTip(tr("Opened in every new tab. Leave empty for the default."));
    homeLayout->addWidget(m_homePage, 1);
    m_resetHomePage = addReset();
    homeLayout->addWidget(m_resetHomePage);
    form->addRow(tr("Homepage"), homeRow);

    auto* searchRow = new QWidget(this);
    auto* searchLayout = new QHBoxLayout(searchRow);
    searchLayout->setContentsMargins(0, 0, 0, 0);
    searchLayout->setSpacing(8);
    m_searchTemplate = new QLineEdit(searchRow);
    m_searchTemplate->setObjectName(QStringLiteral("settingsSearch"));
    m_searchTemplate->setPlaceholderText(SettingsStore::defaultSearchTemplate());
    m_searchTemplate->setToolTip(
        tr("Used when the address bar holds something that is not a URL.\n"
           "%1 is replaced with the search term."));
    searchLayout->addWidget(m_searchTemplate, 1);
    m_resetSearch = addReset();
    searchLayout->addWidget(m_resetSearch);
    form->addRow(tr("Search address"), searchRow);

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
    m_confirmClose->setToolTip(
        tr("Ask before the window closes with more than one tab open"));
    form->addRow(QString(), m_confirmClose);

    root->addLayout(form);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("settingsStatus"));
    m_status->setWordWrap(true);
    m_status->setVisible(false);
    root->addWidget(m_status);

    auto* note = new QLabel(tr("An address that is not a URL is searched for "
                               "using the template above."), this);
    note->setObjectName(QStringLiteral("settingsFootnote"));
    note->setWordWrap(true);
    root->addWidget(note);

    root->addStretch(1);

    showValues();

    // Committed on every keystroke rather than on editingFinished. That signal
    // fires when the field loses focus, which does not happen when the window is
    // closed by Escape or the close button: the text the user just typed would
    // be discarded with nothing to show that it was never saved.
    connect(m_homePage, &QLineEdit::textChanged, this,
        [this](const QString&) { commitFields(); });
    connect(m_searchTemplate, &QLineEdit::textChanged, this,
        [this](const QString&) { commitFields(); });
    connect(m_downloadDirectory, &QLineEdit::textChanged, this,
        [this](const QString&) { commitFields(); });

    connect(m_confirmClose, &QCheckBox::toggled, this, [this](bool on) {
        SettingsStore::instance().setConfirmBeforeClosingTabs(on);
    });

    connect(m_resetHomePage, &QToolButton::clicked, this, [this] {
        SettingsStore::instance().setHomePage({});
        showValues();
    });
    connect(m_resetSearch, &QToolButton::clicked, this, [this] {
        SettingsStore::instance().setSearchTemplate({});
        showValues();
    });

    connect(m_browseDownloads, &QToolButton::clicked, this, [this] {
        const QString start = m_downloadDirectory->text().isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
            : m_downloadDirectory->text();
        const QString chosen = QFileDialog::getExistingDirectory(
            this, tr("Choose a download folder"), start);
        if (chosen.isEmpty())
            return;
        SettingsStore::instance().setDownloadDirectory(chosen);
        showValues();
    });
}

void GeneralPage::commitFields()
{
    // Guarded, because showValues() sets the text programmatically and that
    // would otherwise be committed back as if the user had typed it -- which
    // would, for instance, write a rejected search template back over the
    // default that searchTemplate() had just substituted.
    if (m_showing)
        return;

    SettingsStore& store = SettingsStore::instance();

    // A homepage that is a phrase would be searched on in every new tab, which is
    // never what was meant, and a search template without the placeholder is
    // stored but never used. Both are collected before anything is written and
    // reported as one message: setting the label as each field is handled would
    // mean the second problem overwrote the first, so fixing one would hide the
    // other.
    QStringList problems;

    const QString home = m_homePage->text().trimmed();

    if (!home.isEmpty() && !SettingsStore::looksLikeAddress(home))
        problems.append(tr("\"%1\" is not a web address, so it would be searched "
                           "for rather than opened.").arg(home));

    const QString search = m_searchTemplate->text().trimmed();
    // Stored as typed either way: the reader rejects a template without the
    // placeholder and falls back to the default, so this is said out loud rather
    // than left for the user to notice that their searches quietly go elsewhere.
    if (!search.isEmpty() && !search.contains(QLatin1String("%1"))) {
        problems.append(tr("The search template needs %1 where the search term "
                           "goes. The default is used until then."));
    }

    store.setHomePage(home);
    store.setSearchTemplate(search);
    store.setDownloadDirectory(m_downloadDirectory->text().trimmed());

    setStatus(problems.join(QLatin1Char(' ')), !problems.isEmpty());
}

void GeneralPage::showValues()
{
    const SettingsStore& store = SettingsStore::instance();

    m_showing = true;
    // The stored value, not the effective one: showing the default in the field
    // would make an unconfigured field look configured, and a reset would have
    // nothing to reset.
    m_homePage->setText(store.storedHomePage());
    m_searchTemplate->setText(store.storedSearchTemplate());
    m_downloadDirectory->setText(store.downloadDirectory());
    m_showing = false;

    m_confirmClose->setChecked(store.confirmBeforeClosingTabs());
    // A reset button that is visible but does nothing is worse than no button.
    m_resetHomePage->setVisible(store.hasCustomHomePage());
    m_resetSearch->setVisible(store.hasCustomSearchTemplate());
}

void GeneralPage::refresh()
{
    showValues();
}

void GeneralPage::setStatus(const QString& message, bool isError)
{
    if (message.isEmpty()) {
        m_status->setVisible(false);
        return;
    }
    m_status->setText(message);
    m_status->setProperty("error", isError);
    // Re-polished so the error colours in the shared stylesheet take effect on a
    // label that already exists.
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
    m_status->setVisible(true);
}

} // namespace iridium
