#include "preferences/dialog/dlgprefnavidrome.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>
#include <utility>

#include "library/library.h"
#include "moc_dlgprefnavidrome.cpp"
#include "preferences/configobject.h"

namespace {

// Must match the keys used by mixxx::navidrome::NavidromeFeature.
const ConfigKey kServerUrlConfigKey("[Navidrome]", "server_url");
const ConfigKey kUserNameConfigKey("[Navidrome]", "username");
const ConfigKey kPasswordConfigKey("[Navidrome]", "password");

} // namespace

DlgPrefNavidrome::DlgPrefNavidrome(
        QWidget* pParent,
        UserSettingsPointer pConfig,
        Library* pLibrary)
        : DlgPreferencePage(pParent),
          m_pConfig(std::move(pConfig)),
          m_pLibrary(pLibrary),
          m_pServerUrl(new QLineEdit(this)),
          m_pUserName(new QLineEdit(this)),
          m_pPassword(new QLineEdit(this)),
          m_pStatusLabel(new QLabel(this)) {
    m_pServerUrl->setPlaceholderText(QStringLiteral("https://musik.example.org"));
    m_pUserName->setPlaceholderText(tr("User name"));
    m_pPassword->setEchoMode(QLineEdit::Password);

    auto* pGroupBox = new QGroupBox(tr("Navidrome Server"), this);
    auto* pFormLayout = new QFormLayout(pGroupBox);
    pFormLayout->addRow(tr("&Server URL:"), m_pServerUrl);
    pFormLayout->addRow(tr("&User name:"), m_pUserName);
    pFormLayout->addRow(tr("&Password:"), m_pPassword);

    auto* pHintLabel = new QLabel(
            tr("The remote library is browsed with the Subsonic/OpenSubsonic "
               "API. Songs are only downloaded locally when they are dragged "
               "onto a deck."),
            this);
    pHintLabel->setWordWrap(true);

    m_pStatusLabel->setWordWrap(true);
    m_pStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* pLayout = new QVBoxLayout(this);
    pLayout->addWidget(pGroupBox);
    pLayout->addWidget(pHintLabel);
    pLayout->addWidget(m_pStatusLabel);
    pLayout->addStretch();

    loadSettings();
}

QUrl DlgPrefNavidrome::helpUrl() const {
    return QUrl(QStringLiteral(
            "https://manual.mixxx.org/latest/en/chapters/library.html"));
}

void DlgPrefNavidrome::slotUpdate() {
    loadSettings();
}

void DlgPrefNavidrome::slotApply() {
    saveSettings();
    // Let the library pick up the new settings without a restart.
    if (m_pLibrary) {
        m_pLibrary->reloadNavidromeSettings();
    }
}

void DlgPrefNavidrome::slotResetToDefaults() {
    m_pServerUrl->clear();
    m_pUserName->clear();
    m_pPassword->clear();
    updateStatusHint();
}

void DlgPrefNavidrome::loadSettings() {
    m_pServerUrl->setText(
            m_pConfig->getValueString(kServerUrlConfigKey).trimmed());
    m_pUserName->setText(
            m_pConfig->getValueString(kUserNameConfigKey).trimmed());
    m_pPassword->setText(m_pConfig->getValueString(kPasswordConfigKey));
    updateStatusHint();
}

void DlgPrefNavidrome::saveSettings() {
    QString serverUrl = m_pServerUrl->text().trimmed();
    // Be forgiving about a missing scheme.
    if (!serverUrl.isEmpty() &&
            !serverUrl.startsWith(QLatin1String("http://")) &&
            !serverUrl.startsWith(QLatin1String("https://"))) {
        serverUrl.prepend(QLatin1String("https://"));
        m_pServerUrl->setText(serverUrl);
    }
    // Trailing slashes are added by the client itself.
    while (serverUrl.endsWith(QLatin1Char('/'))) {
        serverUrl.chop(1);
    }

    m_pConfig->setValue(kServerUrlConfigKey, serverUrl);
    m_pConfig->setValue(kUserNameConfigKey, m_pUserName->text().trimmed());
    m_pConfig->setValue(kPasswordConfigKey, m_pPassword->text());
    updateStatusHint();
}

void DlgPrefNavidrome::updateStatusHint() {
    const QString serverUrl = m_pServerUrl->text().trimmed();
    if (serverUrl.isEmpty() || m_pUserName->text().trimmed().isEmpty() ||
            m_pPassword->text().isEmpty()) {
        m_pStatusLabel->setText(
                tr("Incomplete configuration: server URL, user name and "
                   "password are required."));
        return;
    }
    m_pStatusLabel->setText(
            tr("Configured. The credentials are stored in the Mixxx "
               "settings file (section [Navidrome]).\n"
               "The password itself is never sent to the server, only a "
               "salted MD5 token."));
}
