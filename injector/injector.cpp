#include "config.h"
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <tchar.h>
#include <cstdlib>
#include <cassert>
#include "../config.h"

BOOL DoCheckBits(HANDLE hProcess)
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);

    switch (info.wProcessorArchitecture)
    {
#ifdef _WIN64
    case PROCESSOR_ARCHITECTURE_AMD64:
    case PROCESSOR_ARCHITECTURE_IA64:
        return TRUE;
#else
    case PROCESSOR_ARCHITECTURE_INTEL:
        return TRUE;
#endif
    }
    return FALSE;
}

struct AutoCloseHandle
{
    HANDLE m_h;
    AutoCloseHandle(HANDLE h) : m_h(h)
    {
    }
    ~AutoCloseHandle()
    {
        CloseHandle(m_h);
    }
    operator HANDLE()
    {
        return m_h;
    }
};

BOOL DoInjectDLL(DWORD pid, LPCWSTR pszDllFile)
{
    AutoCloseHandle hProcess(OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid));
    if (!hProcess)
    {
        assert(0);
        return FALSE;
    }

    if (!DoCheckBits(hProcess))
    {
        assert(0);
        return FALSE;
    }

    DWORD cbParam = (lstrlenW(pszDllFile) + 1) * sizeof(WCHAR);
    LPVOID pParam = VirtualAllocEx(hProcess, NULL, cbParam, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!pParam)
    {
        assert(0);
        return FALSE;
    }

    WriteProcessMemory(hProcess, pParam, pszDllFile, cbParam, NULL);

    HMODULE hKernel32 = GetModuleHandle(TEXT("kernel32"));
    FARPROC pLoadLibraryW = GetProcAddress(hKernel32, "LoadLibraryW");
    if (!pLoadLibraryW)
    {
        assert(0);
        VirtualFreeEx(hProcess, pParam, cbParam, MEM_RELEASE);
        return FALSE;
    }

    AutoCloseHandle hThread(CreateRemoteThread(hProcess, NULL, 0,
        (LPTHREAD_START_ROUTINE)pLoadLibraryW, pParam, 0, NULL));
    if (!hThread)
    {
        assert(0);
        VirtualFreeEx(hProcess, pParam, cbParam, MEM_RELEASE);
        return FALSE;
    }

    WaitForSingleObject(hThread, INFINITE);

    VirtualFreeEx(hProcess, pParam, cbParam, MEM_RELEASE);
    return TRUE;
}

BOOL DoEnableProcessPriviledge(LPCTSTR pszSE_)
{
    BOOL f;
    HANDLE hToken;
    LUID luid;
    TOKEN_PRIVILEGES tp;
    
    f = FALSE;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hToken))
    {
        if (LookupPrivilegeValue(NULL, pszSE_, &luid))
        {
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            tp.Privileges[0].Luid = luid;
            f = AdjustTokenPrivileges(hToken, FALSE, &tp, 0, NULL, NULL);
        }
        CloseHandle(hToken);
    }
    
    return f;
}

BOOL DoGetProcessModuleInfo(LPMODULEENTRY32W pme, DWORD pid, LPCWSTR pszModule)
{
    MODULEENTRY32W me = { sizeof(me) };

    AutoCloseHandle hSnapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid));
    if (hSnapshot == INVALID_HANDLE_VALUE)
        return FALSE;

    if (Module32FirstW(hSnapshot, &me))
    {
        do
        {
            if (lstrcmpiW(me.szModule, pszModule) == 0)
            {
                *pme = me;
                CloseHandle(hSnapshot);
                return TRUE;
            }
        } while (Module32NextW(hSnapshot, &me));
    }

    return FALSE;
}

BOOL DoUninjectDLL(DWORD pid, LPCWSTR pszDllFile)
{
    AutoCloseHandle hProcess(OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid));
    assert(hProcess);
    if (!hProcess)
        return FALSE;

    if (!DoCheckBits(hProcess))
    {
        assert(0);
        return FALSE;
    }

    MODULEENTRY32W me;
    if (!DoGetProcessModuleInfo(&me, pid, PathFindFileNameW(pszDllFile)))
    {
        assert(0);
        return FALSE;
    }
    HMODULE hModule = me.hModule;

    HMODULE hNTDLL = GetModuleHandle(TEXT("ntdll"));
    FARPROC pLdrUnloadDll = GetProcAddress(hNTDLL, "LdrUnloadDll");
    if (!pLdrUnloadDll)
    {
        assert(0);
        return FALSE;
    }

    AutoCloseHandle hThread(CreateRemoteThread(hProcess, NULL, 0,
        (LPTHREAD_START_ROUTINE)pLdrUnloadDll, hModule, 0, NULL));
    if (!hThread)
    {
        assert(0);
        return FALSE;
    }

    WaitForSingleObject(hThread, INFINITE);

    return TRUE;
}

