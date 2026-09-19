// dbgcapture - captures OutputDebugString output, like DebugView's "Capture
// Win32", with no driver, no injection and no debugger attached.
//
// Why this exists: mxavcaacplug.dll narrates its own failures through
// OutputDebugString. Its string table contains lines such as
//
//     %s line %d: cuInit error:0x%x
//     %s line %d: cuCtxCreate error:0x%x
//     %s line %d: cuDeviceComputeCapability error:0x%x
//     %s line %d: nvEncGetEncodePresetConfig returned failure
//
// When VEGAS abandons a hardware render before making a single NVENC call, the
// reason is in that channel and nowhere else. This tool reads it.
//
// Usage:  dbgcapture.exe [output.log]
//         Ctrl+C to stop. Default output: dbgcapture.log in the current dir.
//
// Mechanism: OutputDebugString writes the caller's PID and text into a shared
// 4 KiB section named DBWIN_BUFFER and signals DBWIN_DATA_READY; the listener
// signals DBWIN_BUFFER_READY when it has consumed the slot. Only one listener
// can hold these objects at a time, so close DebugView or Visual Studio first.

#include <windows.h>
#include <stdio.h>

#pragma pack(push, 1)
struct dbwin_buffer {
    DWORD dwProcessId;
    char  data[4096 - sizeof(DWORD)];
};
#pragma pack(pop)

static volatile BOOL g_stop = FALSE;

static BOOL WINAPI on_ctrl(DWORD type)
{
    (void)type;
    g_stop = TRUE;
    return TRUE;
}

// Best-effort image name for a pid, so the log says who spoke.
static void image_name(DWORD pid, char *out, size_t cch)
{
    out[0] = '\0';
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return;
    char path[MAX_PATH];
    DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameA(h, 0, path, &n)) {
        const char *base = strrchr(path, '\\');
        snprintf(out, cch, "%s", base ? base + 1 : path);
    }
    CloseHandle(h);
}

int main(int argc, char **argv)
{
    const char *outPath = (argc > 1) ? argv[1] : "dbgcapture.log";

    SetConsoleCtrlHandler(on_ctrl, TRUE);

    // A NULL DACL lets processes at other integrity levels signal us, which
    // matters if VEGAS is ever launched elevated.
    SECURITY_DESCRIPTOR sd;
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), &sd, FALSE };

    HANDLE hReady  = CreateEventA(&sa, FALSE, FALSE, "DBWIN_BUFFER_READY");
    HANDLE hData   = CreateEventA(&sa, FALSE, FALSE, "DBWIN_DATA_READY");
    HANDLE hMap    = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                        0, sizeof(struct dbwin_buffer), "DBWIN_BUFFER");
    if (!hReady || !hData || !hMap) {
        printf("FATAL: could not create the DBWIN objects (err %lu).\n", GetLastError());
        printf("Another listener is probably running - close DebugView or Visual Studio.\n");
        return 2;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        printf("WARNING: DBWIN objects already existed; another listener may be active.\n");

    struct dbwin_buffer *buf =
        (struct dbwin_buffer *)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!buf) { printf("FATAL: MapViewOfFile failed (err %lu)\n", GetLastError()); return 2; }

    FILE *f = fopen(outPath, "w");
    if (!f) { printf("FATAL: cannot write %s\n", outPath); return 2; }

    printf("Capturing OutputDebugString -> %s\n", outPath);
    printf("Reproduce the problem now. Press Ctrl+C when done.\n\n");

    unsigned long count = 0;
    SetEvent(hReady);

    while (!g_stop) {
        DWORD w = WaitForSingleObject(hData, 300);
        if (w == WAIT_TIMEOUT) continue;
        if (w != WAIT_OBJECT_0) break;

        DWORD pid = buf->dwProcessId;
        char text[sizeof(buf->data) + 1];
        memcpy(text, buf->data, sizeof(buf->data));
        text[sizeof(buf->data)] = '\0';

        // Trim the trailing newline these messages usually carry.
        size_t len = strlen(text);
        while (len && (text[len - 1] == '\n' || text[len - 1] == '\r')) text[--len] = '\0';

        char name[MAX_PATH];
        image_name(pid, name, sizeof(name));

        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "%02d:%02d:%02d.%03d [%5lu %s] %s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                pid, name[0] ? name : "?", text);
        fflush(f);

        // Echo anything that looks like a failure, so the console is useful live.
        if (strstr(text, "error") || strstr(text, "Error") || strstr(text, "ERROR") ||
            strstr(text, "fail")  || strstr(text, "Fail")  || strstr(text, "FAIL") ||
            strstr(text, "cu")    || strstr(text, "nvEnc") || strstr(text, "NvEnc"))
            printf("  %s\n", text);

        count++;
        SetEvent(hReady);
    }

    printf("\nCaptured %lu message(s) -> %s\n", count, outPath);
    fclose(f);
    UnmapViewOfFile(buf);
    CloseHandle(hMap); CloseHandle(hData); CloseHandle(hReady);
    return 0;
}
