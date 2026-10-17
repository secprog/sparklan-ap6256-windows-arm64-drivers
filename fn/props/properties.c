/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Licensed under the GNU General Public License, version 3 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <setupapi.h>
#include <strsafe.h>
#include "resource.h"

typedef struct _FN_PROPERTY_CONTEXT {
    LONG References;
    HDEVINFO DeviceInfoSet;
    SP_DEVINFO_DATA DeviceInfoData;
    DWORD OriginalLevel;
    BOOL Initialized;
} FN_PROPERTY_CONTEXT;

static HINSTANCE FnPropertiesModule;
static const WCHAR FnLogValue[] = L"DebugLogLevel";
static const WCHAR *const FnLogNames[] = {
    L"Disabled", L"Errors", L"Warnings", L"Information", L"Verbose"
};
static const WCHAR FnRestartText[] =
    L"Save changes and close these properties to restart this companion device.";

static VOID
FnReleaseProperties(FN_PROPERTY_CONTEXT *Context)
{
    if (InterlockedDecrement(&Context->References) == 0) {
        HeapFree(GetProcessHeap(), 0, Context);
    }
}

static UINT CALLBACK
FnPropertyCallback(HWND Window, UINT Message, LPPROPSHEETPAGEW Page)
{
    FN_PROPERTY_CONTEXT *context = (FN_PROPERTY_CONTEXT *)Page->lParam;
    UNREFERENCED_PARAMETER(Window);
    if (Message == PSPCB_ADDREF) {
        InterlockedIncrement(&context->References);
    } else if (Message == PSPCB_RELEASE) {
        FnReleaseProperties(context);
    }
    return 1;
}

static HKEY
FnOpenSettings(FN_PROPERTY_CONTEXT *Context, REGSAM Access)
{
    return SetupDiOpenDevRegKey(Context->DeviceInfoSet, &Context->DeviceInfoData,
                               DICS_FLAG_GLOBAL, 0, DIREG_DRV, Access);
}

static VOID
FnInitializeProperties(HWND Dialog, FN_PROPERTY_CONTEXT *Context)
{
    HKEY key = FnOpenSettings(Context, KEY_QUERY_VALUE | KEY_SET_VALUE);
    BOOL writable = key != INVALID_HANDLE_VALUE;
    DWORD level = 0;
    DWORD type = 0;
    DWORD size = sizeof(level);
    UINT index;

    if (!writable) {
        key = FnOpenSettings(Context, KEY_QUERY_VALUE);
    }
    if (key != INVALID_HANDLE_VALUE) {
        if (RegQueryValueExW(key, FnLogValue, NULL, &type,
                            (LPBYTE)&level, &size) != ERROR_SUCCESS ||
            type != REG_DWORD || size != sizeof(level) || level >= ARRAYSIZE(FnLogNames)) {
            level = 0;
        }
        RegCloseKey(key);
    }
    Context->OriginalLevel = level;
    for (index = 0; index < ARRAYSIZE(FnLogNames); index++) {
        SendDlgItemMessageW(Dialog, IDC_LOG_LEVEL, CB_ADDSTRING, 0, (LPARAM)FnLogNames[index]);
    }
    SendDlgItemMessageW(Dialog, IDC_LOG_LEVEL, CB_SETCURSEL, level, 0);
    EnableWindow(GetDlgItem(Dialog, IDC_LOG_LEVEL), writable);
    EnableWindow(GetDlgItem(Dialog, IDC_RESTORE_DEFAULTS), writable);
    if (!writable) {
        SetDlgItemTextW(Dialog, IDC_STATUS, L"Open Device Manager as administrator to change Log Level.");
    }
}

static DWORD
FnApplyProperties(HWND Dialog, FN_PROPERTY_CONTEXT *Context)
{
    LRESULT selected = SendDlgItemMessageW(Dialog, IDC_LOG_LEVEL, CB_GETCURSEL, 0, 0);
    SP_DEVINSTALL_PARAMS_W parameters;
    SP_DEVINSTALL_PARAMS_W previous;
    HKEY key;
    LSTATUS result;
    DWORD level;
    DWORD error;

    if (selected < 0 || selected >= ARRAYSIZE(FnLogNames)) {
        return ERROR_INVALID_DATA;
    }
    level = (DWORD)selected;
    if (level == Context->OriginalLevel) {
        return ERROR_SUCCESS;
    }
    key = FnOpenSettings(Context, KEY_SET_VALUE);
    if (key == INVALID_HANDLE_VALUE) {
        return GetLastError();
    }
    ZeroMemory(&parameters, sizeof(parameters));
    parameters.cbSize = sizeof(parameters);
    if (!SetupDiGetDeviceInstallParamsW(Context->DeviceInfoSet, &Context->DeviceInfoData, &parameters)) {
        error = GetLastError();
        RegCloseKey(key);
        return error;
    }
    previous = parameters;
    parameters.FlagsEx |= DI_FLAGSEX_PROPCHANGE_PENDING;
    if (!SetupDiSetDeviceInstallParamsW(Context->DeviceInfoSet, &Context->DeviceInfoData, &parameters)) {
        error = GetLastError();
        RegCloseKey(key);
        return error;
    }
    result = RegSetValueExW(key, FnLogValue, 0, REG_DWORD, (const BYTE *)&level, sizeof(level));
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) {
        SetupDiSetDeviceInstallParamsW(Context->DeviceInfoSet, &Context->DeviceInfoData, &previous);
        return (DWORD)result;
    }
    Context->OriginalLevel = level;
    return ERROR_SUCCESS;
}