void OnInject(HWND hwnd, BOOL bInject)
{
    BOOL bTranslated = FALSE;
    DWORD pid = GetDlgItemInt(hwnd, edt1, &bTranslated, FALSE);
    if (!bTranslated)
    {
        MessageBoxW(hwnd, L"Invalid PID", NULL, MB_ICONERROR);
        return;
    }

    WCHAR szDllFile[MAX_PATH];
    GetModuleFileNameW(NULL, szDllFile, MAX_PATH);
    PathRemoveFileSpecW(szDllFile);
    PathAppendW(szDllFile, PAYLOAD_NAME L".dll");
    //MessageBoxW(NULL, szDllFile, NULL, 0);

    if (bInject)
    {
        DoInjectDLL(pid, szDllFile);
    }
    else
    {
        DoUninjectDLL(pid, szDllFile);
    }
}

BOOL OnInitDialog(HWND hwnd, HWND hwndFocus, LPARAM lParam)
{
    WCHAR szExeFile[MAX_PATH];
    GetModuleFileNameW(NULL, szExeFile, MAX_PATH);
    PathRemoveFileSpecW(szExeFile);
    PathAppendW(szExeFile, L"target.exe");

    SetDlgItemTextW(hwnd, edt2, szExeFile);
    return TRUE;
}

void OnBrowse(HWND hwnd)
{
    WCHAR szExeFile[MAX_PATH];
    GetDlgItemTextW(hwnd, edt2, szExeFile, MAX_PATH);

    OPENFILENAMEW ofn = { OPENFILENAME_SIZE_VERSION_400W };
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"EXE files (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = szExeFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_EXPLORER | OFN_ENABLESIZING | OFN_PATHMUSTEXIST |
                OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrTitle = L"Choose EXE file";
    ofn.lpstrDefExt = L"exe";
    if (GetOpenFileNameW(&ofn))
    {
        SetDlgItemTextW(hwnd, edt2, szExeFile);
    }
}

void OnRunWithInjection(HWND hwnd)
{
    WCHAR szExeFile[MAX_PATH], szParams[320];
    GetDlgItemTextW(hwnd, edt2, szExeFile, MAX_PATH);
    GetDlgItemTextW(hwnd, edt3, szParams, MAX_PATH);

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;

    LPWSTR parameters = _wcsdup(szParams);
    BOOL ret = CreateProcessW(szExeFile, parameters, NULL, NULL, TRUE,
                              CREATE_SUSPENDED, NULL, NULL, &si, &pi);
    std::free(parameters);

    if (!ret)
    {
        MessageBoxW(hwnd, L"Cannot startup process!", NULL, MB_ICONERROR);
        return;
    }


    SetDlgItemInt(hwnd, edt1, pi.dwProcessId, FALSE);

    WCHAR szDllFile[MAX_PATH];
    GetModuleFileNameW(NULL, szDllFile, MAX_PATH);
    PathRemoveFileSpecW(szDllFile);
    PathAppendW(szDllFile, PAYLOAD_NAME L".dll");
    //MessageBoxW(NULL, szDllFile, NULL, 0);

    DoInjectDLL(pi.dwProcessId, szDllFile);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

void OnCommand(HWND hwnd, int id, HWND hwndCtl, UINT codeNotify)
{
    switch (id)
    {
    case IDOK:
    case IDCANCEL:
        EndDialog(hwnd, id);
        break;
    case psh1:
        OnInject(hwnd, TRUE);
        break;
    case psh2:
        OnInject(hwnd, FALSE);
        break;
    case psh3:
        OnBrowse(hwnd);
        break;
    case psh4:
        OnRunWithInjection(hwnd);
        break;
    }
}

INT_PTR CALLBACK
DialogProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        HANDLE_MSG(hwnd, WM_INITDIALOG, OnInitDialog);
        HANDLE_MSG(hwnd, WM_COMMAND, OnCommand);
    }
    return 0;
}

INT WINAPI
WinMain(HINSTANCE   hInstance,
        HINSTANCE   hPrevInstance,
        LPSTR       lpCmdLine,
        INT         nCmdShow)
{
    DoEnableProcessPriviledge(SE_DEBUG_NAME);
    DialogBoxW(hInstance, MAKEINTRESOURCEW(1), NULL, DialogProc);
    return 0;
}
