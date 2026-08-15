#pragma once

#include <windows.h>

#define WM_PROCESS_MONITOR_UI (WM_APP + 20)
#define WM_PROCESS_MONITOR_NOTIFICATION (WM_APP + 21)

BOOL ProcessMonitorInitialize(HWND mainWindow, HINSTANCE instance, HICON appIcon, UINT trayIconId);
void ProcessMonitorShutdown();
void ProcessMonitorOpenRulesWindow(HWND owner);
void ProcessMonitorDispatchUi(BOOL trayIconAvailable);
BOOL ProcessMonitorHandleNotification(WPARAM wParam, LPARAM lParam);
BOOL ProcessMonitorIsDialogMessage(MSG* message);
