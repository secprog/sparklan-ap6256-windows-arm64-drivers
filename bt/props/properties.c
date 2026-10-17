/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Licensed under the GNU General Public License, version 3 only.
 * Provided without warranty. See LICENSE for the license text.
 * Additional permission: see BLUETOOTH-LINKING-EXCEPTION.txt.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <setupapi.h>
#include <strsafe.h>
#include "resource.h"

#define BT_SETTING_COUNT 8

typedef enum _BT_SETTING_KIND {
    BtLogSetting,
    BtBaudSetting,
    BtDelaySetting,
    BtBooleanSetting
} BT_SETTING_KIND;

typedef struct _BT_SETTING {
    PCWSTR Name;
    PCWSTR Label;
    UINT Control;
    BT_SETTING_KIND Kind;
    UINT Store;
    DWORD Default;
    DWORD Minimum;
    DWORD Maximum;
} BT_SETTING;

static const BT_SETTING BtSettings[BT_SETTING_COUNT] = {
    {L"DebugLogLevel", L"Log level", IDC_LOG_LEVEL, BtLogSetting, 0, 0, 0, 4},
    {L"InitialBaudRate", L"Startup UART baud rate", IDC_INITIAL_BAUD, BtBaudSetting, 1, 115200, 1, MAXDWORD},
    {L"DownloadBaudRate", L"Download UART baud rate", IDC_DOWNLOAD_BAUD, BtBaudSetting, 1, 921600, 1, MAXDWORD},
    {L"OperationalBaudRate", L"Operating UART baud rate", IDC_OPERATIONAL_BAUD, BtBaudSetting, 1, 3000000, 1, 4000000},
    {L"PostLaunchDelayMs", L"Post-launch delay", IDC_POST_LAUNCH_DELAY, BtDelaySetting, 1, 250, 0, 5000},
    {L"RegOnDelayMs", L"Reset release delay", IDC_REG_ON_DELAY, BtDelaySetting, 1, 100, 0, 5000},
    {L"ConfigureSleepMode", L"Controller sleep", IDC_SLEEP_MODE, BtBooleanSetting, 1, 0, 0, 1},
    {L"ConfigureAudio", L"Configure PCM audio", IDC_PCM_AUDIO, BtBooleanSetting, 1, 0, 0, 1}
};
static const DWORD BtSafeDefaults[BT_SETTING_COUNT] = {
    0, 115200, 921600, 3000000, 250, 100, 1, 1
};

typedef struct _BT_REG_VALUE {
    BOOL Present;
    DWORD Type;
    DWORD Size;
    DWORD Data;
} BT_REG_VALUE;

typedef struct _BT_PROPERTY_CONTEXT {
    LONG References;
    HDEVINFO DeviceInfoSet;
    SP_DEVINFO_DATA DeviceInfoData;
    DWORD Original[BT_SETTING_COUNT];
    UINT InvalidSetting;
    BOOL SaveIncomplete;
    BOOL Initialized;
} BT_PROPERTY_CONTEXT;

static HINSTANCE BtPropertiesModule;
static const WCHAR *const BtLogNames[] = {
    L"Disabled", L"Errors", L"Warnings", L"Information", L"Verbose"
};
static const WCHAR *const BtBooleanNames[] = {L"Disabled", L"Enabled"};
static const DWORD BtBaudRates[] = {
    115200, 230400, 460800, 921600, 1000000,
    1500000, 2000000, 2500000, 3000000, 4000000
};
static const WCHAR BtRestartText[] =
    L"Bluetooth restarts when you save changes and close these properties. "
    L"Delays accept 0-5000 ms. Startup baud must match the controller.";

static VOID
BtReleaseProperties(BT_PROPERTY_CONTEXT *Context)
{
    if (InterlockedDecrement(&Context->References) == 0) {
        HeapFree(GetProcessHeap(), 0, Context);
    }
}

static UINT CALLBACK
BtPropertyCallback(HWND Window, UINT Message, LPPROPSHEETPAGEW Page)
{
    BT_PROPERTY_CONTEXT *context = (BT_PROPERTY_CONTEXT *)Page->lParam;

    UNREFERENCED_PARAMETER(Window);
    if (Message == PSPCB_ADDREF) {
        InterlockedIncrement(&context->References);
    } else if (Message == PSPCB_RELEASE) {
        BtReleaseProperties(context);
    }
    return 1;
}

