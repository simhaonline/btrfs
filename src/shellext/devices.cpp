/* Copyright (c) Mark Harmstone 2016
 * 
 * This file is part of WinBtrfs.
 * 
 * WinBtrfs is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public Licence as published by
 * the Free Software Foundation, either version 3 of the Licence, or
 * (at your option) any later version.
 * 
 * WinBtrfs is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public Licence for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public Licence
 * along with WinBtrfs.  If not, see <http://www.gnu.org/licenses/>. */

#define ISOLATION_AWARE_ENABLED 1
#define STRSAFE_NO_DEPRECATE

#include "shellext.h"
#include "devices.h"
#include "resource.h"
#include "../btrfsioctl.h"
#include "balance.h"
#include <uxtheme.h>
#include <stdio.h>
#include <strsafe.h>
#include <algorithm>

typedef struct _OBJECT_DIRECTORY_INFORMATION {
    UNICODE_STRING Name;
    UNICODE_STRING TypeName;
} OBJECT_DIRECTORY_INFORMATION, *POBJECT_DIRECTORY_INFORMATION;

#define DIRECTORY_QUERY         0x0001
#define DIRECTORY_TRAVERSE      0x0002

typedef NTSTATUS (NTAPI *pNtOpenDirectoryObject)(PHANDLE DirectoryHandle, ACCESS_MASK DesiredAccess, OBJECT_ATTRIBUTES* ObjectAttributes);

typedef NTSTATUS (NTAPI *pNtQueryDirectoryObject)(HANDLE DirectoryHandle, PVOID Buffer, ULONG Length, BOOLEAN ReturnSingleEntry, 
                                                  BOOLEAN RestartScan, PULONG Context, PULONG ReturnLength);

void BtrfsDeviceAdd::add_partition_to_tree(HWND tree, HTREEITEM parent, WCHAR* s, UINT32 partnum) {
    TVINSERTSTRUCTW tis;
    WCHAR t[255], u[255], *v;
    device_info di;
    
    if (!LoadStringW(module, partnum != 0 ? IDS_PARTITION : IDS_WHOLE_DISK, t, sizeof(t) / sizeof(WCHAR))) {
        ShowError(GetParent(tree), GetLastError());
        return;
    }
    
    if (partnum != 0) {
        if (StringCchPrintfW(u, sizeof(u) / sizeof(WCHAR), t, partnum) == STRSAFE_E_INSUFFICIENT_BUFFER)
            return;
        
        v = u;
    } else
        v = t;
    
    di.path = (WCHAR*)malloc((sizeof(WCHAR) * wcslen(s)) + sizeof(WCHAR));
    memcpy(di.path, s, (sizeof(WCHAR) * wcslen(s)) + sizeof(WCHAR));
    
    devpaths.push_back(di);
    
    tis.hParent = parent;
    tis.hInsertAfter = TVI_LAST;
    tis.itemex.mask = TVIF_TEXT | TVIF_PARAM;
    tis.itemex.pszText = v;
    tis.itemex.cchTextMax = wcslen(v);
    tis.itemex.lParam = (LPARAM)devpaths.size();
    
    SendMessageW(tree, TVM_INSERTITEMW, 0, (LPARAM)&tis);
}