static INT_PTR CALLBACK
FnPropertyDialog(HWND Dialog, UINT Message, WPARAM WParam, LPARAM LParam)
{
    FN_PROPERTY_CONTEXT *context = (FN_PROPERTY_CONTEXT *)GetWindowLongPtrW(Dialog, DWLP_USER);
    if (Message == WM_INITDIALOG) {
        const PROPSHEETPAGEW *page = (const PROPSHEETPAGEW *)LParam;
        context = (FN_PROPERTY_CONTEXT *)page->lParam;
        SetWindowLongPtrW(Dialog, DWLP_USER, (LONG_PTR)context);
        context->Initialized = FALSE;
        FnInitializeProperties(Dialog, context);
        context->Initialized = TRUE;
        return TRUE;
    }
    if (context == NULL) {
        return FALSE;
    }
    if (Message == WM_COMMAND && context->Initialized) {
        if (LOWORD(WParam) == IDC_LOG_LEVEL && HIWORD(WParam) == CBN_SELCHANGE) {
            PropSheet_Changed(GetParent(Dialog), Dialog);
            return TRUE;
        }
        if (LOWORD(WParam) == IDC_RESTORE_DEFAULTS && HIWORD(WParam) == BN_CLICKED) {
            if (IsWindowEnabled(GetDlgItem(Dialog, IDC_LOG_LEVEL)) &&
                SendDlgItemMessageW(Dialog, IDC_LOG_LEVEL, CB_GETCURSEL, 0, 0) != 0) {
                SendDlgItemMessageW(Dialog, IDC_LOG_LEVEL, CB_SETCURSEL, 0, 0);
                PropSheet_Changed(GetParent(Dialog), Dialog);
                SetDlgItemTextW(Dialog, IDC_STATUS, L"Disabled is selected. Choose Apply or OK to save.");
            }
            return TRUE;
        }
    }
    if (Message == WM_NOTIFY && ((const NMHDR *)LParam)->code == PSN_APPLY) {
        DWORD error = FnApplyProperties(Dialog, context);
        if (error != ERROR_SUCCESS) {
            WCHAR text[128];
            StringCchPrintfW(text, ARRAYSIZE(text), L"Could not save Log Level (error %lu).", error);
            SetDlgItemTextW(Dialog, IDC_STATUS, text);
            SetWindowLongPtrW(Dialog, DWLP_MSGRESULT, PSNRET_INVALID_NOCHANGEPAGE);
        } else {
            SetDlgItemTextW(Dialog, IDC_STATUS, FnRestartText);
            SetWindowLongPtrW(Dialog, DWLP_MSGRESULT, PSNRET_NOERROR);
        }
        return TRUE;
    }
    return FALSE;
}

__declspec(dllexport) BOOL CALLBACK
Ap6256FnPropPageProvider(PVOID Request, LPFNADDPROPSHEETPAGE AddPage, LPARAM Context)
{
    const SP_PROPSHEETPAGE_REQUEST *request = (const SP_PROPSHEETPAGE_REQUEST *)Request;
    FN_PROPERTY_CONTEXT *context;
    PROPSHEETPAGEW page;
    HPROPSHEETPAGE handle;
    BOOL added;

    if (request == NULL || request->cbSize != sizeof(*request) || AddPage == NULL ||
        request->PageRequested != SPPSR_ENUM_ADV_DEVICE_PROPERTIES ||
        request->DeviceInfoData == NULL || request->DeviceInfoData->cbSize != sizeof(SP_DEVINFO_DATA)) {
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
    page.hInstance = FnPropertiesModule;
    page.pszTemplate = MAKEINTRESOURCEW(IDD_ADVANCED);
    page.pszTitle = L"Advanced";
    page.pfnDlgProc = FnPropertyDialog;
    page.lParam = (LPARAM)context;
    page.pfnCallback = FnPropertyCallback;
    handle = CreatePropertySheetPageW(&page);
    added = handle != NULL && AddPage(handle, Context);
    if (handle != NULL && !added) {
        DestroyPropertySheetPage(handle);
    }
    FnReleaseProperties(context);
    return added;
}

BOOL WINAPI
DllMain(HINSTANCE Module, DWORD Reason, LPVOID Reserved)
{
    UNREFERENCED_PARAMETER(Reserved);
    if (Reason == DLL_PROCESS_ATTACH) {
        FnPropertiesModule = Module;
    }
    return TRUE;
}
