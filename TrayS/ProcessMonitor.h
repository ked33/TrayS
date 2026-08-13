#pragma once

#include <windows.h>

#define WM_PROCESS_MONITOR_UI (WM_APP + 20)

BOOL ProcessMonitorInitialize(HWND mainWindow, HINSTANCE instance, HICON appIcon, UINT trayIconId);
void ProcessMonitorShutdown();
void ProcessMonitorOpenRulesWindow(HWND owner);
void ProcessMonitorDispatchUi(BOOL trayIconAvailable);
BOOL ProcessMonitorIsDialogMessage(MSG* message);