void BtrfsDeviceAdd::add_device_to_tree(HWND tree, UNICODE_STRING* us) {
    NTSTATUS Status;
    OBJECT_ATTRIBUTES attr;
    HANDLE h;
    ULONG odisize, context;
    OBJECT_DIRECTORY_INFORMATION* odi;
    BOOL restart;
    TVINSERTSTRUCTW tis;
    HTREEITEM item;
    HMODULE ntdll;
    std::vector<int> parts;
    unsigned int i;
    
    pNtOpenDirectoryObject NtOpenDirectoryObject;
    pNtQueryDirectoryObject NtQueryDirectoryObject;
    
    static const WCHAR partition[] = L"Partition";
    
    ntdll = GetModuleHandleW(L"ntdll.dll");
    NtOpenDirectoryObject = (pNtOpenDirectoryObject)GetProcAddress(ntdll, "NtOpenDirectoryObject");
    NtQueryDirectoryObject = (pNtQueryDirectoryObject)GetProcAddress(ntdll, "NtQueryDirectoryObject");
    
    tis.hParent = TVI_ROOT;
    tis.hInsertAfter = TVI_LAST;
    tis.itemex.mask = TVIF_TEXT | TVIF_STATE | TVIF_PARAM;
    tis.itemex.state = TVIS_EXPANDED;
    tis.itemex.stateMask = TVIS_EXPANDED;
    tis.itemex.pszText = us->Buffer;
    tis.itemex.cchTextMax = us->Length / sizeof(WCHAR);
    tis.itemex.lParam = NULL;
    
    item = (HTREEITEM)SendMessageW(tree, TVM_INSERTITEMW, 0, (LPARAM)&tis);
    if (!item) {
        MessageBoxW(GetParent(tree), L"TVM_INSERTITEM failed", L"Error", MB_ICONERROR);
        return;
    }
    
    attr.Length = sizeof(attr);
    attr.RootDirectory = 0;
    attr.Attributes = OBJ_CASE_INSENSITIVE;
    attr.ObjectName = us;
    attr.SecurityDescriptor = NULL;
    attr.SecurityQualityOfService = NULL;

    Status = NtOpenDirectoryObject(&h, DIRECTORY_TRAVERSE | DIRECTORY_QUERY, &attr);

    if (!NT_SUCCESS(Status)) {
        char s[255];
        sprintf(s, "NtOpenDirectoryObject returned %08lx\n", Status);
        MessageBoxA(GetParent(tree), s, "Error", MB_ICONERROR);
        return;
    }
    
    odisize = sizeof(OBJECT_DIRECTORY_INFORMATION) * 16;
    odi = (OBJECT_DIRECTORY_INFORMATION*)malloc(odisize);
    if (!odi) {
        NtClose(h);
        MessageBoxA(GetParent(tree), "Out of memory", "Error", MB_ICONERROR);
        return;
    }
    
    parts.clear();
    
    restart = TRUE;
    do {
        Status = NtQueryDirectoryObject(h, odi, odisize, FALSE, restart, &context, NULL/*&retlen*/);
        restart = FALSE;
        
        if (NT_SUCCESS(Status)) {
            OBJECT_DIRECTORY_INFORMATION* odi2 = odi;
            
            while (odi2->Name.Buffer) {
                if (odi2->Name.Length > wcslen(partition) * sizeof(WCHAR) &&
                    RtlCompareMemory(odi2->Name.Buffer, partition, wcslen(partition) * sizeof(WCHAR)) == wcslen(partition) * sizeof(WCHAR)) {
                    WCHAR s[255];
                    int v;
                
                    memcpy(s, odi2->Name.Buffer + wcslen(partition), odi2->Name.Length - (wcslen(partition) * sizeof(WCHAR)));
                    s[(odi2->Name.Length / sizeof(WCHAR)) - wcslen(partition)] = 0;
                
                    v = _wtoi(s);
                
                    if (v != 0)
                        parts.push_back(v);
                }
                
                odi2 = &odi2[1];
            }
        }
    } while (NT_SUCCESS(Status));
    
    free(odi);
    NtClose(h);
    
    if (parts.size() == 0)
        parts.push_back(0);
    
    std::sort(parts.begin(), parts.end());
    
    for (i = 0; i < parts.size(); i++) {
        WCHAR n[255], *s;
        int len;
        
        wcscpy(n, partition);
        _itow(parts[i], n + wcslen(partition), 10);
        
        len = us->Length + sizeof(WCHAR) + (wcslen(n) * sizeof(WCHAR));
        s = (WCHAR*)malloc(len + sizeof(WCHAR));
        
        memcpy(s, us->Buffer, us->Length);
        s[us->Length / sizeof(WCHAR)] = '\\';
        memcpy((UINT8*)s + us->Length + sizeof(WCHAR), n, wcslen(n) * sizeof(WCHAR));
        s[len / sizeof(WCHAR)] = 0;
    
        add_partition_to_tree(tree, item, s, parts[i]);
        
        free(s);
    }
}

