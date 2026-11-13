#pragma once

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QUrl>

#include "preferences/dialog/dlgpreferencepage.h"
#include "preferences/usersettings.h"

class Library;
class QGroupBox;
class QFormLayout;

/// Preferences page for the OpenSubsonic (Subsonic / OpenSubsonic) backend.
///
/// The credentials are stored in the Mixxx configuration file
/// (mixxx.cfg, section [OpenSubsonic]) and are picked up by the library
/// feature immediately after "Apply".
class DlgPrefOpenSubsonic : public DlgPreferencePage {
    Q_OBJECT
  public:
    DlgPrefOpenSubsonic(QWidget* pParent,
            UserSettingsPointer pConfig,
            Library* pLibrary);
    ~DlgPrefOpenSubsonic() override = default;

    QUrl helpUrl() const override;

  public slots:
    void slotUpdate() override;
    void slotApply() override;
    void slotResetToDefaults() override;

  private:
    void loadSettings();
    void saveSettings();
    void updateStatusHint();

    UserSettingsPointer m_pConfig;
    Library* m_pLibrary;

    QLineEdit* m_pServerUrl;
    QLineEdit* m_pUserName;
    QLineEdit* m_pPassword;
    QLabel* m_pStatusLabel;
};