static HKEY
BtOpenSettings(BT_PROPERTY_CONTEXT *Context, UINT Store, REGSAM Access)
{
    if (Store != 0) {
        HKEY key;
        LSTATUS result = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Services\\ap6256bt\\Parameters", 0, Access, &key);
        if (result != ERROR_SUCCESS) {
            SetLastError((DWORD)result);
            return INVALID_HANDLE_VALUE;
        }
        return key;
    }
    return SetupDiOpenDevRegKey(Context->DeviceInfoSet, &Context->DeviceInfoData,
                               DICS_FLAG_GLOBAL, 0, DIREG_DRV, Access);
}

static DWORD
BtReadValue(HKEY Key, PCWSTR Name, BT_REG_VALUE *Value)
{
    LSTATUS result;

    ZeroMemory(Value, sizeof(*Value));
    Value->Size = sizeof(Value->Data);
    result = RegQueryValueExW(Key, Name, NULL, &Value->Type,
                             (LPBYTE)&Value->Data, &Value->Size);
    if (result == ERROR_FILE_NOT_FOUND) {
        return ERROR_SUCCESS;
    }
    Value->Present = result == ERROR_SUCCESS;
    return (DWORD)result;
}

static LRESULT
BtAddChoice(HWND Dialog, UINT Control, PCWSTR Label, DWORD Value)
{
    LRESULT item = SendDlgItemMessageW(Dialog, Control, CB_ADDSTRING, 0, (LPARAM)Label);
    if (item >= 0) {
        SendDlgItemMessageW(Dialog, Control, CB_SETITEMDATA, (WPARAM)item, Value);
    }
    return item;
}

static VOID
BtInitializeProperties(HWND Dialog, BT_PROPERTY_CONTEXT *Context)
{
    HKEY keys[2];
    BOOL writable[2];
    UINT index;
    UINT store;

    for (store = 0; store < ARRAYSIZE(keys); store++) {
        keys[store] = BtOpenSettings(Context, store, KEY_QUERY_VALUE | KEY_SET_VALUE);
        writable[store] = keys[store] != INVALID_HANDLE_VALUE;
        if (!writable[store]) {
            keys[store] = BtOpenSettings(Context, store, KEY_QUERY_VALUE);
        }
    }
    for (index = 0; index < ARRAYSIZE(BtSettings); index++) {
        const BT_SETTING *setting = &BtSettings[index];
        BT_REG_VALUE raw;
        DWORD value = setting->Default;
        UINT choice;
        LRESULT selected = -1;
        WCHAR text[16];

        if (keys[setting->Store] != INVALID_HANDLE_VALUE &&
            BtReadValue(keys[setting->Store], setting->Name, &raw) == ERROR_SUCCESS &&
            raw.Present && raw.Type == REG_DWORD && raw.Size == sizeof(DWORD) &&
            raw.Data >= setting->Minimum && raw.Data <= setting->Maximum) {
            value = raw.Data;
        }
        Context->Original[index] = value;
        if (setting->Kind == BtDelaySetting) {
            SendDlgItemMessageW(Dialog, setting->Control, EM_SETLIMITTEXT, 10, 0);
            SetDlgItemInt(Dialog, setting->Control, value, FALSE);
        } else if (setting->Kind == BtBaudSetting) {
            for (choice = 0; choice < ARRAYSIZE(BtBaudRates); choice++) {
                LRESULT item;
                StringCchPrintfW(text, ARRAYSIZE(text), L"%lu", BtBaudRates[choice]);
                item = BtAddChoice(Dialog, setting->Control, text, BtBaudRates[choice]);
                if (BtBaudRates[choice] == value) {
                    selected = item;
                }
            }
            if (selected < 0) {
                StringCchPrintfW(text, ARRAYSIZE(text), L"%lu", value);
                selected = BtAddChoice(Dialog, setting->Control, text, value);
            }
            SendDlgItemMessageW(Dialog, setting->Control, CB_SETCURSEL, (WPARAM)selected, 0);
        } else {
            UINT count = setting->Kind == BtLogSetting ? ARRAYSIZE(BtLogNames) : ARRAYSIZE(BtBooleanNames);
            for (choice = 0; choice < count; choice++) {
                BtAddChoice(Dialog, setting->Control,
                    setting->Kind == BtLogSetting ? BtLogNames[choice] : BtBooleanNames[choice], choice);
            }
            SendDlgItemMessageW(Dialog, setting->Control, CB_SETCURSEL, value, 0);
        }
        EnableWindow(GetDlgItem(Dialog, setting->Control), writable[setting->Store]);
    }
    for (store = 0; store < ARRAYSIZE(keys); store++) {
        if (keys[store] != INVALID_HANDLE_VALUE) {
            RegCloseKey(keys[store]);
        }
    }
    Context->InvalidSetting = BT_SETTING_COUNT;
    EnableWindow(GetDlgItem(Dialog, IDC_RESTORE_DEFAULTS), writable[0] || writable[1]);
    if (!writable[0] || !writable[1]) {
        SetDlgItemTextW(Dialog, IDC_STATUS,
                        L"Open Device Manager as administrator to change read-only settings.");
    }
}