void BtrfsDeviceAdd::populate_device_tree(HWND tree) {
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES attr;
    NTSTATUS Status;
    HANDLE h;
    ULONG odisize, context;
    OBJECT_DIRECTORY_INFORMATION* odi;
    BOOL restart;
    HMODULE ntdll;
    
    pNtOpenDirectoryObject NtOpenDirectoryObject;
    pNtQueryDirectoryObject NtQueryDirectoryObject;
    
    static const WCHAR device[] = L"\\Device";
    static const WCHAR directory[] = L"Directory";
    static const WCHAR harddisk[] = L"Harddisk";
    
    ntdll = GetModuleHandleW(L"ntdll.dll");
    NtOpenDirectoryObject = (pNtOpenDirectoryObject)GetProcAddress(ntdll, "NtOpenDirectoryObject");
    NtQueryDirectoryObject = (pNtQueryDirectoryObject)GetProcAddress(ntdll, "NtQueryDirectoryObject");
    
    us.Buffer = (WCHAR*)device;
    us.Length = us.MaximumLength = wcslen(us.Buffer) * sizeof(WCHAR);
    
    attr.Length = sizeof(attr);
    attr.RootDirectory = 0;
    attr.Attributes = OBJ_CASE_INSENSITIVE;
    attr.ObjectName = &us;
    attr.SecurityDescriptor = NULL;
    attr.SecurityQualityOfService = NULL;

    Status = NtOpenDirectoryObject(&h, DIRECTORY_TRAVERSE | DIRECTORY_QUERY, &attr);

    if (!NT_SUCCESS(Status)) {
        char s[255];
        sprintf(s, "NtOpenDirectoryObject returned %08lx\n", Status);
        MessageBoxA(GetParent(tree), s, "Error", MB_ICONERROR);
        return;
    }
    
    odisize = sizeof(OBJECT_DIRECTORY_INFORMATION) * 16;
    odi = (OBJECT_DIRECTORY_INFORMATION*)malloc(odisize);
    if (!odi) {
        NtClose(h);
        MessageBoxA(GetParent(tree), "Out of memory", "Error", MB_ICONERROR);
        return;
    }
    
    restart = TRUE;
    do {
        Status = NtQueryDirectoryObject(h, odi, odisize, FALSE, restart, &context, NULL);
        restart = FALSE;
        
        if (NT_SUCCESS(Status)) {
            OBJECT_DIRECTORY_INFORMATION* odi2 = odi;
            
            while (odi2->Name.Buffer) {
                if (odi2->TypeName.Length == wcslen(directory) * sizeof(WCHAR) &&
                    RtlCompareMemory(odi2->TypeName.Buffer, directory, odi2->TypeName.Length) == odi2->TypeName.Length &&
                    odi2->Name.Length > wcslen(harddisk) * sizeof(WCHAR) &&
                    RtlCompareMemory(odi2->Name.Buffer, harddisk, wcslen(harddisk) * sizeof(WCHAR)) == wcslen(harddisk) * sizeof(WCHAR)) {
                        UNICODE_STRING us2;
                        
                        us2.Length = us2.MaximumLength = us.Length + sizeof(WCHAR) + odi2->Name.Length;
                        us2.Buffer = (WCHAR*)malloc(us2.Length + sizeof(WCHAR));
                        
                        memcpy(us2.Buffer, us.Buffer, us.Length);
                        us2.Buffer[us.Length / sizeof(WCHAR)] = '\\';
                        memcpy((UINT8*)us2.Buffer + us.Length + sizeof(WCHAR), odi2->Name.Buffer, odi2->Name.Length);
                        us2.Buffer[us2.Length / sizeof(WCHAR)] = 0;
                    
                        add_device_to_tree(tree, &us2);
                        
                        free(us2.Buffer);
                }
                
                odi2 = &odi2[1];
            }
        }
    } while (NT_SUCCESS(Status));
    
    free(odi);
    NtClose(h);
}

