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
#include <assert.h>
#include <stdio.h>
#include "bt-property-defaults.h"

static const WCHAR *const expectedNames[] = {
    L"DebugLogLevel", L"InitialBaudRate", L"DownloadBaudRate", L"OperationalBaudRate",
    L"PostLaunchDelayMs", L"RegOnDelayMs", L"ConfigureSleepMode", L"ConfigureAudio"
};
static const DWORD defaults[] = {
    BT_DEFAULT_DebugLogLevel, BT_DEFAULT_InitialBaudRate, BT_DEFAULT_DownloadBaudRate,
    BT_DEFAULT_OperationalBaudRate, BT_DEFAULT_PostLaunchDelayMs, BT_DEFAULT_RegOnDelayMs,
    BT_DEFAULT_ConfigureSleepMode, BT_DEFAULT_ConfigureAudio
};
static const DWORD changedValues[] = {4, 230400, 115200, 921600, 5000, 0, 0, 0};
typedef struct { BOOL present; DWORD type, size, data; LSTATUS error; } TEST_VALUE;
static TEST_VALUE registry[8];
static unsigned allocations, frees, writes, attempts, parameterSets, opened, closed, deleted;
static unsigned failWriteAt, readOnlyMask;
static BOOL failAllocate, failCreate, failGet, failSet, acceptPage, failRollback, writeFailed;
static SP_DEVINSTALL_PARAMS_W installed;
static HPROPSHEETPAGE retained;
static WNDPROC oldParentProc;
static unsigned dirtyMessages;

static LRESULT CALLBACK TestParent(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam) {
    if (Message == PSM_CHANGED) dirtyMessages++;
    return CallWindowProcW(oldParentProc, Window, Message, WParam, LParam);
}

static UINT settingIndex(HKEY key, LPCWSTR name) {
    UINT index;
    for (index = 0; index < ARRAYSIZE(expectedNames); index++) {
        if (!wcscmp(name, expectedNames[index])) {
            assert(key == (HKEY)(UINT_PTR)(index == 0 ? 43 : 44));
            return index;
        }
    }
    assert(0); return 0;
}
static LPVOID TestAllocate(HANDLE heap, DWORD flags, SIZE_T size) {
    LPVOID result = failAllocate ? NULL : HeapAlloc(heap, flags, size);
    if (result) allocations++;
    return result;
}
static BOOL TestFree(HANDLE heap, DWORD flags, LPVOID pointer) {
    frees++; return HeapFree(heap, flags, pointer);
}
static HPROPSHEETPAGE TestCreate(const PROPSHEETPAGEW *page) {
    if (failCreate) return NULL;
    assert(wcscmp(page->pszTitle, L"Advanced") == 0);
    return CreatePropertySheetPageW(page);
}
static HKEY openKey(UINT store, REGSAM access) {
    if ((readOnlyMask & (1u << store)) && (access & KEY_SET_VALUE)) {
        SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE;
    }
    opened++; return (HKEY)(UINT_PTR)(store == 0 ? 43 : 44);
}
static HKEY TestOpen(HDEVINFO set, SP_DEVINFO_DATA *device, DWORD scope, DWORD profile, DWORD kind, REGSAM access) {
    assert(set == (HDEVINFO)42 && device->DevInst == 99 && scope == DICS_FLAG_GLOBAL && profile == 0 && kind == DIREG_DRV);
    return openKey(0, access);
}
static LSTATUS TestOpenService(HKEY key, LPCWSTR path, DWORD options, REGSAM access, PHKEY result) {
    assert(key == HKEY_LOCAL_MACHINE && options == 0);
    assert(!wcscmp(path, L"SYSTEM\\CurrentControlSet\\Services\\ap6256bt\\Parameters"));
    *result = openKey(1, access);
    return *result == INVALID_HANDLE_VALUE ? ERROR_ACCESS_DENIED : ERROR_SUCCESS;
}
static LSTATUS TestQuery(HKEY key, LPCWSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD size) {
    TEST_VALUE *value = &registry[settingIndex(key, name)];
    DWORD capacity = *size;
    assert(!reserved);
    if (!value->present) return ERROR_FILE_NOT_FOUND;
    if (value->error) return value->error;
    *type = value->type; *size = value->size;
    if (capacity < value->size) return ERROR_MORE_DATA;
    assert(value->size <= sizeof(value->data));
    CopyMemory(data, &value->data, value->size); return ERROR_SUCCESS;
}
static LSTATUS TestWrite(HKEY key, LPCWSTR name, DWORD reserved, DWORD type, const BYTE *data, DWORD size) {
    TEST_VALUE *value = &registry[settingIndex(key, name)];
    assert(reserved == 0 && size <= sizeof(DWORD));
    attempts++;
    if (attempts == failWriteAt || (writeFailed && failRollback)) {
        writeFailed = TRUE; return ERROR_ACCESS_DENIED;
    }
    value->present = TRUE; value->type = type; value->size = size; value->data = 0;
    CopyMemory(&value->data, data, size); writes++; return ERROR_SUCCESS;
}
static LSTATUS TestDelete(HKEY key, LPCWSTR name) {
    registry[settingIndex(key, name)].present = FALSE;
    deleted++; return ERROR_SUCCESS;
}
static LSTATUS TestClose(HKEY key) {
    assert(key == (HKEY)43 || key == (HKEY)44); closed++; return ERROR_SUCCESS;
}
static BOOL TestGetParams(HDEVINFO set, SP_DEVINFO_DATA *device, SP_DEVINSTALL_PARAMS_W *params) {
    assert(set == (HDEVINFO)42 && device->DevInst == 99 && params->cbSize == sizeof(*params));
    if (failGet) { SetLastError(ERROR_GEN_FAILURE); return FALSE; }
    *params = installed; return TRUE;
}
static BOOL TestSetParams(HDEVINFO set, SP_DEVINFO_DATA *device, SP_DEVINSTALL_PARAMS_W *params) {
    assert(set == (HDEVINFO)42 && device->DevInst == 99);
    if (failSet) { SetLastError(ERROR_GEN_FAILURE); return FALSE; }
    installed = *params; parameterSets++; return TRUE;
}
#define HeapAlloc TestAllocate
#define HeapFree TestFree
#define CreatePropertySheetPageW TestCreate
#define SetupDiOpenDevRegKey TestOpen
#define RegOpenKeyExW TestOpenService
#define RegQueryValueExW TestQuery
#define RegSetValueExW TestWrite
#define RegDeleteValueW TestDelete
#define RegCloseKey TestClose
#define SetupDiGetDeviceInstallParamsW TestGetParams
#define SetupDiSetDeviceInstallParamsW TestSetParams
#include "properties.c"