static BOOL
BtGetSetting(HWND Dialog, const BT_SETTING *Setting, DWORD *Value)
{
    if (Setting->Kind == BtDelaySetting) {
        WCHAR text[16];
        UINT length = GetDlgItemTextW(Dialog, Setting->Control, text, ARRAYSIZE(text));
        UINT index;
        DWORD value = 0;
        if (length == 0) {
            return FALSE;
        }
        for (index = 0; index < length; index++) {
            DWORD digit = (DWORD)(text[index] - L'0');
            if (digit > 9 || value > (MAXDWORD - digit) / 10) {
                return FALSE;
            }
            value = value * 10 + digit;
        }
        *Value = value;
    } else {
        LRESULT selected = SendDlgItemMessageW(Dialog, Setting->Control, CB_GETCURSEL, 0, 0);
        if (selected < 0) {
            return FALSE;
        }
        *Value = (DWORD)SendDlgItemMessageW(Dialog, Setting->Control, CB_GETITEMDATA, (WPARAM)selected, 0);
    }
    return *Value >= Setting->Minimum && *Value <= Setting->Maximum;
}

static VOID
BtRestoreDefaults(HWND Dialog, BT_PROPERTY_CONTEXT *Context)
{
    BOOL changed = FALSE;
    UINT index;

    Context->Initialized = FALSE;
    for (index = 0; index < ARRAYSIZE(BtSettings); index++) {
        const BT_SETTING *setting = &BtSettings[index];
        DWORD current;
        if (!IsWindowEnabled(GetDlgItem(Dialog, setting->Control))) {
            continue;
        }
        if (!BtGetSetting(Dialog, setting, &current) || current != BtSafeDefaults[index]) {
            changed = TRUE;
        }
        if (setting->Kind == BtDelaySetting) {
            SetDlgItemInt(Dialog, setting->Control, BtSafeDefaults[index], FALSE);
        } else {
            LRESULT item;
            LRESULT count = SendDlgItemMessageW(Dialog, setting->Control, CB_GETCOUNT, 0, 0);
            for (item = 0; item < count; item++) {
                if ((DWORD)SendDlgItemMessageW(Dialog, setting->Control,
                        CB_GETITEMDATA, (WPARAM)item, 0) == BtSafeDefaults[index]) {
                    SendDlgItemMessageW(Dialog, setting->Control, CB_SETCURSEL, (WPARAM)item, 0);
                    break;
                }
            }
        }
    }
    Context->Initialized = TRUE;
    if (changed) {
        PropSheet_Changed(GetParent(Dialog), Dialog);
        SetDlgItemTextW(Dialog, IDC_STATUS,
            L"Safe defaults are selected. Choose Apply or OK to save, then close properties to restart Bluetooth.");
    }
}