void BtrfsDeviceAdd::AddDevice(HWND hwndDlg) {
    NTSTATUS Status;
    UNICODE_STRING vn;
    OBJECT_ATTRIBUTES attr;
    IO_STATUS_BLOCK iosb;
    HANDLE h, h2;
    
    if (!sel) {
        EndDialog(hwndDlg, 0);
        return;
    }
    
    // FIXME - ask for confirmation
    
    h = CreateFileW(cmdline, FILE_TRAVERSE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    
    if (h == INVALID_HANDLE_VALUE) {
        ShowError(hwndDlg, GetLastError());
        return;
    }
    
    vn.Length = vn.MaximumLength = wcslen(sel->path) * sizeof(WCHAR);
    vn.Buffer = sel->path;

    InitializeObjectAttributes(&attr, &vn, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    Status = NtOpenFile(&h2, FILE_GENERIC_READ | FILE_GENERIC_WRITE, &attr, &iosb, FILE_SHARE_READ, FILE_SYNCHRONOUS_IO_ALERT);
    if (!NT_SUCCESS(Status)) {
        ShowNtStatusError(hwndDlg, Status);
        CloseHandle(h);
        return;
    }

    Status = NtFsControlFile(h2, NULL, NULL, NULL, &iosb, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0);
    if (!NT_SUCCESS(Status)) {
        ShowNtStatusError(hwndDlg, Status);
        NtClose(h2);
        CloseHandle(h);
        return;
    }

    Status = NtFsControlFile(h, NULL, NULL, NULL, &iosb, FSCTL_BTRFS_ADD_DEVICE, &h2, sizeof(HANDLE), NULL, 0);
    if (!NT_SUCCESS(Status)) {
        ShowNtStatusError(hwndDlg, Status);
        NtClose(h2);
        CloseHandle(h);
        return;
    }
    
    Status = NtFsControlFile(h2, NULL, NULL, NULL, &iosb, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0);
    if (!NT_SUCCESS(Status))
        ShowNtStatusError(hwndDlg, Status);
    
    Status = NtFsControlFile(h2, NULL, NULL, NULL, &iosb, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0);
    if (!NT_SUCCESS(Status))
        ShowNtStatusError(hwndDlg, Status);
    
    NtClose(h2);
    CloseHandle(h);
    
    EndDialog(hwndDlg, 0);
}

INT_PTR CALLBACK BtrfsDeviceAdd::DeviceAddDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_INITDIALOG:
        {
            EnableThemeDialogTexture(hwndDlg, ETDT_ENABLETAB);
            populate_device_tree(GetDlgItem(hwndDlg, IDC_DEVICE_TREE));
            EnableWindow(GetDlgItem(hwndDlg, IDOK), FALSE);
            break;
        }
        
        case WM_COMMAND:
            switch (HIWORD(wParam)) {
                case BN_CLICKED:
                    switch (LOWORD(wParam)) {
                        case IDOK:
                            AddDevice(hwndDlg);
                        return TRUE;
                        
                        case IDCANCEL:
                            EndDialog(hwndDlg, 0);
                        return TRUE;
                    }
                break;
            }
        break;
        
        case WM_NOTIFY:
            switch (((LPNMHDR)lParam)->code) {
                case TVN_SELCHANGEDW:
                {
                    NMTREEVIEWW* nmtv = (NMTREEVIEWW*)lParam;
                    TVITEMW tvi;
                    
                    RtlZeroMemory(&tvi, sizeof(TVITEMW));
                    tvi.hItem = nmtv->itemNew.hItem;
                    tvi.mask = TVIF_PARAM | TVIF_HANDLE;
                    
                    if (SendMessageW(GetDlgItem(hwndDlg, IDC_DEVICE_TREE), TVM_GETITEMW, 0, (LPARAM)&tvi))
                        sel = tvi.lParam == NULL ? NULL : &devpaths[tvi.lParam - 1];
                    else
                        sel = NULL;
                    
                    EnableWindow(GetDlgItem(hwndDlg, IDOK), sel ? TRUE : FALSE);
                    break;
                }
            }
        break;
    }
    
    return FALSE;
}

