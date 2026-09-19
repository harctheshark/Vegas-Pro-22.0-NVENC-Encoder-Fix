// nvenc_whichdll - resolves "nvEncodeAPI64.dll" the way VEGAS does and reports
// which file the loader actually picked.
//
// The shim only works if the application directory is searched before
// System32. This proves it for a given folder: drop this exe next to the shim
// and run it. If the printed path is the local copy, the patch will engage.

#include <windows.h>
#include <stdio.h>

int main(void)
{
    WCHAR self[MAX_PATH] = L"?";
    GetModuleFileNameW(NULL, self, MAX_PATH);
    wprintf(L"this executable : %s\n", self);

    HMODULE h = LoadLibraryW(L"nvEncodeAPI64.dll");   // exactly what VEGAS does
    if (!h) {
        wprintf(L"LoadLibraryW failed, err=%lu\n", GetLastError());
        return 2;
    }

    WCHAR resolved[MAX_PATH] = L"?";
    GetModuleFileNameW(h, resolved, MAX_PATH);
    wprintf(L"resolved to     : %s\n", resolved);

    WCHAR sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    int isSystem = (_wcsnicmp(resolved, sys, wcslen(sys)) == 0);

    wprintf(L"\n%s\n", isSystem
        ? L"=> System32 copy. A shim in this folder would NOT be used."
        : L"=> local copy wins. A shim placed here WILL be used.");

    FreeLibrary(h);
    return isSystem ? 1 : 0;
}
