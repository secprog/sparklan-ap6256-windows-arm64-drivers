/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Licensed under the GNU General Public License, version 3 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <setupapi.h>
#include <assert.h>
#include <stdio.h>

static unsigned allocations, frees, opened, closed, writes, parameterSets, dirty;
static BOOL failAllocate, failCreate, failGet, failSet, readOnly, acceptPage;
static DWORD storedLevels[4], storedType = REG_DWORD, storedSize = 4;
static LSTATUS readError, writeError;
static SP_DEVINSTALL_PARAMS_W installed;
static HPROPSHEETPAGE retained;
static WNDPROC oldParent;
static LRESULT CALLBACK TestParent(HWND w, UINT m, WPARAM p, LPARAM l) {
    if (m == PSM_CHANGED) dirty++;
    return CallWindowProcW(oldParent, w, m, p, l);
}
static LPVOID TestAllocate(HANDLE heap, DWORD flags, SIZE_T size) {
    LPVOID result = failAllocate ? NULL : HeapAlloc(heap, flags, size);
    if (result) allocations++;
    return result;
}
static BOOL TestFree(HANDLE heap, DWORD flags, LPVOID pointer) { frees++; return HeapFree(heap, flags, pointer); }
static HPROPSHEETPAGE TestCreate(const PROPSHEETPAGEW *page) {
    if (failCreate) return NULL;
    assert(!wcscmp(page->pszTitle, L"Advanced")); return CreatePropertySheetPageW(page);
}
static HKEY TestOpen(HDEVINFO set, SP_DEVINFO_DATA *device, DWORD scope, DWORD profile, DWORD kind, REGSAM access) {
    assert(set == (HDEVINFO)42 && (device->DevInst == 2 || device->DevInst == 3) && scope == DICS_FLAG_GLOBAL && !profile && kind == DIREG_DRV);
    if (readOnly && (access & KEY_SET_VALUE)) { SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
    opened++; return (HKEY)(UINT_PTR)device->DevInst;
}
static LSTATUS TestQuery(HKEY key, LPCWSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD size) {
    assert((key == (HKEY)2 || key == (HKEY)3) && !wcscmp(name, L"DebugLogLevel") && !reserved && *size == 4);
    *type = storedType; CopyMemory(data, &storedLevels[(UINT_PTR)key], 4); *size = storedSize; return readError;
}
static LSTATUS TestWrite(HKEY key, LPCWSTR name, DWORD reserved, DWORD type, const BYTE *data, DWORD size) {
    assert((key == (HKEY)2 || key == (HKEY)3) && !wcscmp(name, L"DebugLogLevel") && !reserved && type == REG_DWORD && size == 4);
    if (writeError) return writeError;
    CopyMemory(&storedLevels[(UINT_PTR)key], data, 4); writes++; return 0;
}
static LSTATUS TestClose(HKEY key) { assert(key == (HKEY)2 || key == (HKEY)3); closed++; return 0; }
static BOOL TestGetParams(HDEVINFO set, SP_DEVINFO_DATA *device, SP_DEVINSTALL_PARAMS_W *params) {
    assert(set == (HDEVINFO)42 && (device->DevInst == 2 || device->DevInst == 3) && params->cbSize == sizeof(*params));
    if (failGet) { SetLastError(ERROR_GEN_FAILURE); return FALSE; }
    *params = installed; return TRUE;
}
static BOOL TestSetParams(HDEVINFO set, SP_DEVINFO_DATA *device, SP_DEVINSTALL_PARAMS_W *params) {
    assert(set == (HDEVINFO)42 && (device->DevInst == 2 || device->DevInst == 3));
    if (failSet) { SetLastError(ERROR_GEN_FAILURE); return FALSE; }
    installed = *params; parameterSets++; return TRUE;
}
#define HeapAlloc TestAllocate
#define HeapFree TestFree
#define CreatePropertySheetPageW TestCreate
#define SetupDiOpenDevRegKey TestOpen
#define RegQueryValueExW TestQuery
#define RegSetValueExW TestWrite
#define RegCloseKey TestClose
#define SetupDiGetDeviceInstallParamsW TestGetParams
#define SetupDiSetDeviceInstallParamsW TestSetParams
#include "properties.c"

static BOOL CALLBACK AddPage(HPROPSHEETPAGE page, LPARAM cookie) { assert(cookie == 123); if (acceptPage) retained = page; return acceptPage; }
static void reset(void) {
    assert(opened == closed);
    ZeroMemory(storedLevels, sizeof(storedLevels)); storedType = REG_DWORD; storedSize = 4;
    readError = writeError = 0; failGet = failSet = readOnly = FALSE;
    ZeroMemory(&installed, sizeof(installed)); installed.cbSize = sizeof(installed); installed.FlagsEx = DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
    writes = parameterSets = 0;
}
static HWND dialog(HWND parent, FN_PROPERTY_CONTEXT *context) {
    PROPSHEETPAGEW page = {0}; HWND window; unsigned before = dirty;
    page.lParam = (LPARAM)context;
    window = CreateDialogParamW(FnPropertiesModule, MAKEINTRESOURCEW(IDD_ADVANCED), parent, FnPropertyDialog, (LPARAM)&page);
    assert(window && !IsWindowVisible(window) && dirty == before && opened == closed);
    assert(SendDlgItemMessageW(window, IDC_LOG_LEVEL, CB_GETCOUNT, 0, 0) == 5); return window;
}
static LONG_PTR apply(HWND window) {
    NMHDR header = {0}; header.code = PSN_APPLY; SendMessageW(window, WM_NOTIFY, 0, (LPARAM)&header);
    assert(opened == closed); return GetWindowLongPtrW(window, DWLP_MSGRESULT);
}
static void restore(HWND window) { SendMessageW(window, WM_COMMAND, MAKEWPARAM(IDC_RESTORE_DEFAULTS, BN_CLICKED), (LPARAM)GetDlgItem(window, IDC_RESTORE_DEFAULTS)); }
int main(void) {
    SP_DEVINFO_DATA device = {0}; SP_PROPSHEETPAGE_REQUEST request = {0};
    FN_PROPERTY_CONTEXT f2 = {0}, f3 = {0}; INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_WIN95_CLASSES};
    HWND parent, w2, w3; DWORD level; WCHAR text[128];
    assert(InitCommonControlsEx(&controls)); FnPropertiesModule = GetModuleHandleW(NULL);
    device.cbSize = sizeof(device); device.DevInst = 2;
    request.cbSize = sizeof(request); request.PageRequested = SPPSR_ENUM_ADV_DEVICE_PROPERTIES;
    request.DeviceInfoSet = (HDEVINFO)42; request.DeviceInfoData = &device;
    assert(!Ap6256FnPropPageProvider(NULL, AddPage, 123)); assert(!Ap6256FnPropPageProvider(&request, NULL, 123));
    request.PageRequested = 0; assert(!Ap6256FnPropPageProvider(&request, AddPage, 123)); request.PageRequested = SPPSR_ENUM_ADV_DEVICE_PROPERTIES;
    device.cbSize = 0; assert(!Ap6256FnPropPageProvider(&request, AddPage, 123)); device.cbSize = sizeof(device);
    failAllocate = TRUE; assert(!Ap6256FnPropPageProvider(&request, AddPage, 123)); failAllocate = FALSE;
    failCreate = TRUE; assert(!Ap6256FnPropPageProvider(&request, AddPage, 123)); failCreate = FALSE;
    assert(allocations == frees); acceptPage = FALSE; assert(!Ap6256FnPropPageProvider(&request, AddPage, 123)); assert(allocations == frees);
    acceptPage = TRUE; assert(Ap6256FnPropPageProvider(&request, AddPage, 123)); assert(allocations == frees + 1);
    assert(DestroyPropertySheetPage(retained) && allocations == frees);
    parent = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 600, 400, NULL, NULL, FnPropertiesModule, NULL); assert(parent);
    oldParent = (WNDPROC)SetWindowLongPtrW(parent, GWLP_WNDPROC, (LONG_PTR)TestParent);
    f2.DeviceInfoSet = (HDEVINFO)42; f2.DeviceInfoData = device; f3 = f2; f3.DeviceInfoData.DevInst = 3;
    for (level = 0; level <= 4; level++) {
        reset(); storedLevels[2] = level; w2 = dialog(parent, &f2);
        assert(f2.OriginalLevel == level); SendDlgItemMessageW(w2, IDC_LOG_LEVEL, CB_GETLBTEXT, level, (LPARAM)text); assert(!wcscmp(text, FnLogNames[level]));
        assert(apply(w2) == PSNRET_NOERROR && !writes && !parameterSets);
        SendDlgItemMessageW(w2, IDC_LOG_LEVEL, CB_SETCURSEL, (level + 1) % 5, 0);
        assert(apply(w2) == PSNRET_NOERROR && writes == 1 && storedLevels[2] == (level + 1) % 5);
        assert(installed.FlagsEx == (DI_FLAGSEX_ALLOWEXCLUDEDDRVS | DI_FLAGSEX_PROPCHANGE_PENDING)); DestroyWindow(w2);
    }
    reset(); w2 = dialog(parent, &f2); w3 = dialog(parent, &f3);
    SendDlgItemMessageW(w2, IDC_LOG_LEVEL, CB_SETCURSEL, 2, 0); assert(apply(w2) == PSNRET_NOERROR);
    SendDlgItemMessageW(w3, IDC_LOG_LEVEL, CB_SETCURSEL, 4, 0); assert(apply(w3) == PSNRET_NOERROR);
    assert(storedLevels[2] == 2 && storedLevels[3] == 4); restore(w2); assert(storedLevels[2] == 2 && writes == 2);
    assert(apply(w2) == PSNRET_NOERROR && storedLevels[2] == 0 && storedLevels[3] == 4); DestroyWindow(w2); DestroyWindow(w3);
    reset(); storedLevels[2] = 4; w2 = dialog(parent, &f2); restore(w2);
    { NMHDR cancel = {0}; cancel.code = PSN_RESET; SendMessageW(w2, WM_NOTIFY, 0, (LPARAM)&cancel); }
    assert(!writes && storedLevels[2] == 4); DestroyWindow(w2);
    reset(); storedLevels[2] = MAXDWORD; w2 = dialog(parent, &f2); assert(f2.OriginalLevel == 0); DestroyWindow(w2);
    reset(); storedType = REG_SZ; w2 = dialog(parent, &f2); assert(f2.OriginalLevel == 0); DestroyWindow(w2);
    reset(); storedSize = 3; w2 = dialog(parent, &f2); assert(f2.OriginalLevel == 0); DestroyWindow(w2);
    reset(); readError = ERROR_FILE_NOT_FOUND; w2 = dialog(parent, &f2); assert(f2.OriginalLevel == 0); DestroyWindow(w2);
    reset(); readOnly = TRUE; storedLevels[2] = 4; w2 = dialog(parent, &f2);
    assert(!IsWindowEnabled(GetDlgItem(w2, IDC_LOG_LEVEL)) && !IsWindowEnabled(GetDlgItem(w2, IDC_RESTORE_DEFAULTS)));
    restore(w2); assert(SendDlgItemMessageW(w2, IDC_LOG_LEVEL, CB_GETCURSEL, 0, 0) == 4); DestroyWindow(w2);
    reset(); w2 = dialog(parent, &f2); SendDlgItemMessageW(w2, IDC_LOG_LEVEL, CB_SETCURSEL, 1, 0);
    failGet = TRUE; assert(apply(w2) == PSNRET_INVALID_NOCHANGEPAGE && !writes); failGet = FALSE; failSet = TRUE;
    assert(apply(w2) == PSNRET_INVALID_NOCHANGEPAGE && !writes); failSet = FALSE; writeError = ERROR_ACCESS_DENIED;
    assert(apply(w2) == PSNRET_INVALID_NOCHANGEPAGE && !writes && installed.FlagsEx == DI_FLAGSEX_ALLOWEXCLUDEDDRVS);
    writeError = 0; assert(apply(w2) == PSNRET_NOERROR && writes == 1); DestroyWindow(w2);
    reset(); w2 = dialog(parent, &f2); SendDlgItemMessageW(w2, IDC_LOG_LEVEL, CB_SETCURSEL, (WPARAM)-1, 0);
    assert(apply(w2) == PSNRET_INVALID_NOCHANGEPAGE && !writes); DestroyWindow(w2); DestroyWindow(parent);
    assert(allocations == frees && opened == closed); puts("Companion property controls, independent functions, Apply/Cancel, safe defaults, access errors and lifetime passed"); return 0;
}