static DWORD
BtApplyProperties(HWND Dialog, BT_PROPERTY_CONTEXT *Context)
{
    SP_DEVINSTALL_PARAMS_W parameters;
    SP_DEVINSTALL_PARAMS_W previous;
    HKEY keys[2] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
    DWORD values[BT_SETTING_COUNT];
    BT_REG_VALUE original[BT_SETTING_COUNT];
    BOOL changed[BT_SETTING_COUNT];
    BOOL written[BT_SETTING_COUNT] = {FALSE};
    BOOL anyChanged = FALSE;
    DWORD error = ERROR_SUCCESS;
    DWORD rollbackError = ERROR_SUCCESS;
    UINT index;
    UINT store;

    Context->InvalidSetting = BT_SETTING_COUNT;
    Context->SaveIncomplete = FALSE;
    for (index = 0; index < ARRAYSIZE(BtSettings); index++) {
        if (!BtGetSetting(Dialog, &BtSettings[index], &values[index])) {
            Context->InvalidSetting = index;
            SetFocus(GetDlgItem(Dialog, BtSettings[index].Control));
            return ERROR_INVALID_DATA;
        }
        changed[index] = values[index] != Context->Original[index];
        anyChanged |= changed[index];
    }
    if (!anyChanged) {
        return ERROR_SUCCESS;
    }
    for (index = 0; index < ARRAYSIZE(BtSettings); index++) {
        if (!changed[index]) {
            continue;
        }
        store = BtSettings[index].Store;
        if (keys[store] == INVALID_HANDLE_VALUE) {
            keys[store] = BtOpenSettings(Context, store, KEY_QUERY_VALUE | KEY_SET_VALUE);
            if (keys[store] == INVALID_HANDLE_VALUE) {
                error = GetLastError();
                goto Exit;
            }
        }
        error = BtReadValue(keys[store], BtSettings[index].Name, &original[index]);
        if (error != ERROR_SUCCESS) {
            goto Exit;
        }
    }
    ZeroMemory(&parameters, sizeof(parameters));
    parameters.cbSize = sizeof(parameters);
    if (!SetupDiGetDeviceInstallParamsW(Context->DeviceInfoSet,
                                        &Context->DeviceInfoData, &parameters)) {
        error = GetLastError();
        goto Exit;
    }
    previous = parameters;
    parameters.FlagsEx |= DI_FLAGSEX_PROPCHANGE_PENDING;
    if (!SetupDiSetDeviceInstallParamsW(Context->DeviceInfoSet,
                                        &Context->DeviceInfoData, &parameters)) {
        error = GetLastError();
        goto Exit;
    }
    for (index = 0; index < ARRAYSIZE(BtSettings); index++) {
        if (changed[index]) {
            error = (DWORD)RegSetValueExW(keys[BtSettings[index].Store],
                BtSettings[index].Name, 0, REG_DWORD, (const BYTE *)&values[index], sizeof(DWORD));
            if (error != ERROR_SUCCESS) {
                goto Rollback;
            }
            written[index] = TRUE;
        }
    }
    CopyMemory(Context->Original, values, sizeof(values));
    goto Exit;

Rollback:
    for (index = BT_SETTING_COUNT; index-- > 0;) {
        if (written[index]) {
            DWORD result;
            store = BtSettings[index].Store;
            if (original[index].Present) {
                result = (DWORD)RegSetValueExW(keys[store], BtSettings[index].Name, 0,
                    original[index].Type, (const BYTE *)&original[index].Data, original[index].Size);
            } else {
                result = (DWORD)RegDeleteValueW(keys[store], BtSettings[index].Name);
            }
            if (result != ERROR_SUCCESS) {
                rollbackError = result;
            }
        }
    }
    if (rollbackError == ERROR_SUCCESS) {
        SetupDiSetDeviceInstallParamsW(Context->DeviceInfoSet,
                                       &Context->DeviceInfoData, &previous);
    } else {
        Context->SaveIncomplete = TRUE;
        error = rollbackError;
    }

Exit:
    for (store = 0; store < ARRAYSIZE(keys); store++) {
        if (keys[store] != INVALID_HANDLE_VALUE) {
            RegCloseKey(keys[store]);
        }
    }
    return error;
}