static INT_PTR CALLBACK stub_DeviceAddDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    BtrfsDeviceAdd* bda;
    
    if (uMsg == WM_INITDIALOG) {
        SetWindowLongPtr(hwndDlg, GWLP_USERDATA, (LONG_PTR)lParam);
        bda = (BtrfsDeviceAdd*)lParam;
    } else {
        bda = (BtrfsDeviceAdd*)GetWindowLongPtr(hwndDlg, GWLP_USERDATA);
    }
    
    if (bda)
        return bda->DeviceAddDlgProc(hwndDlg, uMsg, wParam, lParam);
    else
        return FALSE;
}

void BtrfsDeviceAdd::ShowDialog() {
    DialogBoxParamW(module, MAKEINTRESOURCEW(IDD_DEVICE_ADD), hwnd, stub_DeviceAddDlgProc, (LPARAM)this);
}

BtrfsDeviceAdd::BtrfsDeviceAdd(HINSTANCE hinst, HWND hwnd, WCHAR* cmdline) {
    this->hinst = hinst;
    this->hwnd = hwnd;
    this->cmdline = cmdline;
    
    sel = NULL;
}

#ifdef __cplusplus
extern "C" {
#endif

void CALLBACK AddDeviceW(HWND hwnd, HINSTANCE hinst, LPWSTR lpszCmdLine, int nCmdShow) {
    HANDLE token;
    TOKEN_PRIVILEGES tp;
    LUID luid;
    BtrfsDeviceAdd* bda;
    
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        ShowError(hwnd, GetLastError());
        return;
    }
    
    if (!LookupPrivilegeValueW(NULL, L"SeManageVolumePrivilege", &luid)) {
        ShowError(hwnd, GetLastError());
        return;
    }

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!AdjustTokenPrivileges(token, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL)) {
        ShowError(hwnd, GetLastError());
        return;
    }
    
    bda = new BtrfsDeviceAdd(hinst, hwnd, lpszCmdLine);
    bda->ShowDialog();
    delete bda;
}

void CALLBACK RemoveDeviceW(HWND hwnd, HINSTANCE hinst, LPWSTR lpszCmdLine, int nCmdShow) {
    WCHAR *s, *vol, *dev;
    UINT64 devid;
    HANDLE h, token;
    TOKEN_PRIVILEGES tp;
    LUID luid;
    NTSTATUS Status;
    IO_STATUS_BLOCK iosb;
    BtrfsBalance* bb;
    
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        ShowError(hwnd, GetLastError());
        return;
    }
    
    if (!LookupPrivilegeValueW(NULL, L"SeManageVolumePrivilege", &luid)) {
        ShowError(hwnd, GetLastError());
        return;
    }

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!AdjustTokenPrivileges(token, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL)) {
        ShowError(hwnd, GetLastError());
        return;
    }
    
    s = wcsstr(lpszCmdLine, L"|");
    if (!s)
        return;
    
    s[0] = 0;
    
    vol = lpszCmdLine;
    dev = &s[1];
    
    devid = _wtoi(dev);
    if (devid == 0)
        return;
    
    h = CreateFileW(vol, FILE_TRAVERSE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    
    if (h == INVALID_HANDLE_VALUE) {
        ShowError(hwnd, GetLastError());
        return;
    }
    
    Status = NtFsControlFile(h, NULL, NULL, NULL, &iosb, FSCTL_BTRFS_REMOVE_DEVICE, &devid, sizeof(UINT64), NULL, 0);
    if (!NT_SUCCESS(Status)) {
        if (Status == STATUS_CANNOT_DELETE)
            ShowStringError(hwnd, IDS_CANNOT_REMOVE_RAID);
        else
            ShowNtStatusError(hwnd, Status);
        
        CloseHandle(h);
        return;
    }
    
    CloseHandle(h);
    
    bb = new BtrfsBalance(vol);
    
    bb->ShowBalance(hwnd);
    
    delete bb;
}

#ifdef __cplusplus
}
#endif
