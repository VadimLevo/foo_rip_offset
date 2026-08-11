#pragma once

#include <foobar2000/SDK/foobar2000.h>
#include <atlbase.h>
#include <atlwin.h>
#include <atlstr.h>
#include <shlobj.h> // Для окна выбора папки
#include "resource.h"

// --- Глобальные настройки плагина (автоматически сохраняются foobar2000) ---
static const GUID guid_cfg_same_as_source = { 0x18b33732, 0xf269, 0x45f4, { 0xa0, 0xd3, 0xf8, 0xbc, 0x42, 0x87, 0x8e, 0xc2 } };
static const GUID guid_cfg_create_subfolder = { 0x22da111f, 0xbd53, 0x4809, { 0xb7, 0x01, 0xeb, 0xa4, 0x7c, 0x70, 0x62, 0x4e } };
static const GUID guid_cfg_subfolder_name = { 0xd0b8c802, 0x3d2d, 0x4523, { 0x93, 0x3b, 0xef, 0xa5, 0x3d, 0x1c, 0xba, 0x82 } };
static const GUID guid_cfg_custom_path = { 0x7a950dae, 0x4fe0, 0x4660, { 0x84, 0x51, 0xba, 0x1b, 0x5a, 0xc7, 0x3e, 0xf7 } };

static cfg_bool   cfg_same_as_source(guid_cfg_same_as_source, true);
static cfg_bool   cfg_create_subfolder(guid_cfg_create_subfolder, false);
static cfg_string cfg_subfolder_name(guid_cfg_subfolder_name, "AccurateRip_Fixed");
static cfg_string cfg_custom_path(guid_cfg_custom_path, "");

class COffsetDialog : public CDialogImpl<COffsetDialog> {
public:
    enum { IDD = IDD_DIALOG1 };

    // Переменные для хранения результатов ввода пользователя
    int m_offset_value = 0;
    bool m_same_as_source = true;
    bool m_create_subfolder = false;
    CString m_subfolder_name = _T("");
    CString m_custom_path = _T("");

    BEGIN_MSG_MAP(COffsetDialog)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
        COMMAND_ID_HANDLER(IDOK, OnOK)
        COMMAND_ID_HANDLER(IDCANCEL, OnCancel)
        COMMAND_ID_HANDLER(IDC_BTN_BROWSE, OnBrowse)
        // Обработчики кликов для обновления активности полей
        COMMAND_HANDLER(IDC_RADIO_SAME, BN_CLICKED, OnConfigChanged)
        COMMAND_HANDLER(IDC_RADIO_CUSTOM, BN_CLICKED, OnConfigChanged)
        COMMAND_HANDLER(IDC_CHK_SUBFOLDER, BN_CLICKED, OnConfigChanged)
    END_MSG_MAP()

    LRESULT OnInitDialog(UINT, WPARAM, LPARAM, BOOL&) {
        // 1. Устанавливаем оффсет
        SetDlgItemInt(IDC_EDIT1, m_offset_value, TRUE);

        // 2. Читаем сохраненные флаги
        m_same_as_source = cfg_same_as_source.get_value();
        m_create_subfolder = cfg_create_subfolder.get_value();

        // 3. Выставляем состояния радиокнопок и чекбокса
        CheckDlgButton(IDC_RADIO_SAME, m_same_as_source ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(IDC_RADIO_CUSTOM, !m_same_as_source ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(IDC_CHK_SUBFOLDER, m_create_subfolder ? BST_CHECKED : BST_UNCHECKED);

        // 4. Загружаем пути и конвертируем UTF-8 из cfg_string в формат Windows (TCHAR)
        pfc::stringcvt::string_os_from_utf8 os_subfolder(cfg_subfolder_name.get_ptr());
        m_subfolder_name = os_subfolder.get_ptr();
        SetDlgItemText(IDC_EDIT_SUBFOLDER, m_subfolder_name);

        pfc::stringcvt::string_os_from_utf8 os_custom(cfg_custom_path.get_ptr());
        m_custom_path = os_custom.get_ptr();
        SetDlgItemText(IDC_EDIT_CUSTOM_PATH, m_custom_path);

        // 5. Обновляем блокировку полей и центрируем окно
        UpdateUI();
        CenterWindow(GetParent());
        return TRUE;
    }

    // Вспомогательная функция для блокировки/разблокировки элементов
    void UpdateUI() {
        BOOL isSame = IsDlgButtonChecked(IDC_RADIO_SAME);
        ::EnableWindow(GetDlgItem(IDC_EDIT_CUSTOM_PATH), !isSame);
        ::EnableWindow(GetDlgItem(IDC_BTN_BROWSE), !isSame);

        BOOL hasSub = IsDlgButtonChecked(IDC_CHK_SUBFOLDER);
        ::EnableWindow(GetDlgItem(IDC_EDIT_SUBFOLDER), hasSub);
    }

    LRESULT OnConfigChanged(WORD, WORD, HWND, BOOL&) {
        UpdateUI();
        return 0;
    }

    LRESULT OnBrowse(WORD, WORD, HWND, BOOL&) {
        BROWSEINFO bi = { 0 };
        bi.hwndOwner = m_hWnd;
        bi.lpszTitle = _T("Select Output Folder");
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;

        LPITEMIDLIST pidl = SHBrowseForFolder(&bi);
        if (pidl != 0) {
            TCHAR path[MAX_PATH];
            if (SHGetPathFromIDList(pidl, path)) {
                SetDlgItemText(IDC_EDIT_CUSTOM_PATH, path);
            }
            CoTaskMemFree(pidl);
        }
        return 0;
    }

    LRESULT OnOK(WORD, WORD wID, HWND, BOOL&) {
        // 1. Читаем Оффсет
        BOOL translated = FALSE;
        int val = GetDlgItemInt(IDC_EDIT1, &translated, TRUE);
        if (translated) m_offset_value = val;

        // 2. Читаем радиокнопки и текст из полей
        m_same_as_source = (IsDlgButtonChecked(IDC_RADIO_SAME) == BST_CHECKED);
        m_create_subfolder = (IsDlgButtonChecked(IDC_CHK_SUBFOLDER) == BST_CHECKED);

        GetDlgItemText(IDC_EDIT_CUSTOM_PATH, m_custom_path);
        GetDlgItemText(IDC_EDIT_SUBFOLDER, m_subfolder_name);

        // 3. Сохраняем значения в конфигурацию foobar2000
        cfg_same_as_source = m_same_as_source;
        cfg_create_subfolder = m_create_subfolder;

        pfc::stringcvt::string_utf8_from_os utf8_subfolder(m_subfolder_name);
        cfg_subfolder_name = utf8_subfolder.get_ptr();

        pfc::stringcvt::string_utf8_from_os utf8_custom(m_custom_path);
        cfg_custom_path = utf8_custom.get_ptr();

        EndDialog(wID);
        return 0;
    }

    LRESULT OnCancel(WORD, WORD wID, HWND, BOOL&) {
        EndDialog(wID);
        return 0;
    }
};