static INT_PTR CALLBACK
BtPropertyDialog(HWND Dialog, UINT Message, WPARAM WParam, LPARAM LParam)
{
    BT_PROPERTY_CONTEXT *context = (BT_PROPERTY_CONTEXT *)GetWindowLongPtrW(Dialog, DWLP_USER);

    if (Message == WM_INITDIALOG) {
        const PROPSHEETPAGEW *page = (const PROPSHEETPAGEW *)LParam;
        context = (BT_PROPERTY_CONTEXT *)page->lParam;
        SetWindowLongPtrW(Dialog, DWLP_USER, (LONG_PTR)context);
        context->Initialized = FALSE;
        BtInitializeProperties(Dialog, context);
        context->Initialized = TRUE;
        return TRUE;
    }
    if (context == NULL) {
        return FALSE;
    }
    if (Message == WM_COMMAND && context->Initialized &&
        LOWORD(WParam) == IDC_RESTORE_DEFAULTS && HIWORD(WParam) == BN_CLICKED) {
        if (IsWindowEnabled(GetDlgItem(Dialog, IDC_RESTORE_DEFAULTS))) {
            BtRestoreDefaults(Dialog, context);
        }
        return TRUE;
    }
    if (Message == WM_COMMAND && context->Initialized &&
        (HIWORD(WParam) == CBN_SELCHANGE || HIWORD(WParam) == EN_CHANGE)) {
        UINT index;
        for (index = 0; index < ARRAYSIZE(BtSettings); index++) {
            if (LOWORD(WParam) == BtSettings[index].Control) {
                PropSheet_Changed(GetParent(Dialog), Dialog);
                return TRUE;
            }
        }
    }
    if (Message == WM_NOTIFY && ((const NMHDR *)LParam)->code == PSN_APPLY) {
        DWORD error = BtApplyProperties(Dialog, context);
        if (error != ERROR_SUCCESS) {
            WCHAR text[192];
            if (context->InvalidSetting < BT_SETTING_COUNT) {
                const BT_SETTING *setting = &BtSettings[context->InvalidSetting];
                StringCchPrintfW(text, ARRAYSIZE(text), L"Choose a valid value for %s (%lu-%lu).",
                    setting->Label, setting->Minimum, setting->Maximum);
            } else {
                StringCchPrintfW(text, ARRAYSIZE(text), context->SaveIncomplete ?
                    L"Some settings changed and could not be restored (error %lu). Reopen properties to review." :
                    L"Could not save settings (error %lu).", error);
            }
            SetDlgItemTextW(Dialog, IDC_STATUS, text);
            SetWindowLongPtrW(Dialog, DWLP_MSGRESULT, PSNRET_INVALID_NOCHANGEPAGE);
        } else {
            SetDlgItemTextW(Dialog, IDC_STATUS, BtRestartText);
            SetWindowLongPtrW(Dialog, DWLP_MSGRESULT, PSNRET_NOERROR);
        }
        return TRUE;
    }
    return FALSE;
}

__declspec(dllexport) BOOL CALLBACK
Ap6256BtPropPageProvider(PVOID Request, LPFNADDPROPSHEETPAGE AddPage, LPARAM Context)
{
    const SP_PROPSHEETPAGE_REQUEST *request = (const SP_PROPSHEETPAGE_REQUEST *)Request;
    BT_PROPERTY_CONTEXT *context;
    PROPSHEETPAGEW page;
    HPROPSHEETPAGE handle;
    BOOL added;

    if (request == NULL || request->cbSize != sizeof(*request) || AddPage == NULL ||
        request->PageRequested != SPPSR_ENUM_ADV_DEVICE_PROPERTIES ||
        request->DeviceInfoData == NULL ||
        request->DeviceInfoData->cbSize != sizeof(SP_DEVINFO_DATA)) {
        return FALSE;
    }
    context = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*context));
    if (context == NULL) {
        return FALSE;
    }
    context->References = 1;
    context->DeviceInfoSet = request->DeviceInfoSet;
    context->DeviceInfoData = *request->DeviceInfoData;

    ZeroMemory(&page, sizeof(page));
    page.dwSize = sizeof(page);
    page.dwFlags = PSP_USECALLBACK | PSP_USETITLE;
    page.hInstance = BtPropertiesModule;
    page.pszTemplate = MAKEINTRESOURCEW(IDD_ADVANCED);
    page.pszTitle = L"Advanced";
    page.pfnDlgProc = BtPropertyDialog;
    page.lParam = (LPARAM)context;
    page.pfnCallback = BtPropertyCallback;
    handle = CreatePropertySheetPageW(&page);
    added = handle != NULL && AddPage(handle, Context);
    if (handle != NULL && !added) {
        DestroyPropertySheetPage(handle);
    }
    BtReleaseProperties(context);
    return added;
}

BOOL WINAPI
DllMain(HINSTANCE Module, DWORD Reason, LPVOID Reserved)
{
    UNREFERENCED_PARAMETER(Reserved);
    if (Reason == DLL_PROCESS_ATTACH) {
        BtPropertiesModule = Module;
    }
    return TRUE;
}