static BOOL CALLBACK AddPage(HPROPSHEETPAGE page, LPARAM cookie) {
    assert(cookie == 123);
    if (acceptPage) retained = page;
    return acceptPage;
}
static void reset(void) {
    UINT index;
    assert(opened == closed);
    for (index = 0; index < ARRAYSIZE(registry); index++) {
        registry[index].present = TRUE; registry[index].type = REG_DWORD;
        registry[index].size = sizeof(DWORD); registry[index].data = defaults[index]; registry[index].error = 0;
    }
    failGet = failSet = failRollback = writeFailed = FALSE;
    readOnlyMask = failWriteAt = 0;
    ZeroMemory(&installed, sizeof(installed)); installed.cbSize = sizeof(installed);
    installed.FlagsEx = DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
    writes = attempts = parameterSets = deleted = 0;
}
static HWND dialog(HWND parent, BT_PROPERTY_CONTEXT *context) {
    PROPSHEETPAGEW page = {0};
    HWND result;
    RECT client;
    UINT index;
    unsigned before = dirtyMessages;
    page.lParam = (LPARAM)context;
    result = CreateDialogParamW(BtPropertiesModule, MAKEINTRESOURCEW(IDD_ADVANCED), parent, BtPropertyDialog, (LPARAM)&page);
    assert(result != NULL && !IsWindowVisible(result));
    assert(SendDlgItemMessageW(result, IDC_LOG_LEVEL, CB_GETCOUNT, 0, 0) == 5);
    GetClientRect(result, &client);
    for (index = 0; index < BT_SETTING_COUNT; index++) {
        RECT control;
        HWND item = GetDlgItem(result, BtSettings[index].Control);
        assert(item && !wcscmp(BtSettings[index].Name, expectedNames[index]));
        GetWindowRect(item, &control); MapWindowPoints(NULL, result, (POINT *)&control, 2);
        assert(control.left >= 0 && control.top >= 0 && control.right <= client.right && control.bottom <= client.bottom);
    }
    assert(opened == closed);
    assert(dirtyMessages == before);
    return result;
}
static void selectValue(HWND window, UINT index, DWORD value) {
    const BT_SETTING *setting = &BtSettings[index];
    if (setting->Kind == BtDelaySetting) {
        SetDlgItemInt(window, setting->Control, value, FALSE);
    } else {
        LRESULT item, count = SendDlgItemMessageW(window, setting->Control, CB_GETCOUNT, 0, 0);
        for (item = 0; item < count; item++) {
            if ((DWORD)SendDlgItemMessageW(window, setting->Control, CB_GETITEMDATA, (WPARAM)item, 0) == value) {
                SendDlgItemMessageW(window, setting->Control, CB_SETCURSEL, (WPARAM)item, 0); return;
            }
        }
        assert(0);
    }
}
static LONG_PTR apply(HWND window) {
    NMHDR header = {0}; header.code = PSN_APPLY;
    SendMessageW(window, WM_NOTIFY, 0, (LPARAM)&header);
    assert(opened == closed);
    return GetWindowLongPtrW(window, DWLP_MSGRESULT);
}
static void changeAll(HWND window) {
    UINT index;
    for (index = 0; index < BT_SETTING_COUNT; index++) selectValue(window, index, changedValues[index]);
}
static void restoreDefaults(HWND window) {
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(IDC_RESTORE_DEFAULTS, BN_CLICKED),
                 (LPARAM)GetDlgItem(window, IDC_RESTORE_DEFAULTS));
    assert(opened == closed);
}
int main(void) {
    SP_DEVINFO_DATA device = {0};
    SP_PROPSHEETPAGE_REQUEST request = {0};
    BT_PROPERTY_CONTEXT context = {0};
    INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_WIN95_CLASSES};
    HWND parent, window;
    UINT level, index;
    WCHAR text[192];
    assert(InitCommonControlsEx(&controls));
    BtPropertiesModule = GetModuleHandleW(NULL);
    device.cbSize = sizeof(device); device.DevInst = 99;
    request.cbSize = sizeof(request); request.PageRequested = SPPSR_ENUM_ADV_DEVICE_PROPERTIES;
    request.DeviceInfoSet = (HDEVINFO)42; request.DeviceInfoData = &device;
    assert(!Ap6256BtPropPageProvider(NULL, AddPage, 123));
    assert(!Ap6256BtPropPageProvider(&request, NULL, 123));
    request.PageRequested = 0; assert(!Ap6256BtPropPageProvider(&request, AddPage, 123));
    request.PageRequested = SPPSR_ENUM_ADV_DEVICE_PROPERTIES;
    device.cbSize = 0; assert(!Ap6256BtPropPageProvider(&request, AddPage, 123)); device.cbSize = sizeof(device);
    failAllocate = TRUE; assert(!Ap6256BtPropPageProvider(&request, AddPage, 123)); failAllocate = FALSE;
    failCreate = TRUE; assert(!Ap6256BtPropPageProvider(&request, AddPage, 123)); failCreate = FALSE;
    assert(allocations == frees);
    acceptPage = FALSE; assert(!Ap6256BtPropPageProvider(&request, AddPage, 123)); assert(allocations == frees);
    acceptPage = TRUE; assert(Ap6256BtPropPageProvider(&request, AddPage, 123)); assert(allocations == frees + 1);
    assert(DestroyPropertySheetPage(retained)); assert(allocations == frees);
    parent = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 600, 600, NULL, NULL, BtPropertiesModule, NULL);
    assert(parent);
    oldParentProc = (WNDPROC)SetWindowLongPtrW(parent, GWLP_WNDPROC, (LONG_PTR)TestParent);
    context.DeviceInfoSet = (HDEVINFO)42; context.DeviceInfoData = device;
    for (level = 0; level <= 4; level++) {
        reset(); registry[0].data = level; window = dialog(parent, &context);
        assert(context.Original[0] == level);
        SendDlgItemMessageW(window, IDC_LOG_LEVEL, CB_GETLBTEXT, level, (LPARAM)text);
        assert(!wcscmp(text, BtLogNames[level]));
        assert(apply(window) == PSNRET_NOERROR && writes == 0 && parameterSets == 0);
        selectValue(window, 0, (level + 1) % 5);
        assert(apply(window) == PSNRET_NOERROR && writes == 1 && registry[0].data == (level + 1) % 5);
        assert(installed.FlagsEx == (DI_FLAGSEX_ALLOWEXCLUDEDDRVS | DI_FLAGSEX_PROPCHANGE_PENDING));
        assert(apply(window) == PSNRET_NOERROR && writes == 1); DestroyWindow(window);
    }
    reset(); window = dialog(parent, &context);
    for (index = 0; index < BT_SETTING_COUNT; index++) assert(context.Original[index] == defaults[index]);
    changeAll(window); assert(apply(window) == PSNRET_NOERROR && writes == BT_SETTING_COUNT);
    for (index = 0; index < BT_SETTING_COUNT; index++) assert(registry[index].data == changedValues[index]);
    DestroyWindow(window); window = dialog(parent, &context);
    for (index = 0; index < BT_SETTING_COUNT; index++) assert(context.Original[index] == changedValues[index]);
    DestroyWindow(window);
    reset(); registry[1].data = 57600; registry[2].data = 1234567; window = dialog(parent, &context);
    assert(context.Original[1] == 57600 && context.Original[2] == 1234567);
    assert(apply(window) == PSNRET_NOERROR && writes == 0); DestroyWindow(window);
    for (index = 0; index < BT_SETTING_COUNT; index++) {
        reset(); registry[index].data = index == 1 || index == 2 ? 0 : MAXDWORD;
        window = dialog(parent, &context); assert(context.Original[index] == BtSettings[index].Default); DestroyWindow(window);
        reset(); registry[index].present = FALSE; window = dialog(parent, &context);
        assert(context.Original[index] == BtSettings[index].Default); DestroyWindow(window);
    }
    reset(); registry[0].type = REG_SZ; window = dialog(parent, &context); assert(context.Original[0] == 0); DestroyWindow(window);
    reset(); registry[0].size = 3; window = dialog(parent, &context); assert(context.Original[0] == 0); DestroyWindow(window);
    for (index = 0; index < 2; index++) {
        reset(); readOnlyMask = 1u << index; window = dialog(parent, &context);
        assert(IsWindowEnabled(GetDlgItem(window, IDC_LOG_LEVEL)) == (index != 0));
        assert(IsWindowEnabled(GetDlgItem(window, IDC_INITIAL_BAUD)) == (index == 0));
        selectValue(window, index == 0 ? 0 : 1, index == 0 ? 1 : 230400);
        assert(apply(window) == PSNRET_INVALID_NOCHANGEPAGE && writes == 0); DestroyWindow(window);
    }
    reset(); window = dialog(parent, &context); failGet = TRUE; selectValue(window, 0, 1);
    assert(apply(window) == PSNRET_INVALID_NOCHANGEPAGE && writes == 0); failGet = FALSE; failSet = TRUE;
    assert(apply(window) == PSNRET_INVALID_NOCHANGEPAGE && writes == 0); DestroyWindow(window);
    for (level = 1; level <= BT_SETTING_COUNT; level++) {
        reset(); window = dialog(parent, &context); changeAll(window); failWriteAt = level;
        assert(apply(window) == PSNRET_INVALID_NOCHANGEPAGE && !context.SaveIncomplete);
        for (index = 0; index < BT_SETTING_COUNT; index++) assert(registry[index].data == defaults[index]);
        assert(installed.FlagsEx == DI_FLAGSEX_ALLOWEXCLUDEDDRVS); DestroyWindow(window);
    }
    reset(); registry[0].present = FALSE; window = dialog(parent, &context); changeAll(window); failWriteAt = 2;
    assert(apply(window) == PSNRET_INVALID_NOCHANGEPAGE && !registry[0].present && deleted == 1); DestroyWindow(window);
    reset(); window = dialog(parent, &context); changeAll(window); failWriteAt = 3; failRollback = TRUE;
    assert(apply(window) == PSNRET_INVALID_NOCHANGEPAGE && context.SaveIncomplete);
    assert(installed.FlagsEx & DI_FLAGSEX_PROPCHANGE_PENDING);
    GetDlgItemTextW(window, IDC_STATUS, text, ARRAYSIZE(text)); assert(wcsstr(text, L"could not be restored"));
    failWriteAt = 0; failRollback = FALSE; assert(apply(window) == PSNRET_NOERROR && !context.SaveIncomplete); DestroyWindow(window);
    reset(); window = dialog(parent, &context); registry[1].error = ERROR_ACCESS_DENIED; changeAll(window);
    assert(apply(window) == PSNRET_INVALID_NOCHANGEPAGE && writes == 0 && parameterSets == 0); DestroyWindow(window);
    for (index = 0; index < BT_SETTING_COUNT; index++) {
        reset(); window = dialog(parent, &context);
        if (BtSettings[index].Kind == BtDelaySetting) SetDlgItemTextW(window, BtSettings[index].Control, L"5001");
        else SendDlgItemMessageW(window, BtSettings[index].Control, CB_SETCURSEL, (WPARAM)-1, 0);
        assert(apply(window) == PSNRET_INVALID_NOCHANGEPAGE && writes == 0 && parameterSets == 0);
        assert(context.InvalidSetting == index); DestroyWindow(window);
    }
    { const WCHAR *bad[] = {L"", L"-1", L"12x", L"4294967296"};
      for (index = 0; index < ARRAYSIZE(bad); index++) {
        reset(); window = dialog(parent, &context); SetDlgItemTextW(window, IDC_POST_LAUNCH_DELAY, bad[index]);
        assert(apply(window) == PSNRET_INVALID_NOCHANGEPAGE && writes == 0); DestroyWindow(window);
      }
    }
    reset(); window = dialog(parent, &context); selectValue(window, 4, 0); selectValue(window, 5, 5000);
    assert(apply(window) == PSNRET_NOERROR && registry[4].data == 0 && registry[5].data == 5000); DestroyWindow(window);
    reset(); window = dialog(parent, &context); changeAll(window);
    { NMHDR cancel = {0}; cancel.code = PSN_RESET; SendMessageW(window, WM_NOTIFY, 0, (LPARAM)&cancel); }
    assert(writes == 0 && parameterSets == 0); DestroyWindow(window);
    reset(); window = dialog(parent, &context);
    { unsigned before = dirtyMessages;
      restoreDefaults(window); assert(dirtyMessages == before && writes == 0 && parameterSets == 0);
    }
    DestroyWindow(window);
    reset();
    for (index = 0; index < BT_SETTING_COUNT; index++) registry[index].data = changedValues[index];
    window = dialog(parent, &context);
    { unsigned before = dirtyMessages;
      restoreDefaults(window); assert(dirtyMessages == before + 1 && writes == 0 && parameterSets == 0);
    }
    for (index = 0; index < BT_SETTING_COUNT; index++) {
        DWORD value;
        assert(BtGetSetting(window, &BtSettings[index], &value) && value == defaults[index]);
        assert(registry[index].data == changedValues[index] && context.Original[index] == changedValues[index]);
    }
    { NMHDR cancel = {0}; cancel.code = PSN_RESET; SendMessageW(window, WM_NOTIFY, 0, (LPARAM)&cancel); }
    assert(writes == 0 && parameterSets == 0); DestroyWindow(window);
    window = dialog(parent, &context); restoreDefaults(window);
    assert(apply(window) == PSNRET_NOERROR && writes == BT_SETTING_COUNT);
    for (index = 0; index < BT_SETTING_COUNT; index++) assert(registry[index].data == defaults[index]);
    DestroyWindow(window);
    reset(); window = dialog(parent, &context); SetDlgItemTextW(window, IDC_POST_LAUNCH_DELAY, L"");
    restoreDefaults(window); assert(apply(window) == PSNRET_NOERROR && writes == 0); DestroyWindow(window);
    for (level = 1; level <= 3; level++) {
        reset(); readOnlyMask = level;
        for (index = 0; index < BT_SETTING_COUNT; index++) registry[index].data = changedValues[index];
        window = dialog(parent, &context);
        assert(IsWindowEnabled(GetDlgItem(window, IDC_RESTORE_DEFAULTS)) == (level != 3));
        restoreDefaults(window); assert(writes == 0 && parameterSets == 0);
        for (index = 0; index < BT_SETTING_COUNT; index++) {
            DWORD value;
            assert(BtGetSetting(window, &BtSettings[index], &value));
            assert(value == ((readOnlyMask & (1u << BtSettings[index].Store)) ? changedValues[index] : defaults[index]));
        }
        assert(apply(window) == PSNRET_NOERROR);
        for (index = 0; index < BT_SETTING_COUNT; index++) {
            assert(registry[index].data == ((readOnlyMask & (1u << BtSettings[index].Store)) ? changedValues[index] : defaults[index]));
        }
        DestroyWindow(window);
    }
    DestroyWindow(parent);
    assert(allocations == frees && opened == closed);
    puts("Bluetooth properties: eight controls, clean initialization, safe defaults staging, both registry stores, validation, Apply/Cancel, write rollback, restart flags and page lifetime passed");
    return 0;
}
