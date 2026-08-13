// TrayS.cpp : 定义应用程序的入口点。
//
#ifdef _WIN64
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")
#else
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='x86' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#include "framework.h"
#include "TrayS.h"
COLORREF oPixelColor;
HDC hDesktopDC=NULL;
int DPI(int pixel)
{
	return pixel * iDPI / 96;
}

COLORREF GetWindowPixel(HWND hWnd)
{
	RECT rc;
	GetWindowRect(hWnd, &rc);
	COLORREF c = GetPixel(hDesktopDC, rc.left+1, rc.top+=(rc.bottom-rc.top)/2);
	return c;
}
BOOL CALLBACK FindSettingWindowFunc(HWND hWnd, LPARAM lpAram)///////////查找TrayS主窗口防止重复开启
{
	WCHAR szText[16];
	GetWindowText(hWnd, szText, 16);
	if (lstrcmp(szText, L"_TrayS_") == 0)
	{
		SendMessage(hWnd, WM_TRAYS, 0, 0);
		ExitProcess(0);
		return FALSE;
	}
	return TRUE;
}
BOOL CALLBACK IsZoomedFunc(HWND hWnd, LPARAM lpAram)////是否有最大化窗口
{
	if (::IsWindowVisible(hWnd) && IsZoomed(hWnd))
	{
		if (MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST) == (HMONITOR)lpAram)
		{
			BOOL Attribute = FALSE;
			if (pDwmGetWindowAttribute)
				pDwmGetWindowAttribute(hWnd, 14, &Attribute, sizeof(BOOL));
			if (Attribute == FALSE)
			{
				iWindowMode = 1;
				return FALSE;
			}
		}
	}
	return TRUE;
}
/*
BOOL CreateProcessByExplorer(LPCWSTR process, LPCWSTR szDir, LPCWSTR cmd)
{
	BOOL ret = FALSE;

	HANDLE hProcess = 0, hToken = 0, hDuplicatedToken = 0;
	LPVOID lpEnv = NULL;
	do
	{
		HWND hTrayWnd = ::FindWindow(szShellTray, NULL);
		DWORD explorerPid;
		GetWindowThreadProcessId(hTrayWnd, &explorerPid);// 获取explorer进程号，自行实现

		if (explorerPid == NULL)
			break;
		hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, TRUE, explorerPid);
		if (INVALID_HANDLE_VALUE == hProcess)
			break;

		if (!OpenProcessToken(hProcess, TOKEN_ALL_ACCESS, &hToken))
			break;

		DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, NULL, SecurityIdentification, TokenPrimary, &hDuplicatedToken);
		CreateEnvironmentBlock(&lpEnv, hDuplicatedToken, FALSE);

		/ *
			WCHAR szDir[MAX_PATH] = L"\"";
		wcscpy_s(&szDir[2], MAX_PATH, process);
		int iLen = wcslen(szDir);
		if (NULL != cmd)
		{
			wcscpy_s(&szDir[iLen], MAX_PATH, L"\" \"");
			iLen = wcslen(szDir);
			wcscpy_s(&szDir[iLen], MAX_PATH, cmd);
		}
		iLen = wcslen(szDir);
		wcscpy_s(&szDir[iLen], MAX_PATH, L"\"");
		* /

			STARTUPINFO si = { 0 };
		PROCESS_INFORMATION pi = { 0 };
		si.cb = sizeof(STARTUPINFO);
		si.lpDesktop = (LPWSTR)L"winsta0\\default";
		si.dwFlags = STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_HIDE;
		if (!CreateProcessAsUser(hToken, process, NULL / *const_cast<LPWSTR>(szDir) * / , 0, 0, FALSE, CREATE_UNICODE_ENVIRONMENT, lpEnv, NULL, &si, &pi))
			break;
		ret = TRUE;
	} while (0);
	if (INVALID_HANDLE_VALUE != hProcess)
		CloseHandle(hProcess);
	if (INVALID_HANDLE_VALUE != hToken)
		CloseHandle(hToken);
	if (INVALID_HANDLE_VALUE != hDuplicatedToken)
		CloseHandle(hDuplicatedToken);
	if (NULL != lpEnv)
		DestroyEnvironmentBlock(lpEnv);
	return ret;
}

*/
#define CTL_CODE( DeviceType, Function, Method, Access ) (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))
#define IOCTL_STORAGE_GET_DEVICE_NUMBER       CTL_CODE(0x0000002d, 0x0420, 0, 0)

typedef struct _STORAGE_DEVICE_NUMBER {
	DWORD		DeviceType;
	DWORD       DeviceNumber;
	DWORD       PartitionNumber;
} STORAGE_DEVICE_NUMBER, * PSTORAGE_DEVICE_NUMBER;
DWORD GetPhysicalDriveFromPartitionLetter(WCHAR letter)//通过盘符获取物理硬盘位置
{
	HANDLE hDevice;
	DWORD readed;
	STORAGE_DEVICE_NUMBER number;
	WCHAR path[64];
	wsprintf(path, L"\\\\.\\%c:", letter);
	hDevice = CreateFile(path,GENERIC_READ | GENERIC_WRITE,FILE_SHARE_READ | FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
	if (hDevice != INVALID_HANDLE_VALUE) // cannot open the drive
	{
		if(DeviceIoControl(hDevice, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &number, sizeof(number), &readed, NULL))
		{		
			(void)CloseHandle(hDevice);
			return number.DeviceNumber;
		}
		CloseHandle(hDevice);
	}
	return DWORD(-1);
}

DWORD iCPU;
/////////////////////////////////////////////////////////////////////////////CPU占用率
FILETIME pre_idleTime;
FILETIME pre_kernelTime;
FILETIME pre_userTime;
__int64 CompareFileTime(FILETIME time1, FILETIME time2)
{
	__int64 a = time1.dwHighDateTime;
	a = a << 32 | time1.dwLowDateTime;
	__int64 b = time2.dwHighDateTime;
	b = b << 32 | time2.dwLowDateTime;
	return (b - a);
}
typedef struct _PDH_RAW_COUNTER {
	volatile DWORD CStatus;
	FILETIME    TimeStamp;
	LONGLONG    FirstValue;
	LONGLONG    SecondValue;
	DWORD       MultiCount;
} PDH_RAW_COUNTER, * PPDH_RAW_COUNTER;
PDH_RAW_COUNTER m_last_rawData;

#define PDH_FMT_RAW          ((DWORD) 0x00000010)
#define PDH_FMT_ANSI         ((DWORD) 0x00000020)
#define PDH_FMT_UNICODE      ((DWORD) 0x00000040)
#define PDH_FMT_LONG         ((DWORD) 0x00000100)
#define PDH_FMT_DOUBLE       ((DWORD) 0x00000200)
#define PDH_FMT_LARGE        ((DWORD) 0x00000400)
#define PDH_FMT_NOSCALE      ((DWORD) 0x00001000)
#define PDH_FMT_1000         ((DWORD) 0x00002000)
#define PDH_FMT_NODATA       ((DWORD) 0x00004000)
#define PDH_FMT_NOCAP100     ((DWORD) 0x00008000)
#define PERF_DETAIL_COSTLY   ((DWORD) 0x00010000)
#define PERF_DETAIL_STANDARD ((DWORD) 0x0000FFFF)
typedef HANDLE       PDH_HCOUNTER;
typedef HANDLE       PDH_HQUERY;
typedef HANDLE       PDH_HLOG;

typedef PDH_HCOUNTER HCOUNTER;
typedef PDH_HQUERY   HQUERY;

typedef struct _PDH_FMT_COUNTERVALUE {
	DWORD    CStatus;
	union {
		LONG        longValue;
		double      doubleValue;
		LONGLONG    largeValue;
		LPCSTR      AnsiStringValue;
		LPCWSTR     WideStringValue;
	};
} PDH_FMT_COUNTERVALUE, * PPDH_FMT_COUNTERVALUE;
#define PDH_CSTATUS_VALID_DATA ((DWORD)0x00000000)
#define PDH_CSTATUS_NEW_DATA   ((DWORD)0x00000001)
BOOL IsValidPdhValue(const PDH_FMT_COUNTERVALUE& value)
{
	return value.CStatus == PDH_CSTATUS_VALID_DATA || value.CStatus == PDH_CSTATUS_NEW_DATA;
}
HQUERY hQuery;
HCOUNTER hCounter;
HCOUNTER hDiskRead;
HCOUNTER hDiskWrite;
HCOUNTER hDiskTime;
DWORD counterType;
PDH_RAW_COUNTER rawData;
typedef ULONG(WINAPI* pfnPdhOpenQuery)(_In_opt_ LPCWSTR szDataSource, _In_ DWORD_PTR dwUserData, _Out_ PDH_HQUERY* phQuery);
typedef ULONG(WINAPI* pfnPdhAddCounter)(_In_ PDH_HQUERY hQuery, _In_ LPCWSTR szFullCounterPath, _In_ DWORD_PTR dwUserData, _Out_ PDH_HCOUNTER* phCounter);
typedef ULONG(WINAPI* pfnPdhCollectQueryData)(PDH_HQUERY hQuery);
//typedef ULONG(WINAPI* pfnPdhGetRawCounterValue)(PDH_HCOUNTER hCounter, LPDWORD lpdwType, PPDH_RAW_COUNTER pValue);
//typedef ULONG(WINAPI* pfnPdhCalculateCounterFromRawValue)(PDH_HCOUNTER hCounter, DWORD dwFormat, PPDH_RAW_COUNTER rawValue1, PPDH_RAW_COUNTER rawValue2, PPDH_FMT_COUNTERVALUE fmtValue);
typedef ULONG(WINAPI *pfnPdhRemoveCounter)(PDH_HCOUNTER hCounter);
typedef ULONG(WINAPI* pfnPdhCloseQuery)(PDH_HQUERY hQuery);
typedef ULONG(WINAPI* pfnPdhGetFormattedCounterValue)(PDH_HCOUNTER hCounter, DWORD dwFormat, LPDWORD lpdwType, PPDH_FMT_COUNTERVALUE pValue);
pfnPdhOpenQuery PdhOpenQuery;
pfnPdhAddCounter PdhAddCounter;
pfnPdhCollectQueryData PdhCollectQueryData;
pfnPdhRemoveCounter PdhRemoveCounter;
//pfnPdhGetRawCounterValue PdhGetRawCounterValue;
//pfnPdhCalculateCounterFromRawValue PdhCalculateCounterFromRawValue;
pfnPdhCloseQuery PdhCloseQuery;
pfnPdhGetFormattedCounterValue PdhGetFormattedCounterValue;
void LockMonitorBackend()
{
	if (bMonitorBackendLockInitialized)
		EnterCriticalSection(&MonitorBackendLock);
}
void UnlockMonitorBackend()
{
	if (bMonitorBackendLockInitialized)
		LeaveCriticalSection(&MonitorBackendLock);
}
void LockMonitorData()
{
	if (bMonitorDataLockInitialized)
		EnterCriticalSection(&MonitorDataLock);
}
void UnlockMonitorData()
{
	if (bMonitorDataLockInitialized)
		LeaveCriticalSection(&MonitorDataLock);
}
void PublishMonitorSettings()
{
	LockMonitorData();
	MonitorSettings = TraySave;
	UnlockMonitorData();
}
void ReadMonitorDataSnapshot(TRAYDATA* monitorData, MEMORYSTATUSEX* memoryStatus, DWORD* cpuUsage)
{
	LockMonitorData();
	if (monitorData)
		*monitorData = MonitorDataSnapshot;
	if (memoryStatus)
		*memoryStatus = MemoryStatusSnapshot;
	if (cpuUsage)
		*cpuUsage = CpuUsageSnapshot;
	UnlockMonitorData();
}
void ReadMonitorSettings(TRAYSAVE* settings)
{
	if (!settings)
		return;
	LockMonitorData();
	*settings = MonitorSettings;
	UnlockMonitorData();
}
int ReadTrafficSnapshot(TRAFFIC* trafficData, int trafficCapacity)
{
	LockMonitorData();
	int trafficCount = nTrafficSnapshot;
	if (trafficData && trafficCapacity > 0)
	{
		if (trafficCount > trafficCapacity)
			trafficCount = trafficCapacity;
		if (trafficCount > 0)
			CopyMemory(trafficData, TrafficSnapshot, trafficCount * sizeof(TRAFFIC));
	}
	else if (trafficData)
	{
		trafficCount = 0;
	}
	UnlockMonitorData();
	return trafficCount;
}
int ReadTaskTipsSnapshot(
	TRAFFIC* trafficData,
	int trafficCapacity,
	PROCESSMEMORYUSAGE* memoryProcesses,
	PROCESSCPUUSAGE* cpuProcesses,
	MEMORYSTATUSEX* memoryStatus,
	TRAYSAVE* settings)
{
	LockMonitorData();
	int trafficCount = nTrafficSnapshot;
	if (trafficCapacity < 0)
		trafficCapacity = 0;
	if (trafficCount > trafficCapacity)
		trafficCount = trafficCapacity;
	if (trafficData && trafficCount > 0)
		CopyMemory(trafficData, TrafficSnapshot, trafficCount * sizeof(TRAFFIC));
	if (memoryProcesses)
		CopyMemory(memoryProcesses, pmu, sizeof(pmu));
	if (cpuProcesses)
		CopyMemory(cpuProcesses, pcu, sizeof(pcu));
	if (memoryStatus)
		*memoryStatus = MemoryStatusSnapshot;
	if (settings)
		*settings = MonitorSettings;
	UnlockMonitorData();
	return trafficCount;
}
void RefreshMonitorSnapshot()
{
	LockMonitorData();
	MonitorDataSnapshot = *TrayData;
	MemoryStatusSnapshot = MemoryStatusEx;
	CpuUsageSnapshot = iCPU;
	nTrafficSnapshot = nTraffic;
	if (nTrafficSnapshot < 0)
		nTrafficSnapshot = 0;
	if (nTrafficSnapshot > MAX_TRAFFIC_ADAPTERS)
		nTrafficSnapshot = MAX_TRAFFIC_ADAPTERS;
	ZeroMemory(TrafficSnapshot, sizeof(TrafficSnapshot));
	if (nTrafficSnapshot > 0 && traffic)
		CopyMemory(TrafficSnapshot, traffic, nTrafficSnapshot * sizeof(TRAFFIC));
	for (int i = 0; i < 6; ++i)
	{
		pmu[i] = *ppmuWork[i];
		pcu[i] = *ppcuWork[i];
	}
	UnlockMonitorData();
}
void ResetTrafficMonitorData()
{
	LockMonitorBackend();
	TrayData->m_last_in_bytes = 0;
	TrayData->m_last_out_bytes = 0;
	TrayData->s_in_byte = 0;
	TrayData->s_out_byte = 0;
	RefreshMonitorSnapshot();
	UnlockMonitorBackend();
}
void ResetDiskMonitorData()
{
	LockMonitorBackend();
	TrayData->diskreadbyte = 0;
	TrayData->diskwritebyte = 0;
	TrayData->disktime = 0;
	RefreshMonitorSnapshot();
	UnlockMonitorBackend();
}
void ClosePDHUnlocked()
{
	if (PdhRemoveCounter)
	{
		if (hCounter)
			PdhRemoveCounter(hCounter);
		if (hDiskRead)
			PdhRemoveCounter(hDiskRead);
		if (hDiskWrite)
			PdhRemoveCounter(hDiskWrite);
		if (hDiskTime)
			PdhRemoveCounter(hDiskTime);
	}
	hCounter = NULL;
	hDiskRead = NULL;
	hDiskWrite = NULL;
	hDiskTime = NULL;
	if (hQuery && PdhCloseQuery)
		PdhCloseQuery(hQuery);
	hQuery = NULL;
	if (hPDH)
		FreeLibrary(hPDH);
	hPDH = NULL;
	PdhOpenQuery = NULL;
	PdhAddCounter = NULL;
	PdhCollectQueryData = NULL;
	PdhRemoveCounter = NULL;
	PdhCloseQuery = NULL;
	PdhGetFormattedCounterValue = NULL;
}
void SwitchPDH(BOOL bOn)
{
	TRAYSAVE settings;
	LockMonitorData();
	settings = MonitorSettings;
	UnlockMonitorData();
	LockMonitorBackend();
	if (bOn)
	{
		BOOL pdhReady =
			hPDH &&
			hQuery &&
			(!settings.bMonitorPDH || hCounter) &&
			(!settings.bMonitorDisk || hDiskRead || hDiskWrite || hDiskTime);
		if (pdhReady)
		{
			UnlockMonitorBackend();
			return;
		}
		ClosePDHUnlocked();
		hPDH = LoadLibrary(L"pdh.dll");
		if (hPDH)
		{
			PdhOpenQuery = (pfnPdhOpenQuery)GetProcAddress(hPDH, "PdhOpenQueryW");
			PdhAddCounter = (pfnPdhAddCounter)GetProcAddress(hPDH, "PdhAddEnglishCounterW");
			if (!PdhAddCounter)
				PdhAddCounter = (pfnPdhAddCounter)GetProcAddress(hPDH, "PdhAddCounterW");
			PdhCollectQueryData = (pfnPdhCollectQueryData)GetProcAddress(hPDH, "PdhCollectQueryData");
//			PdhGetRawCounterValue = (pfnPdhGetRawCounterValue)GetProcAddress(hPDH, "PdhGetRawCounterValue");
//			PdhCalculateCounterFromRawValue = (pfnPdhCalculateCounterFromRawValue)GetProcAddress(hPDH, "PdhCalculateCounterFromRawValue");
			PdhRemoveCounter = (pfnPdhRemoveCounter)GetProcAddress(hPDH, "PdhRemoveCounter");
			PdhGetFormattedCounterValue = (pfnPdhGetFormattedCounterValue)GetProcAddress(hPDH, "PdhGetFormattedCounterValue");
			PdhCloseQuery = (pfnPdhCloseQuery)GetProcAddress(hPDH, "PdhCloseQuery");
			if (PdhOpenQuery && PdhAddCounter && PdhCollectQueryData && PdhRemoveCounter && PdhCloseQuery && PdhGetFormattedCounterValue)
			{
				if (PdhOpenQuery(NULL, 0, &hQuery) == ERROR_SUCCESS)
				{
					const wchar_t* cpuquery_str;
					if (rovi.dwMajorVersion >= 10)
						cpuquery_str = L"\\Processor Information(_Total)\\% Processor Utility";
					else
						cpuquery_str = L"\\Processor Information(_Total)\\% Processor Time";
					if (PdhAddCounter(hQuery, cpuquery_str, 0, &hCounter) != ERROR_SUCCESS)
					{
						hCounter = NULL;
						if (rovi.dwMajorVersion >= 10 &&
							PdhAddCounter(hQuery, L"\\Processor Information(_Total)\\% Processor Time", 0, &hCounter) != ERROR_SUCCESS)
							hCounter = NULL;
					}

					if (settings.bMonitorDisk && settings.szDisk == L'\0')
					{
						if (PdhAddCounter(hQuery, L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec", 0, &hDiskRead) != ERROR_SUCCESS)
							hDiskRead = NULL;
						if (PdhAddCounter(hQuery, L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec", 0, &hDiskWrite) != ERROR_SUCCESS)
							hDiskWrite = NULL;
						if (PdhAddCounter(hQuery, L"\\PhysicalDisk(_Total)\\% Idle Time", 0, &hDiskTime) != ERROR_SUCCESS)
							hDiskTime = NULL;
					}
					else if (settings.bMonitorDisk)
					{
						WCHAR sz[256];
						DWORD d = GetPhysicalDriveFromPartitionLetter(settings.szDisk);
						if (d != DWORD(-1))
						{
							wsprintf(sz, L"\\PhysicalDisk(%d %c:)\\Disk Read Bytes/sec", d, settings.szDisk);
							if (PdhAddCounter(hQuery, sz, 0, &hDiskRead) != ERROR_SUCCESS)
								hDiskRead = NULL;
							wsprintf(sz, L"\\PhysicalDisk(%d %c:)\\Disk Write Bytes/sec", d, settings.szDisk);
							if (PdhAddCounter(hQuery, sz, 0, &hDiskWrite) != ERROR_SUCCESS)
								hDiskWrite = NULL;
							wsprintf(sz, L"\\PhysicalDisk(%d %c:)\\%% Idle Time", d, settings.szDisk);
							if (PdhAddCounter(hQuery, sz, 0, &hDiskTime) != ERROR_SUCCESS)
								hDiskTime = NULL;
						}
					}
					if ((!settings.bMonitorPDH || hCounter) &&
						(!settings.bMonitorDisk || hDiskRead || hDiskWrite || hDiskTime))
					{
						PdhCollectQueryData(hQuery);
						UnlockMonitorBackend();
						return;
					}
				}
			}
		}
	}
	ClosePDHUnlocked();
	UnlockMonitorBackend();
}
int GetPDH(BOOL bCPU, BOOL bDisk)
{
	BOOL needsInit;
	LockMonitorBackend();
	needsInit = hPDH == NULL || hQuery == NULL || (bCPU && !hCounter) || (bDisk && !hDiskRead && !hDiskWrite && !hDiskTime);
	UnlockMonitorBackend();
	if (needsInit)
		SwitchPDH(TRUE);
	LockMonitorBackend();
	BOOL cpuValid = !bCPU;
	if (hQuery && PdhCollectQueryData && PdhCollectQueryData(hQuery) == ERROR_SUCCESS)
	{
		PDH_FMT_COUNTERVALUE pdhValue = { 0 };
		DWORD dwValue = 0;
		if (bCPU)
		{
			if (hCounter && PdhGetFormattedCounterValue(hCounter, PDH_FMT_LONG, &dwValue, &pdhValue) == ERROR_SUCCESS && IsValidPdhValue(pdhValue))
			{
				if (pdhValue.longValue >= 0)
				{
					iCPU = pdhValue.longValue >= 100 ? 99 : pdhValue.longValue;
					cpuValid = TRUE;
				}
			}
		}
		if (bDisk)
		{
			BOOL diskReadValid = FALSE;
			BOOL diskWriteValid = FALSE;
			BOOL diskTimeValid = FALSE;
			if (hDiskRead && PdhGetFormattedCounterValue(hDiskRead, PDH_FMT_DOUBLE, &dwValue, &pdhValue) == ERROR_SUCCESS &&
				IsValidPdhValue(pdhValue) && pdhValue.doubleValue >= 0.0)
			{
				TrayData->diskreadbyte = pdhValue.doubleValue;
				diskReadValid = TRUE;
			}
			if (hDiskWrite && PdhGetFormattedCounterValue(hDiskWrite, PDH_FMT_DOUBLE, &dwValue, &pdhValue) == ERROR_SUCCESS &&
				IsValidPdhValue(pdhValue) && pdhValue.doubleValue >= 0.0)
			{
				TrayData->diskwritebyte = pdhValue.doubleValue;
				diskWriteValid = TRUE;
			}
			if (hDiskTime && PdhGetFormattedCounterValue(hDiskTime, PDH_FMT_LONG, &dwValue, &pdhValue) == ERROR_SUCCESS &&
				IsValidPdhValue(pdhValue) && pdhValue.longValue >= 0 && pdhValue.longValue <= 100)
			{
				TrayData->disktime = 100 - pdhValue.longValue;
				if (TrayData->disktime >= 100)
					TrayData->disktime = 99;
				diskTimeValid = TRUE;
			}
			if (!diskReadValid)
				TrayData->diskreadbyte = 0;
			if (!diskWriteValid)
				TrayData->diskwritebyte = 0;
			if (!diskTimeValid)
				TrayData->disktime = 0;
		}
	}
	else if (bDisk)
	{
		TrayData->diskreadbyte = 0;
		TrayData->diskwritebyte = 0;
		TrayData->disktime = 0;
	}
	if (!cpuValid)
	{
		UnlockMonitorBackend();
		return -1;
	}
	UnlockMonitorBackend();
	return iCPU;
}
int GetCPUUseRate(const TRAYSAVE& settings)
{
	if (settings.bMonitorPDH)
	{
		int pdhRate = GetPDH(TRUE, settings.bMonitorDisk);
		if (pdhRate >= 0)
			return pdhRate;
	}
	else if (settings.bMonitorDisk)
	{
		GetPDH(FALSE, TRUE);
	}
	if (hPDH && !settings.bMonitorDisk && !settings.bMonitorPDH)
	{
		SwitchPDH(FALSE);
	}
	int nCPUUseRate = -1;
	FILETIME idleTime;//空闲时间
	FILETIME kernelTime;//核心态时间
	FILETIME userTime;//用户态时间
	if (GetSystemTimes(&idleTime, &kernelTime, &userTime))
	{
		__int64 idle = CompareFileTime(pre_idleTime, idleTime);
		__int64 kernel = CompareFileTime(pre_kernelTime, kernelTime);
		__int64 user = CompareFileTime(pre_userTime, userTime);
		if (kernel + user > 0 && kernel + user >= idle)
			nCPUUseRate = (int)((kernel + user - idle) * 100 / (kernel + user));
		pre_idleTime = idleTime;
		pre_kernelTime = kernelTime;
		pre_userTime = userTime;
	}
	if (nCPUUseRate < 1)
		nCPUUseRate = iCPU;
	else if (nCPUUseRate >= 100)
		nCPUUseRate = 99;
	return nCPUUseRate;
}
typedef struct _CONFIG_FILE_HEADER
{
	DWORD magic;
	DWORD formatVersion;
	DWORD payloadVersion;
	DWORD payloadSize;
	DWORD checksum;
} CONFIG_FILE_HEADER;

typedef struct _TRAYSAVE_V116
{
	BYTE prefix[656];
	BYTE removedV116Fields[808];
	BOOL bTrayStyle;
} TRAYSAVE_V116;
static_assert(FIELD_OFFSET(TRAYSAVE, bTrayStyle) == 656, "Unexpected TRAYSAVE layout");
static_assert(sizeof(TRAYSAVE_V116) == 1468, "Unexpected v116 configuration layout");

const DWORD CONFIG_MAGIC = 0x53595254; // TRYS
const DWORD CONFIG_FORMAT_VERSION = 1;
const DWORD CONFIG_PAYLOAD_VERSION = 117;
const WCHAR szTraySaveTemp[] = L"TrayS.dat.tmp";

DWORD CalculateConfigChecksum(const BYTE* data, DWORD size)
{
	DWORD crc = 0xffffffff;
	for (DWORD i = 0; i < size; ++i)
	{
		crc ^= data[i];
		for (int bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ (0xedb88320 & (0 - (crc & 1)));
	}
	return ~crc;
}

BOOL IsConfigBool(BOOL value)
{
	return value == FALSE || value == TRUE;
}

BOOL HasAnsiTerminator(const CHAR* value, size_t count)
{
	for (size_t i = 0; i < count; ++i)
		if (value[i] == '\0')
			return TRUE;
	return FALSE;
}

BOOL HasWideTerminator(const WCHAR* value, size_t count)
{
	for (size_t i = 0; i < count; ++i)
		if (value[i] == L'\0')
			return TRUE;
	return FALSE;
}

void WriteReg();

BOOL ValidateConfig(const TRAYSAVE& config)
{
	if (config.Ver != CONFIG_PAYLOAD_VERSION || config.FlushTime < 100 || config.FlushTime > 5000)
		return FALSE;
	if (config.iPos < 0 || config.iPos > 2 || LOWORD(config.iUnit) > 2 || HIWORD(config.iUnit) > 1)
		return FALSE;
	if (config.iMonitorSimple < 0 || config.iMonitorSimple > 2)
		return FALSE;
	for (int i = 0; i < 2; ++i)
	{
		if (config.aMode[i] != ACCENT_DISABLED && config.aMode[i] != ACCENT_ENABLE_TRANSPARENTGRADIENT &&
			config.aMode[i] != ACCENT_ENABLE_BLURBEHIND && config.aMode[i] != ACCENT_ENABLE_ACRYLICBLURBEHIND)
			return FALSE;
		if (config.bAlpha[i] > 255)
			return FALSE;
	}
	const BOOL values[] = {
		config.bSound, config.bTrayIcon, config.bMonitor, config.bMonitorLeft,
		config.bMonitorFloat, config.bMonitorTransparent, config.bMonitorTraffic,
		config.bMonitorTemperature, config.bMonitorUsage, config.bMonitorPDH,
		config.bMonitorTips, config.bMonitorFuse, config.bMonitorTrafficUpDown,
		config.bMonitorFloatVRow, config.bMonitorTime, config.bSecond, config.bNear,
		config.bMonitorTopmost, config.bMonitorDisk, config.bTrayStyle
	};
	for (int i = 0; i < ARRAYSIZE(values); ++i)
		if (!IsConfigBool(values[i]))
			return FALSE;
	if (!HasAnsiTerminator(config.AdpterName, ARRAYSIZE(config.AdpterName)) ||
		!HasWideTerminator(config.TraybarFont.lfFaceName, ARRAYSIZE(config.TraybarFont.lfFaceName)) ||
		!HasWideTerminator(config.TipsFont.lfFaceName, ARRAYSIZE(config.TipsFont.lfFaceName)))
		return FALSE;
	const WCHAR* strings[] = {
		config.szTrafficOut, config.szTrafficIn, config.szTemperatureCPU,
		config.szTemperatureCPUUnit, config.szTemperatureGPU, config.szTemperatureGPUUnit,
		config.szUsageCPU, config.szUsageCPUUnit, config.szUsageMEM, config.szUsageMEMUnit,
		config.szDiskReadSec, config.szDiskWriteSec, config.szDiskName
	};
	const size_t stringSizes[] = { 8, 8, 8, 4, 8, 4, 8, 4, 8, 4, 8, 8, 8 };
	for (int i = 0; i < ARRAYSIZE(strings); ++i)
		if (!HasWideTerminator(strings[i], stringSizes[i]))
			return FALSE;
	return TRUE;
}

void ReadReg()//读取设置
{

	if (rovi.dwBuildNumber >= 22000)
	{
		TraySave.aMode[1] = ACCENT_ENABLE_TRANSPARENTGRADIENT;
		TraySave.aMode[0] = ACCENT_ENABLE_TRANSPARENTGRADIENT;
//		TraySave.bAlpha[0] = 188;
//		TraySave.bAlpha[1] = 208;
	}

	SetToCurrentPath();
	HANDLE hFile = CreateFile(szTraySave, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_ARCHIVE, NULL);
	if (hFile!= INVALID_HANDLE_VALUE)
	{
		DWORD fileSize = GetFileSize(hFile, NULL);
		DWORD dwBytes = 0;
		TRAYSAVE candidate = TraySave;
		BOOL valid = FALSE;
		BOOL migrateLegacy = FALSE;
		if (fileSize == sizeof(CONFIG_FILE_HEADER) + sizeof(TRAYSAVE))
		{
			CONFIG_FILE_HEADER header = { 0 };
			if (ReadFile(hFile, &header, sizeof(header), &dwBytes, NULL) && dwBytes == sizeof(header) &&
				header.magic == CONFIG_MAGIC && header.formatVersion == CONFIG_FORMAT_VERSION &&
				header.payloadVersion == CONFIG_PAYLOAD_VERSION && header.payloadSize == sizeof(candidate) &&
				ReadFile(hFile, &candidate, sizeof(candidate), &dwBytes, NULL) && dwBytes == sizeof(candidate) &&
				header.checksum == CalculateConfigChecksum((const BYTE*)&candidate, sizeof(candidate)))
			{
				valid = ValidateConfig(candidate);
			}
		}
		else if (fileSize == sizeof(TRAYSAVE_V116))
		{
			TRAYSAVE_V116 legacy = { 0 };
			DWORD legacyVersion = 0;
			if (ReadFile(hFile, &legacy, sizeof(legacy), &dwBytes, NULL) && dwBytes == sizeof(legacy))
			{
				CopyMemory(&legacyVersion, legacy.prefix, sizeof(legacyVersion));
				if (legacyVersion == 116)
				{
					CopyMemory(&candidate, legacy.prefix, sizeof(legacy.prefix));
					candidate.Ver = CONFIG_PAYLOAD_VERSION;
					candidate.bTrayStyle = legacy.bTrayStyle;
					if (candidate.FlushTime < 100 || candidate.FlushTime > 5000)
						candidate.FlushTime = 250;
					valid = ValidateConfig(candidate);
					migrateLegacy = valid;
				}
			}
		}
		CloseHandle(hFile);
		if (valid)
		{
			TraySave = candidate;
			if (migrateLegacy)
				WriteReg();
		}
	}
}
void WriteReg()//写入设置
{
	SetToCurrentPath();
	TraySave.Ver = CONFIG_PAYLOAD_VERSION;
	if (TraySave.FlushTime < 100)
		TraySave.FlushTime = 100;
	else if (TraySave.FlushTime > 5000)
		TraySave.FlushTime = 5000;
	if (!ValidateConfig(TraySave))
		return;
	PublishMonitorSettings();
	CONFIG_FILE_HEADER header = {
		CONFIG_MAGIC,
		CONFIG_FORMAT_VERSION,
		CONFIG_PAYLOAD_VERSION,
		sizeof(TraySave),
		CalculateConfigChecksum((const BYTE*)&TraySave, sizeof(TraySave))
	};
	HANDLE hFile = CreateFile(szTraySaveTemp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_ARCHIVE, NULL);
	if (hFile!= INVALID_HANDLE_VALUE)
	{
		DWORD dwBytes = 0;
		BOOL written = WriteFile(hFile, &header, sizeof(header), &dwBytes, NULL) && dwBytes == sizeof(header);
		written = written && WriteFile(hFile, &TraySave, sizeof TraySave, &dwBytes, NULL) && dwBytes == sizeof(TraySave);
		written = written && FlushFileBuffers(hFile);
		CloseHandle(hFile);
		if (!written || !MoveFileEx(szTraySaveTemp, szTraySave, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			DeleteFile(szTraySaveTemp);
	}
}
BOOL GetShellAllWnd()
{
	hReBarWnd = NULL;
	hStartWnd = NULL;
	hTrayNotifyWnd = NULL;
	hTaskWnd = NULL;
	hTaskListWnd = NULL;
	hWin11UI = NULL;
	hTrayClockWnd = NULL;
	hTray = FindWindow(szShellTray, NULL);
	if (!IsWindow(hTray))
		return FALSE;
	hReBarWnd = FindWindowEx(hTray, 0, L"ReBarWindow32", NULL);
	hStartWnd = FindWindowEx(hTray, 0, L"Start", NULL);
	hTrayNotifyWnd = FindWindowEx(hTray, 0, L"TrayNotifyWnd", NULL);
	if(hReBarWnd)
		hTaskWnd = FindWindowEx(hReBarWnd, NULL, L"MSTaskSwWClass", NULL);
	if(hTaskWnd)
		hTaskListWnd = FindWindowEx(hTaskWnd, NULL, L"MSTaskListWClass", NULL);
	if(!hTaskListWnd)
		hTaskListWnd = FindWindowEx(hTaskWnd, NULL, L"ToolbarWindow32", NULL);
	hWin11UI = FindWindowEx(hTray, 0, L"Windows.UI.Composition.DesktopWindowContentBridge", NULL);
	if(hTrayNotifyWnd)
		hTrayClockWnd = FindWindowEx(hTrayNotifyWnd, NULL, L"TrayClockWClass", NULL);
/*
	if (hWin11UI)
	{
		if (TraySave.cMonitorColor[0] == 0 && !TraySave.bMonitorFloat)
			TraySave.cMonitorColor[0] = RGB(1, 2, 3);
	}
*/
	if (IsWindow(hWin11UI))
		return IsWindow(hStartWnd) || IsWindow(hTaskListWnd) || IsWindow(hTrayNotifyWnd);
	return IsWindow(hReBarWnd) && IsWindow(hTaskWnd) && IsWindow(hTaskListWnd);
}
BOOL RestoreShellIntegration()
{
	if (!GetShellAllWnd())
		return FALSE;
	APPBARDATA abd = { 0 };
	abd.cbSize = sizeof(abd);
	abd.hWnd = hMain;
	abd.uCallbackMessage = MSG_APPBAR_MSGID;
	pSHAppBarMessage(ABM_NEW, &abd);
	if (TraySave.bTrayIcon)
		pShell_NotifyIcon(NIM_ADD, &nid);
	CloseTaskBar();
	SetWH();
	if (TraySave.bMonitor)
		AdjustWindowPos();
	return TRUE;
}
void UpdateMainRefreshTimer()
{
	if (!IsWindow(hMain))
		return;
	KillTimer(hMain, 3);
	if (TraySave.bMonitor)
		SetTimer(hMain, 3, TraySave.FlushTime, NULL);
	else if (TraySave.bTrayStyle)
		SetTimer(hMain, 3, 250, NULL);
}
void CloseTaskBar()
{
	if (IsWindow(hTaskBar))
		DestroyWindow(hTaskBar);
	if (IsWindow(hTaskTips))
		DestroyWindow(hTaskTips);
	if(IsWindow(hTime))
		DestroyWindow(hTime);	
}
void OpenTimeDlg()
{
	if (!IsWindow(hTime) && TraySave.bSecond)
	{
		
		if (!hWin11UI)
		{
			hTime = ::CreateDialog(hInst, MAKEINTRESOURCE(IDD_TIME), NULL, (DLGPROC)TimeProc);
			if (hTrayClockWnd)
				SetParent(hTime, hTrayClockWnd);
		}
		else
		{
			hTime = ::CreateDialog(hInst, MAKEINTRESOURCE(IDD_TIME), NULL, (DLGPROC)TimeProc);
			SetWindowLongPtr(hTime, GWL_EXSTYLE, GetWindowLongPtr(hTime, GWL_EXSTYLE) | WS_EX_LAYERED|WS_EX_TRANSPARENT);
/*
			if(bThemeMode&&rovi.dwBuildNumber>22000)
				SetLayeredWindowAttributes(hTime, RGB(254,254,255), 0, LWA_COLORKEY);
			else
*/
				SetLayeredWindowAttributes(hTime, RGB(0, 0, 1), 0, LWA_COLORKEY);
			SetParent(hTime, hTray);
		}
		ShowWindow(hTime, SW_SHOW);
	}
}

void OpenTaskBar()
{
	if (IsWindow(hTaskBar) == FALSE)
	{		
		if (TraySave.cMonitorColor[0] == RGB(1, 2, 3))
			TraySave.cMonitorColor[0] = RGB(0,0,1);
		hTaskBar = ::CreateDialog(hInst, MAKEINTRESOURCE(IDD_TASKBAR), NULL, (DLGPROC)TaskBarProc);
		if (hTaskBar)
		{			
			if (TraySave.bMonitorFloat||bFullScreen||(rovi.dwBuildNumber<=25000&&TraySave.bTrayStyle)||(rovi.dwMajorVersion==6&&rovi.dwMinorVersion==1))
			{
				if (TraySave.cMonitorColor[0] == RGB(0,0,1) || TraySave.cMonitorColor[0] == 0)
				{
					bShadow = TRUE;
					TraySave.bMonitorFuse = TRUE;
				}
				else
					bShadow = FALSE;
			}
			else
			{
				bShadow = FALSE;
				TraySave.bMonitorFuse = FALSE;
				if (TraySave.cMonitorColor[0] == 0)
					TraySave.cMonitorColor[0] = RGB(0, 0, 1);
				SetWindowLongPtr(hTaskBar, GWL_EXSTYLE, GetWindowLongPtr(hTaskBar, GWL_EXSTYLE) | WS_EX_LAYERED);
				SetLayeredWindowAttributes(hTaskBar, GetWindowPixel(hTray), 0, LWA_COLORKEY);

/*
					if (bThemeMode)
						SetLayeredWindowAttributes(hTaskBar, RGB(222, 222, 223), 128, LWA_COLORKEY | LWA_ALPHA);
					else
						SetLayeredWindowAttributes(hTaskBar, RGB(0, 0, 1), 128, LWA_COLORKEY | LWA_ALPHA);
*/				
//				else
//					SetParent(hTaskBar, GetForegroundWindow());
			}
			if(!TraySave.bMonitorFloat&&!bFullScreen)
				SetParent(hTaskBar, hTray);
			SetWH();
			if (!TraySave.bMonitorFuse && TraySave.bMonitorFloat)
			{
			}
			else
				SetWindowCompositionAttribute(hTaskBar, ACCENT_ENABLE_TRANSPARENT, 0x00111111);
			ShowWindow(hTaskBar, SW_SHOW);
			if (TraySave.bMonitorTransparent&&TraySave.bMonitorFloat)
				SetWindowLongPtr(hTaskBar, GWL_EXSTYLE, GetWindowLongPtr(hTaskBar, GWL_EXSTYLE) |WS_EX_LAYERED |WS_EX_TRANSPARENT);
			SetTimer(hTaskBar, 3, 1000, NULL);
			//			SetTimer(hTaskBar, 6, 100, NULL);
		}
	}
}
////////////////////////////////////////////获取CPU温度
#define MISC_CONTROL_3 0x3+((0x18)<<3)
WCHAR oldDisk=L'\0';
int nDisk = -1;
int GetCpuTemp(DWORD Core, WCHAR disk)
{
	if (hOHMA)
	{
		float fCpu,fHdd,fGpu,fCpuPackge;
		if (disk != oldDisk)
		{
			nDisk = disk == L'\0' ? -1 : GetPhysicalDriveFromPartitionLetter(disk);
			oldDisk = disk;
		}
		GetTemperature(&fCpu,&fGpu,NULL,&fHdd,nDisk,&fCpuPackge);
		TrayData->iHddTemperature = (int)fHdd;
		if (fGpu != -1 && fGpu != 0)
			TrayData->iTemperature2 = (int)fGpu;
		else
			TrayData->iTemperature2 = (int)fCpuPackge;
		return (int)fCpu;
	}
	else
	{
		if (bRing0)
		{
			SetThreadAffinityMask(GetCurrentThread(), Core);
			DWORD eax = 0, ebx, ecx, edx;
			if (!bIntel)//老的AMD_CPU
			{
				Cpuid(1, &eax, &ebx, &ecx, &edx);
				int family = ((eax >> 20) & 0xFF) + ((eax >> 8) & 0xF);
				if (family > 0xf)
				{
					//				DWORD pciDevAddr = FindPciDeviceById(0x1022, 0x1203, 0);
					DWORD miscReg;
					ReadPciConfigDwordEx(MISC_CONTROL_3, 0xa4, &miscReg);
					return (miscReg >> 21) >> 3;
				}
				else
				{
					//				DWORD pciDevAddr = FindPciDeviceById(0x1022, 0x1103, 0);
					DWORD miscReg;
					ReadPciConfigDwordEx(MISC_CONTROL_3, 0xe4, &miscReg);
					return ((miscReg & 0xFF0000) >> 16) - 49;
					//				return (miscReg >> 16) & 0xFF;
				}
			}
			else//INTEL_CPU
			{
				DWORD IAcore;
				int Tjunction = 100;
				Rdmsr(0x1A2, &eax, &edx);
				if (eax & 0x20000000)
					Tjunction = 85;
				Rdmsr(0x19C, &eax, &edx);
				IAcore = eax;
				IAcore &= 0xFF0000;
				IAcore = IAcore >> 16;
				return Tjunction - IAcore;
			}
		}
	}
	return 0;
}
//////////////////////////////////////////////////载入温度DLL
void LoadTemperatureDLL()
{
	LockMonitorBackend();
	if (hOHMA || m_hOpenLibSys || hNVDLL || hATIDLL)
	{
		UnlockMonitorBackend();
		return;
	}
	hOHMA = LoadLibrary(L"OpenHardwareMonitorApi.dll");
	if (hOHMA)
	{
		
		GetTemperature = (pfnGetTemperature) GetProcAddress(hOHMA, "GetTemperature");
		float fCpu=-1;
		if (GetTemperature)
		{
			float fHdd,fGpu,fCpuPackge;
			GetTemperature(&fCpu, &fGpu, NULL,  &fHdd,-1,&fCpuPackge);
		}
		if(fCpu != -1)
			bRing0 = TRUE;
		else
		{
			FreeLibrary(hOHMA);
			hOHMA = NULL;
		}
	}
	if(hOHMA==NULL)
	{
		if (!InitOpenLibSys(&m_hOpenLibSys))
			bRing0 = FALSE;
		else
		{
			bRing0 = TRUE;
			DWORD eax, ebx, ecx, edx;
			Cpuid(0, &eax, &ebx, &ecx, &edx);
			bIntel = TRUE;
			if (ebx == 0x68747541)
			{
				bIntel = FALSE;
			}
		}
	}
#ifdef _WIN64
	hNVDLL = LoadLibrary(L"nvapi64.dll");
#else
	hNVDLL = LoadLibrary(L"nvapi.dll");
#endif
	if (hNVDLL)
	{
		NvAPI_QueryInterface = (NvAPI_QueryInterface_t)GetProcAddress(hNVDLL, "nvapi_QueryInterface");
		if (NvAPI_QueryInterface)
		{
			NvAPI_Initialize_t NvAPI_Initialize = (NvAPI_Initialize_t)NvAPI_QueryInterface(ID_NvAPI_Initialize);
			NvAPI_EnumPhysicalGPUs_t NvAPI_EnumPhysicalGPUs = (NvAPI_EnumPhysicalGPUs_t)NvAPI_QueryInterface(ID_NvAPI_EnumPhysicalGPUs);
			NvAPI_GPU_GetThermalSettings = (NvAPI_GPU_GetThermalSettings_t)NvAPI_QueryInterface(ID_NvAPI_GPU_GetThermalSettings);
			if (NvAPI_Initialize != NULL && NvAPI_EnumPhysicalGPUs != NULL && NvAPI_GPU_GetThermalSettings != NULL)
			{
				if (NvAPI_Initialize() == 0)
				{
					for (NvU32 PhysicalGpuIndex = 0; PhysicalGpuIndex < NVAPI_MAX_PHYSICAL_GPUS; PhysicalGpuIndex++)
					{
						hPhysicalGpu[PhysicalGpuIndex] = 0;
					}
					NvU32 physicalGpuCount = 0;
					if (NvAPI_EnumPhysicalGPUs(hPhysicalGpu, &physicalGpuCount) != 0 || physicalGpuCount <= 0)
					{
						FreeLibrary(hNVDLL);
						hNVDLL = NULL;
						NvAPI_GPU_GetThermalSettings = NULL;
					}
					else
						nPhysicalGpu = physicalGpuCount > NVAPI_MAX_PHYSICAL_GPUS ? NVAPI_MAX_PHYSICAL_GPUS : physicalGpuCount;
				}
				else
				{
					FreeLibrary(hNVDLL);
					hNVDLL = NULL;
				}
			}
			else
			{
				FreeLibrary(hNVDLL);
				hNVDLL = NULL;
			}
		}
		else
		{
			FreeLibrary(hNVDLL);
			hNVDLL = NULL;
		}
	}
#ifdef _WIN64
	hATIDLL = LoadLibrary(L"atiadlxx.dll");
#else
	hATIDLL = LoadLibrary(L"atiadlxy.dll");
#endif
	if (hATIDLL)
	{
		ADL_Main_Control_Create = (ADL_MAIN_CONTROL_CREATE)GetProcAddress(hATIDLL, "ADL_Main_Control_Create");
		ADL_Main_Control_Destroy = (ADL_MAIN_CONTROL_DESTROY)GetProcAddress(hATIDLL, "ADL_Main_Control_Destroy");
		ADL_Overdrive5_Temperature_Get = (ADL_OVERDRIVE5_TEMPERATURE_GET)GetProcAddress(hATIDLL, "ADL_Overdrive5_Temperature_Get");
		if (NULL != ADL_Main_Control_Create && NULL != ADL_Main_Control_Destroy && NULL != ADL_Overdrive5_Temperature_Get)
		{
			if (ADL_OK != ADL_Main_Control_Create(ADL_Main_Memory_Alloc, 1))
			{
				FreeLibrary(hATIDLL);
				hATIDLL = NULL;
			}
		}
		else
		{
			FreeLibrary(hATIDLL);
			hATIDLL = NULL;
		}
	}
	UnlockMonitorBackend();
}
///////////////////////////////////释放温度DLL
void FreeTemperatureDLL()
{
	LockMonitorBackend();
	if (hATIDLL)
	{
		if (ADL_Main_Control_Destroy)
			ADL_Main_Control_Destroy();
		FreeLibrary(hATIDLL);
		hATIDLL = NULL;
	}
	ADL_Main_Control_Create = NULL;
	ADL_Main_Control_Destroy = NULL;
	ADL_Overdrive5_Temperature_Get = NULL;
	if (hNVDLL)
	{
		FreeLibrary(hNVDLL);
		hNVDLL = NULL;
	}
	nPhysicalGpu = 0;
	NvAPI_QueryInterface = NULL;
	NvAPI_GPU_GetThermalSettings = NULL;
	if (hOHMA)
	{
		FreeLibrary(hOHMA);
		hOHMA = NULL;
	}
	GetTemperature = NULL;
	if (m_hOpenLibSys)
		DeinitOpenLibSys(&m_hOpenLibSys);
	m_hOpenLibSys = NULL;
	bRing0 = FALSE;
	UnlockMonitorBackend();
}
///////////////////////////////////////////////打开读取设置
void OpenSetting()
{
	if (IsWindow(hSetting))
	{
		SetForegroundWindow(hSetting);
		return;
	}
	hSetting = ::CreateDialog(hInst, MAKEINTRESOURCE(IDD_SETTING), NULL, (DLGPROC)SettingProc);
	if (!hSetting)
	{
		return;
	}
	SendMessage(hSetting, WM_SETICON, ICON_BIG, (LPARAM)(HICON)iMain);
	SendMessage(hSetting, WM_SETICON, ICON_SMALL, (LPARAM)(HICON)iMain);
	CheckRadioButton(hSetting, IDC_RADIO_NORMAL, IDC_RADIO_MAXIMIZE, IDC_RADIO_NORMAL);
	iProject = iWindowMode;
	if (iProject == 0)
		CheckRadioButton(hSetting, IDC_RADIO_NORMAL, IDC_RADIO_MAXIMIZE, IDC_RADIO_NORMAL);
	else
		CheckRadioButton(hSetting, IDC_RADIO_NORMAL, IDC_RADIO_MAXIMIZE, IDC_RADIO_MAXIMIZE);
	if (TraySave.aMode[iProject] == ACCENT_DISABLED)
		CheckRadioButton(hSetting, IDC_RADIO_DEFAULT, IDC_RADIO_ACRYLIC, IDC_RADIO_DEFAULT);
	else if (TraySave.aMode[iProject] == ACCENT_ENABLE_TRANSPARENTGRADIENT)
		CheckRadioButton(hSetting, IDC_RADIO_DEFAULT, IDC_RADIO_ACRYLIC, IDC_RADIO_TRANSPARENT);
	else if (TraySave.aMode[iProject] == ACCENT_ENABLE_BLURBEHIND)
		CheckRadioButton(hSetting, IDC_RADIO_DEFAULT, IDC_RADIO_ACRYLIC, IDC_RADIO_BLURBEHIND);
	else if (TraySave.aMode[iProject] == ACCENT_ENABLE_ACRYLICBLURBEHIND)
		CheckRadioButton(hSetting, IDC_RADIO_DEFAULT, IDC_RADIO_ACRYLIC, IDC_RADIO_ACRYLIC);
	if (TraySave.iPos == 0)
		CheckRadioButton(hSetting, IDC_RADIO_LEFT, IDC_RADIO_RIGHT, IDC_RADIO_LEFT);
	else if (TraySave.iPos == 1)
		CheckRadioButton(hSetting, IDC_RADIO_LEFT, IDC_RADIO_RIGHT, IDC_RADIO_CENTER);
	else if (TraySave.iPos == 2)
		CheckRadioButton(hSetting, IDC_RADIO_LEFT, IDC_RADIO_RIGHT, IDC_RADIO_RIGHT);
	if (hWin11UI)
	{
		EnableWindow(GetDlgItem(hSetting, IDC_RADIO_LEFT), FALSE);
		EnableWindow(GetDlgItem(hSetting, IDC_RADIO_CENTER), FALSE);
		EnableWindow(GetDlgItem(hSetting, IDC_RADIO_RIGHT), FALSE);
//		EnableWindow(GetDlgItem(hSetting, IDC_CHECK_TOPMOST), TRUE);
	}
	if (LOWORD(TraySave.iUnit) == 0)
		CheckRadioButton(hSetting, IDC_RADIO_AUTO, IDC_RADIO_MB, IDC_RADIO_AUTO);
	else if (LOWORD(TraySave.iUnit) == 1)
		CheckRadioButton(hSetting, IDC_RADIO_AUTO, IDC_RADIO_MB, IDC_RADIO_KB);
	else if (LOWORD(TraySave.iUnit) == 2)
		CheckRadioButton(hSetting, IDC_RADIO_AUTO, IDC_RADIO_MB, IDC_RADIO_MB);
	if (HIWORD(TraySave.iUnit) == 0)
		CheckRadioButton(hSetting, IDC_RADIO_BYTE, IDC_RADIO_BIT, IDC_RADIO_BYTE);
	else
		CheckRadioButton(hSetting, IDC_RADIO_BYTE, IDC_RADIO_BIT, IDC_RADIO_BIT);
	CheckDlgButton(hSetting, IDC_CHECK_TRAYICON, TraySave.bTrayIcon);
	CheckDlgButton(hSetting, IDC_CHECK_MONITOR, TraySave.bMonitor);
	CheckDlgButton(hSetting, IDC_CHECK_TRAY_STYLE, TraySave.bTrayStyle);
	CheckDlgButton(hSetting, IDC_CHECK_TRAFFIC, TraySave.bMonitorTraffic);
	CheckDlgButton(hSetting, IDC_CHECK_MONITOR_UPDOWN, TraySave.bMonitorTrafficUpDown);
	CheckDlgButton(hSetting, IDC_CHECK_TEMPERATURE, TraySave.bMonitorTemperature);
	CheckDlgButton(hSetting, IDC_CHECK_USAGE, TraySave.bMonitorUsage);
	CheckDlgButton(hSetting, IDC_CHECK_DISK, TraySave.bMonitorDisk);
	CheckDlgButton(hSetting, IDC_CHECK_SOUND, TraySave.bSound);
	CheckDlgButton(hSetting, IDC_CHECK_MONITOR_PDH, TraySave.bMonitorPDH);
	CheckDlgButton(hSetting, IDC_CHECK_MONITOR_SIMPLE, TraySave.iMonitorSimple);
	CheckDlgButton(hSetting, IDC_CHECK_MONITOR_LEFT, TraySave.bMonitorLeft);
	CheckDlgButton(hSetting, IDC_CHECK_MONITOR_NEAR, TraySave.bNear);
	CheckDlgButton(hSetting, IDC_CHECK_MONITOR_FLOAT, TraySave.bMonitorFloat);
	CheckDlgButton(hSetting, IDC_CHECK_MONITOR_FLOAT_VROW, TraySave.bMonitorFloatVRow);
	CheckDlgButton(hSetting, IDC_CHECK_MONITOR_TIME, TraySave.bMonitorTime);
	CheckDlgButton(hSetting, IDC_CHECK_TIME, TraySave.bSecond);
	CheckDlgButton(hSetting, IDC_CHECK_TRANSPARENT, TraySave.bMonitorTransparent);
	CheckDlgButton(hSetting, IDC_CHECK_TIPS, TraySave.bMonitorTips);
	CheckDlgButton(hSetting, IDC_CHECK_FUSE, TraySave.bMonitorFuse);
	CheckDlgButton(hSetting, IDC_CHECK_TOPMOST, TraySave.bMonitorTopmost);
	SendDlgItemMessage(hSetting, IDC_SLIDER_ALPHA, TBM_SETRANGE, 0, MAKELPARAM(0, 255));
	SendDlgItemMessage(hSetting, IDC_SLIDER_ALPHA, TBM_SETPOS, TRUE, TraySave.bAlpha[iProject]);
	SendDlgItemMessage(hSetting, IDC_SLIDER_ALPHA_B, TBM_SETRANGE, 0, MAKELPARAM(0, 255));
	BYTE bAlphaB = TraySave.dAlphaColor[iProject] >> 24;
	SendDlgItemMessage(hSetting, IDC_SLIDER_ALPHA_B, TBM_SETPOS, TRUE, bAlphaB);
	SendDlgItemMessage(hSetting, IDC_CHECK_AUTORUN, BM_SETCHECK, AutoRun(FALSE, FALSE, szAppName), NULL);
	bSettingInit = TRUE;
	SetDlgItemInt(hSetting, IDC_EDIT1, TraySave.dNumValues[0] / 1048576, 0);
	SetDlgItemInt(hSetting, IDC_EDIT2, TraySave.dNumValues[1] / 1048576, 0);
	SetDlgItemInt(hSetting, IDC_EDIT3, TraySave.dNumValues[2], 0);
	SetDlgItemInt(hSetting, IDC_EDIT4, TraySave.dNumValues[3], 0);
	SetDlgItemInt(hSetting, IDC_EDIT5, TraySave.dNumValues[4], 0);
	SetDlgItemInt(hSetting, IDC_EDIT6, TraySave.dNumValues[5], 0);
	SetDlgItemInt(hSetting, IDC_EDIT7, TraySave.dNumValues[6], 0);
	SetDlgItemInt(hSetting, IDC_EDIT8, TraySave.dNumValues[7], 0);
	SetDlgItemInt(hSetting, IDC_EDIT9, TraySave.dNumValues[8] / 1048576, 0);
	SetDlgItemInt(hSetting, IDC_EDIT10, TraySave.dNumValues[9], 0);
	SetDlgItemInt(hSetting, IDC_EDIT11, TraySave.dNumValues[10], 0);
	SetDlgItemInt(hSetting, IDC_EDIT12, TraySave.dNumValues[11], 0);
	SetDlgItemInt(hSetting, IDC_EDIT24, TraySave.dNumValues2[0], 0);
	SetDlgItemInt(hSetting, IDC_EDIT25, TraySave.dNumValues2[1], 0);
	SetDlgItemInt(hSetting, IDC_EDIT26, TraySave.dNumValues2[2], 0);
	SetDlgItemInt(hSetting, IDC_EDIT_TIME, TraySave.FlushTime, 0);
	SetDlgItemText(hSetting, IDC_EDIT14, TraySave.szTrafficOut);
	SetDlgItemText(hSetting, IDC_EDIT15, TraySave.szTrafficIn);
	SetDlgItemText(hSetting, IDC_EDIT16, TraySave.szTemperatureCPU);
	SetDlgItemText(hSetting, IDC_EDIT17, TraySave.szTemperatureGPU);
	SetDlgItemText(hSetting, IDC_EDIT18, TraySave.szTemperatureCPUUnit);
	SetDlgItemText(hSetting, IDC_EDIT19, TraySave.szTemperatureGPUUnit);
	SetDlgItemText(hSetting, IDC_EDIT20, TraySave.szUsageCPU);
	SetDlgItemText(hSetting, IDC_EDIT21, TraySave.szUsageMEM);
	SetDlgItemText(hSetting, IDC_EDIT22, TraySave.szUsageCPUUnit);
	SetDlgItemText(hSetting, IDC_EDIT23, TraySave.szUsageMEMUnit);
	SetDlgItemText(hSetting, IDC_EDIT27, TraySave.szDiskReadSec);
	SetDlgItemText(hSetting, IDC_EDIT28, TraySave.szDiskWriteSec);
	SetDlgItemText(hSetting, IDC_EDIT29, TraySave.szDiskName);
	bSettingInit = FALSE;
	oldColorButtonPoroc = (WNDPROC)SetWindowLongPtr(GetDlgItem(hSetting, IDC_BUTTON_COLOR), GWLP_WNDPROC, (LONG_PTR)ColorButtonProc);
	oldColorButtonPoroc = (WNDPROC)SetWindowLongPtr(GetDlgItem(hSetting, IDC_BUTTON_COLOR_BACKGROUND), GWLP_WNDPROC, (LONG_PTR)ColorButtonProc);
	oldColorButtonPoroc = (WNDPROC)SetWindowLongPtr(GetDlgItem(hSetting, IDC_BUTTON_COLOR_TRAFFIC_LOW), GWLP_WNDPROC, (LONG_PTR)ColorButtonProc);
	oldColorButtonPoroc = (WNDPROC)SetWindowLongPtr(GetDlgItem(hSetting, IDC_BUTTON_COLOR_TRAFFIC_MEDIUM), GWLP_WNDPROC, (LONG_PTR)ColorButtonProc);
	oldColorButtonPoroc = (WNDPROC)SetWindowLongPtr(GetDlgItem(hSetting, IDC_BUTTON_COLOR_TRAFFIC_HIGH), GWLP_WNDPROC, (LONG_PTR)ColorButtonProc);
	oldColorButtonPoroc = (WNDPROC)SetWindowLongPtr(GetDlgItem(hSetting, IDC_BUTTON_COLOR_LOW), GWLP_WNDPROC, (LONG_PTR)ColorButtonProc);
	oldColorButtonPoroc = (WNDPROC)SetWindowLongPtr(GetDlgItem(hSetting, IDC_BUTTON_COLOR_MEDUIM), GWLP_WNDPROC, (LONG_PTR)ColorButtonProc);
	oldColorButtonPoroc = (WNDPROC)SetWindowLongPtr(GetDlgItem(hSetting, IDC_BUTTON_COLOR_HIGH), GWLP_WNDPROC, (LONG_PTR)ColorButtonProc);	
	ShowWindow(hSetting, SW_SHOW);
	UpdateWindow(hSetting);
	SetForegroundWindow(hSetting);
}

#ifndef _DEBUG
extern "C" void WinMainCRTStartup()
{
	LPWSTR lpCmdLine;
#else
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);

/*
	if (lpCmdLine[0] == L'c')////打开控制面板
	{
		CloseHandle(pShellExecute(NULL, L"open", L"control.exe", &lpCmdLine[1], NULL, SW_SHOW));
		return 0;
	}
	else if (lpCmdLine[0] == L'o')//用SHELLEXECUTE打开
	{
		CloseHandle(pShellExecute(NULL, L"open", &lpCmdLine[1], NULL, NULL, SW_SHOW));
		return 0;
	}
	else if (lpCmdLine[0] == L's')//打开任务计划
	{
		CloseHandle(pShellExecute(NULL, L"open", L"schtasks", &lpCmdLine[1], NULL, SW_HIDE));
		return 0;
	}
	if (IsUserAdmin())
	{
		LPWSTR lpServiceName = (LPWSTR)szAppName;
		InitService();
		SERVICE_TABLE_ENTRY st[] =
		{
			{ (LPWSTR)szAppName, (LPSERVICE_MAIN_FUNCTION)ServiceMain},
			{ NULL, NULL }
		};
		if (lstrcmpi(lpCmdLine, L"/install") == 0)
		{
			InstallService();
			return 0;
		}
		else if (lstrcmpi(lpCmdLine, L"/uninstall") == 0)
		{
			UninstallService();
			return 0;
		}
		else if (lstrcmpi(lpCmdLine, L"/start") == 0)
		{
			ServiceCtrlStart();
			return 0;
		}
		else if (lstrcmpi(lpCmdLine, L"/stop") == 0)
		{
			ServiceCtrlStop();
			return 0;
		}
		if (ServiceRunState() != SERVICE_RUNNING)
		{
			if (IsServiceInstalled())
			{
				if (ServiceRunState() == SERVICE_STOPPED)
					ServiceCtrlStart();
				StartServiceCtrlDispatcher(st);
				return 0;
			}
		}
		ServiceCtrlStop();
	}
*/
#endif
	pChangeWindowMessageFilter(WM_TRAYS, MSGFLT_ADD);
	pChangeWindowMessageFilter(WM_DROPFILES, MSGFLT_ADD);
	pChangeWindowMessageFilter(0x0049, MSGFLT_ADD);
	lpCmdLine = GetCommandLine();
	LPWSTR lpParameters = NULL;
	int iLen = lstrlen(lpCmdLine);
	int flag = 0;
	for (int i = 0; i < iLen; i++)
	{
		if (lpCmdLine[i] == L'\"')
		{
			++flag;
		}
		else if (flag == 2)
		{
			lpCmdLine = &lpCmdLine[i + 1];
			break;
		}
		else if (lpCmdLine[i] == L' ' && flag == 0)
		{
			lpCmdLine = &lpCmdLine[i + 1];
			if (lpCmdLine[0] == L'o')
			{
				for (int n = 1; n < iLen - i; n++)
				{
					if (lpCmdLine[n] == L' ')
					{
						lpCmdLine[n] = 0;
						lpParameters = &lpCmdLine[n + 1];
						break;
					}
				}
			}
			break;
		}
	}
	if (lpCmdLine[0] == L'c')////打开控制面板
	{
		CloseHandle(pShellExecute(NULL, L"open", L"control.exe", &lpCmdLine[1], NULL, SW_SHOW));
		ExitProcess(0);
	}
	else if (lpCmdLine[0] == L'o')//用SHELLEXECUTE打开
	{
		CloseHandle(pShellExecute(NULL, L"open", &lpCmdLine[1], lpParameters, NULL, SW_SHOW));
		ExitProcess(0);
	}
	else if (lpCmdLine[0] == L's')//打开任务计划
	{
		CloseHandle(pShellExecute(NULL, L"open", L"schtasks", &lpCmdLine[1], NULL, SW_HIDE));
		ExitProcess(0);
	}
	if (IsUserAdmin()==3)//////////////////////////////////////////////////以SYSYTEM权限启动
	{
//		lpServiceName = (LPWSTR)szAppName;
		InitService();
		SERVICE_TABLE_ENTRY st[] =
		{
			{ (LPWSTR)szAppName, (LPSERVICE_MAIN_FUNCTION)ServiceMain},
			{ NULL, NULL }
		};
		if (lstrcmpi(lpCmdLine, L"/install") == 0)
		{
			InstallService();
			ExitProcess(0);
		}
		else if (lstrcmpi(lpCmdLine, L"/uninstall") == 0)
		{
			UninstallService();
			ExitProcess(0);
		}
		else if (lstrcmpi(lpCmdLine, L"/start") == 0)
		{
			ServiceCtrlStart();
			ExitProcess(0);
		}
		else if (lstrcmpi(lpCmdLine, L"/stop") == 0)
		{
			ServiceCtrlStop();
			ExitProcess(0);
		}
		if (ServiceRunState() != SERVICE_RUNNING)
		{
			if (IsServiceInstalled())
			{
				if (ServiceRunState() == SERVICE_STOPPED)
					ServiceCtrlStart();
				StartServiceCtrlDispatcher(st);
				ExitProcess(0);
			}
		}
		ServiceCtrlStop();
	}
	hMutex = CreateMutex(NULL, TRUE, L"Local\\TrayS.SingleInstance");
	if (!hMutex)
		ExitProcess(GetLastError());
	if (GetLastError() == ERROR_ALREADY_EXISTS)
	{
		EnumWindows((WNDENUMPROC)FindSettingWindowFunc, 0);
		CloseHandle(hMutex);
		ExitProcess(0);
	}
	InitializeCriticalSection(&MonitorBackendLock);
	bMonitorBackendLockInitialized = TRUE;
	InitializeCriticalSection(&MonitorDataLock);
	bMonitorDataLockInitialized = TRUE;
	hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!hStopEvent)
	{
		DeleteCriticalSection(&MonitorDataLock);
		bMonitorDataLockInitialized = FALSE;
		DeleteCriticalSection(&MonitorBackendLock);
		bMonitorBackendLockInitialized = FALSE;
		CloseHandle(hMutex);
		ExitProcess(GetLastError());
	}
	hInst = GetModuleHandle(NULL); // 将实例句柄存储在全局变量中
	uTaskbarCreated = RegisterWindowMessage(L"TaskbarCreated");
	typedef WINUSERAPI DWORD WINAPI RTLGETVERSION(PRTL_OSVERSIONINFOW  lpVersionInformation);
	rovi.dwOSVersionInfoSize = sizeof(rovi);
	RTLGETVERSION* RtlGetVersion = (RTLGETVERSION*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
	if (RtlGetVersion)
		RtlGetVersion(&rovi);
	ReadReg();
	PublishMonitorSettings();
	{
		iMain = LoadIcon(hInst, MAKEINTRESOURCE(IDI_TRAYS));
		hDwmapi = LoadLibrary(L"dwmapi.dll");
		if (hDwmapi)
		{
			pDwmGetWindowAttribute = (pfnDwmGetWindowAttribute)GetProcAddress(hDwmapi, "DwmGetWindowAttribute");
		}
		SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
		if (TraySave.bMonitorTemperature)
			LoadTemperatureDLL();
		pProcessTime = NULL;
		EnableDebugPrivilege(TRUE);
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		dNumProcessor = si.dwNumberOfProcessors;
		if (dNumProcessor == 0)
			dNumProcessor = 1;
		for (int i = 0; i < 6; ++i)
		{
			ppmuWork[i] = &pmuWork[i];
			ppcuWork[i] = &pcuWork[i];
		}
//			g_hHeapWindowInfo = HeapCreate(NULL, 0, 0);
//
		// 执行应用程序初始化:
		if (!InitInstance(hInst, 0))
		{
			SetEvent(hStopEvent);
			if (hGetDataThread)
			{
				WaitForSingleObject(hGetDataThread, INFINITE);
				CloseHandle(hGetDataThread);
				hGetDataThread = NULL;
			}
			if (IsWindow(hSetting))
				DestroyWindow(hSetting);
			CloseTaskBar();
			pShell_NotifyIcon(NIM_DELETE, &nid);
			if (IsWindow(hMain))
			{
				APPBARDATA abd = { 0 };
				abd.cbSize = sizeof(abd);
				abd.hWnd = hMain;
				pSHAppBarMessage(ABM_REMOVE, &abd);
			}
			if (IsWindow(hMain))
				DestroyWindow(hMain);
			if (iMain)
			{
				DestroyIcon(iMain);
				iMain = NULL;
			}
			if (hFont)
			{
				DeleteObject(hFont);
				hFont = NULL;
			}
			if (hDwmapi)
			{
				FreeLibrary(hDwmapi);
				hDwmapi = NULL;
				pDwmGetWindowAttribute = NULL;
			}
			if (hIphlpapi)
			{
				FreeLibrary(hIphlpapi);
				hIphlpapi = NULL;
				GetAdaptersAddressesT = NULL;
				GetIfTableT = NULL;
				getIfTable2 = NULL;
				freeMibTable = NULL;
			}
			if (hOleacc)
			{
				FreeLibrary(hOleacc);
				hOleacc = NULL;
				AccessibleObjectFromWindowT = NULL;
				AccessibleChildrenT = NULL;
			}
			SwitchPDH(FALSE);
			FreeTemperatureDLL();
			if (hDesktopDC)
			{
				ReleaseDC(NULL, hDesktopDC);
				hDesktopDC = NULL;
			}
			if (hStopEvent)
			{
				CloseHandle(hStopEvent);
				hStopEvent = NULL;
			}
			if (bMonitorBackendLockInitialized)
			{
				DeleteCriticalSection(&MonitorBackendLock);
				bMonitorBackendLockInitialized = FALSE;
			}
			if (bMonitorDataLockInitialized)
			{
				DeleteCriticalSection(&MonitorDataLock);
				bMonitorDataLockInitialized = FALSE;
			}
			if (hMutex)
			{
				CloseHandle(hMutex);
				hMutex = NULL;
			}
#ifndef _DEBUG
			ExitProcess(0);
#else
			return 0;
#endif
		}
		MSG msg;
		// 主消息循环:
		while (GetMessage(&msg, nullptr, 0, 0))
		{
			if (!IsDialogMessage(hMain, &msg) && !IsDialogMessage(hSetting, &msg))
			{
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
		}
//			WaitForSingleObject(hThread, INFINITE);
		SetEvent(hStopEvent);
		if (hGetDataThread)
		{
			WaitForSingleObject(hGetDataThread, INFINITE);
			CloseHandle(hGetDataThread);
			hGetDataThread = NULL;
		}
//			CloseHandle(hMainThread);
		if (IsWindow(hSetting))
			DestroyWindow(hSetting);
		CloseTaskBar();
		if (IsWindow(hMain))
		{
			APPBARDATA abd = { 0 };
			abd.cbSize = sizeof(abd);
			abd.hWnd = hMain;
			pSHAppBarMessage(ABM_REMOVE, &abd);
		}
		if (IsWindow(hMain))
			DestroyWindow(hMain);
		pShell_NotifyIcon(NIM_DELETE, &nid);
		if (iMain)
		{
			DestroyIcon(iMain);
			iMain = NULL;
		}
		if (hFont)
		{
			DeleteObject(hFont);
			hFont = NULL;
		}
		//free(ipinfo);
		if (hDwmapi)
		{
			FreeLibrary(hDwmapi);
			hDwmapi = NULL;
			pDwmGetWindowAttribute = NULL;
		}
		if (hIphlpapi)
		{
			FreeLibrary(hIphlpapi);
			hIphlpapi = NULL;
			GetAdaptersAddressesT = NULL;
			GetIfTableT = NULL;
			getIfTable2 = NULL;
			freeMibTable = NULL;
		}
		if (hOleacc)
		{
			FreeLibrary(hOleacc);
			hOleacc = NULL;
			AccessibleObjectFromWindowT = NULL;
			AccessibleChildrenT = NULL;
		}
		SwitchPDH(FALSE);
		HeapFree(GetProcessHeap(), 0, mi);
		mi = NULL;
		HeapFree(GetProcessHeap(), 0, piaa);
		piaa = NULL;
		HeapFree(GetProcessHeap(), 0, traffic);
		traffic = NULL;
		nTraffic = 0;
		nTrafficCapacity = 0;
		HeapFree(GetProcessHeap(), 0, pProcessTime);
		pProcessTime = NULL;
		nProcessTimeCapacity = 0;
//			HeapDestroy(g_hHeapWindowInfo);
		FreeTemperatureDLL();
		if (hDesktopDC)
		{
			ReleaseDC(NULL, hDesktopDC);
			hDesktopDC = NULL;
		}
	}
	if (hStopEvent)
	{
		CloseHandle(hStopEvent);
		hStopEvent = NULL;
	}
	if (bMonitorBackendLockInitialized)
	{
		DeleteCriticalSection(&MonitorBackendLock);
		bMonitorBackendLockInitialized = FALSE;
	}
	if (bMonitorDataLockInitialized)
	{
		DeleteCriticalSection(&MonitorDataLock);
		bMonitorDataLockInitialized = FALSE;
	}
	if (hMutex)
	{
		CloseHandle(hMutex);
		hMutex = NULL;
	}
	ExitProcess((UINT)0);
}
DWORD dwIPSize = 0;
DWORD dwMISize = 0;
int iGetAddressTime = 10;//10秒一次获取网卡信息
DWORD WINAPI GetDataThreadProc(PVOID pParam)//获取温度占用硬盘线程
{
	DWORD lastThemeMode = GetSystemUsesLightTheme();
	while (WaitForSingleObject(hStopEvent, 0) == WAIT_TIMEOUT)
	{
		DWORD dStart = GetTickCount();
		TRAYSAVE settings;
		LockMonitorData();
		settings = MonitorSettings;
		UnlockMonitorData();
	if (settings.bMonitor)
	{
			LockMonitorBackend();
			GlobalMemoryStatusEx(&MemoryStatusEx);
			if (settings.bMonitorDisk)
			{
				if (settings.bMonitorUsage == FALSE)
					GetPDH(FALSE, TRUE);
			}
			if (settings.bMonitorUsage)
			{
				iCPU = GetCPUUseRate(settings);
			}
			if (InterlockedCompareExchange(&bTaskTipsActive, FALSE, FALSE))
			{
				nProcess = GetProcessMemUsage();
				int requiredProcessCapacity = nProcess + 32;
				if (requiredProcessCapacity > nProcessTimeCapacity)
				{
					PROCESSTIME* resizedProcessTime = pProcessTime
						? (PROCESSTIME*)HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, pProcessTime, sizeof(PROCESSTIME) * requiredProcessCapacity)
						: (PROCESSTIME*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PROCESSTIME) * requiredProcessCapacity);
					if (resizedProcessTime)
					{
						pProcessTime = resizedProcessTime;
						nProcessTimeCapacity = requiredProcessCapacity;
					}
				}
				GetProcessCpuUsage();
			}
			else
			{
				if (pProcessTime != NULL)
				{
					HeapFree(GetProcessHeap(), 0, pProcessTime);
					pProcessTime = NULL;
					nProcessTimeCapacity = 0;
				}
				ZeroMemory(pmuWork, sizeof(pmuWork));
				ZeroMemory(pcuWork, sizeof(pcuWork));
			}
			if (settings.bMonitorTemperature)
			{
				if (bRing0)
				{
					TrayData->iTemperature1 = GetCpuTemp(1, settings.szDisk);
					if (!hOHMA && hATIDLL == NULL && hNVDLL == NULL)
						TrayData->iTemperature2 = GetCpuTemp(dNumProcessor, settings.szDisk);
				}
				if (!hOHMA)
				{
					int iATITemperature = 0;
					int iNVTemperature = 0;
					if (hNVDLL)
					{
						NV_GPU_THERMAL_SETTINGS currentTemp;//获取温度的数据结构
						currentTemp.version = NV_GPU_THERMAL_SETTINGS_VER;//一定要设置，不然调用获取温度函数时候会出错
						for (int GpuIndex = 0; GpuIndex < nPhysicalGpu; GpuIndex++)
						{
							if (NvAPI_GPU_GetThermalSettings(hPhysicalGpu[GpuIndex], 15, &currentTemp) == 0)
							{
								iNVTemperature = currentTemp.sensor[0].currentTemp;
								break;
							}
						}
					}

					if (hATIDLL && ADL_Overdrive5_Temperature_Get)
					{
						adlTemperature.iSize = sizeof(ADLTemperature);
						if (ADL_Overdrive5_Temperature_Get(0, 0, &adlTemperature) == ADL_OK)
							iATITemperature = adlTemperature.iTemperature / 1000;
					}
					if (iATITemperature != 0 || iNVTemperature != 0)
					{
						if (iATITemperature > iNVTemperature)
							TrayData->iTemperature2 = iATITemperature;
						else
							TrayData->iTemperature2 = iNVTemperature;
					}
				}
			}
			if (settings.bMonitorTraffic)
			{
				if (hIphlpapi == NULL)
				{
					hIphlpapi = LoadLibrary(L"iphlpapi.dll");
					if (hIphlpapi)
					{
						GetAdaptersAddressesT = (pfnGetAdaptersAddresses)GetProcAddress(hIphlpapi, "GetAdaptersAddresses");
						getIfTable2 = (pfnGetIfTable2)GetProcAddress(hIphlpapi, "GetIfTable2");
						if (getIfTable2 == NULL)
							GetIfTableT = (pfnGetIfTable)GetProcAddress(hIphlpapi, "GetIfTable");
						else
							freeMibTable = (pfnFreeMibTable)GetProcAddress(hIphlpapi, "FreeMibTable");
					}
				}
				if (hIphlpapi && GetAdaptersAddressesT)
				{
					PIP_ADAPTER_ADDRESSES paa;
					if (iGetAddressTime == 10)
					{
						//				DWORD odwIPSize = dwIPSize;
						dwIPSize = 0;
						if (GetAdaptersAddressesT(AF_INET, 0, 0, piaa, &dwIPSize) == ERROR_BUFFER_OVERFLOW)
						{
							//					if (dwIPSize != odwIPSize)
							{

								HeapFree(GetProcessHeap(), 0, piaa);
								piaa = NULL;
								int n = 0;
								piaa = (PIP_ADAPTER_ADDRESSES)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dwIPSize);
								if (piaa && GetAdaptersAddressesT(AF_INET, 0, 0, piaa, &dwIPSize) == ERROR_SUCCESS)
								{
									paa = &piaa[0];
					while (paa)
									{
										if (paa->IfType != IF_TYPE_SOFTWARE_LOOPBACK && paa->IfType != IF_TYPE_TUNNEL)
										{
											++n;
										}
										paa = paa->Next;
									}
								if (n > MAX_TRAFFIC_ADAPTERS)
									n = MAX_TRAFFIC_ADAPTERS;
								if (n > nTrafficCapacity)
								{
									TRAFFIC* resizedTraffic = traffic
										? (TRAFFIC*)HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, traffic, n * sizeof(TRAFFIC))
										: (TRAFFIC*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n * sizeof(TRAFFIC));
									if (resizedTraffic)
									{
										traffic = resizedTraffic;
										nTrafficCapacity = n;
									}
								}
								if (n <= nTrafficCapacity)
									nTraffic = n;
								}
							}
						}
						iGetAddressTime = 0;
					}
					else
						iGetAddressTime++;
					if (nTraffic != 0 && piaa && traffic)
					{
						ULONG64 m_in_bytes = 0;
						ULONG64 m_out_bytes = 0;
						if (getIfTable2 && freeMibTable)
						{
							mit2 = NULL;
							if (getIfTable2(&mit2) == NO_ERROR && mit2)
							{
								for (DWORD i = 0; i < mit2->NumEntries; i++)
								{
								int l = 0;
								paa = &piaa[0];
								while (paa && l < nTraffic)
								{
									if (paa->IfType != IF_TYPE_SOFTWARE_LOOPBACK && paa->IfType != IF_TYPE_TUNNEL)
									{
										if (paa->IfIndex == mit2->Table[i].InterfaceIndex)
										{
											traffic[l].in_byte = (mit2->Table[i].InOctets - traffic[l].in_bytes);
											traffic[l].out_byte = (mit2->Table[i].OutOctets - traffic[l].out_bytes);
											traffic[l].in_bytes = mit2->Table[i].InOctets;
											traffic[l].out_bytes = mit2->Table[i].OutOctets;
											PIP_ADAPTER_UNICAST_ADDRESS pUnicast = paa->FirstUnicastAddress;
											while (pUnicast)
											{
												if (pUnicast->Address.lpSockaddr && AF_INET == pUnicast->Address.lpSockaddr->sa_family)// IPV4 地址，使用 IPV4 转换
												{
													void* pAddr = &((sockaddr_in*)pUnicast->Address.lpSockaddr)->sin_addr;
													byte* bp = (byte*)pAddr;
													wsprintf(traffic[l].IP4, L"%d.%d.%d.%d", bp[0], bp[1], bp[2], bp[3]);
													break;
												}
												//											else if (AF_INET6 == pUnicast->Address.lpSockaddr->sa_family)// IPV6 地址，使用 IPV6 转换
												//												inet_ntop(PF_INET6, &((sockaddr_in6*)pUnicast->Address.lpSockaddr)->sin6_addr, IP, sizeof(IP));
												pUnicast = pUnicast->Next;
											}
											lstrcpyn(traffic[l].FriendlyName, paa->FriendlyName, ARRAYSIZE(traffic[l].FriendlyName));
											lstrcpynA(traffic[l].AdapterName, paa->AdapterName, ARRAYSIZE(traffic[l].AdapterName));
											if (lstrlen(paa->FriendlyName) > 19)
											{
												paa->FriendlyName[16] = L'.';
												paa->FriendlyName[17] = L'.';
												paa->FriendlyName[18] = L'.';
												paa->FriendlyName[19] = L'\0';
											}
											if (settings.AdpterName[0] == '\0' || lstrcmpA(paa->AdapterName, settings.AdpterName) == 0)
											{
												m_in_bytes += mit2->Table[i].InOctets;
												m_out_bytes += mit2->Table[i].OutOctets;
											}

										}
										++l;
									}
									paa = paa->Next;
								}
								}
							}
							if (mit2)
							{
								freeMibTable(mit2);
								mit2 = NULL;
							}
						}
						else if (GetIfTableT && piaa)
						{
							DWORD tableStatus = GetIfTableT(mi, &dwMISize, FALSE);
							if (tableStatus == ERROR_INSUFFICIENT_BUFFER)
							{
								dwMISize += sizeof MIB_IFROW * 2;
								HeapFree(GetProcessHeap(), 0, mi);
								mi = NULL;
								mi = (MIB_IFTABLE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dwMISize);
								if (mi)
									tableStatus = GetIfTableT(mi, &dwMISize, FALSE);
							}
							if (tableStatus == NO_ERROR && mi)
							{
								for (DWORD i = 0; i < mi->dwNumEntries; i++)
								{
								int l = 0;
								paa = &piaa[0];
								while (paa && l < nTraffic)
								{
									if (paa->IfType != IF_TYPE_SOFTWARE_LOOPBACK && paa->IfType != IF_TYPE_TUNNEL)
									{
										if (paa->IfIndex == mi->table[i].dwIndex)
										{
											traffic[l].in_byte = (mi->table[i].dwInOctets - traffic[l].in_bytes);
											traffic[l].out_byte = (mi->table[i].dwOutOctets - traffic[l].out_bytes);
											traffic[l].in_bytes = mi->table[i].dwInOctets;
											traffic[l].out_bytes = mi->table[i].dwOutOctets;

											PIP_ADAPTER_UNICAST_ADDRESS pUnicast = paa->FirstUnicastAddress;
											//							char IP[130];
											while (pUnicast)
											{
														if (pUnicast->Address.lpSockaddr && AF_INET == pUnicast->Address.lpSockaddr->sa_family)// IPV4 地址，使用 IPV4 转换
												{
													void* pAddr = &((sockaddr_in*)pUnicast->Address.lpSockaddr)->sin_addr;
													byte* bp = (byte*)pAddr;
													wsprintf(traffic[l].IP4, L"%d.%d.%d.%d", bp[0], bp[1], bp[2], bp[3]);
													break;
												}
												//								else if (AF_INET6 == pUnicast->Address.lpSockaddr->sa_family)// IPV6 地址，使用 IPV6 转换
												//									inet_ntop(PF_INET6, &((sockaddr_in6*)pUnicast->Address.lpSockaddr)->sin6_addr, IP, sizeof(IP));
												pUnicast = pUnicast->Next;
											}
											//							MultiByteToWideChar(CP_ACP, 0, IP, 15, traffic[l].IP4, 15);
											lstrcpyn(traffic[l].FriendlyName, paa->FriendlyName, ARRAYSIZE(traffic[l].FriendlyName));
											lstrcpynA(traffic[l].AdapterName, paa->AdapterName, ARRAYSIZE(traffic[l].AdapterName));
											if (lstrlen(paa->FriendlyName) > 19)
											{
												paa->FriendlyName[16] = L'.';
												paa->FriendlyName[17] = L'.';
												paa->FriendlyName[18] = L'.';
												paa->FriendlyName[19] = L'\0';
											}
											//							wcsncpy_s(traffic[l].FriendlyName, 24, paa->FriendlyName,24);
											if (settings.AdpterName[0] == '\0' || lstrcmpA(paa->AdapterName, settings.AdpterName) == 0)
											{
												m_in_bytes += mi->table[i].dwInOctets;
												m_out_bytes += mi->table[i].dwOutOctets;
											}
										}
										++l;
									}
									paa = paa->Next;
								}
							}
						}
					}
					if (TrayData->m_last_in_bytes != 0)
						{
							TrayData->s_in_byte = m_in_bytes - TrayData->m_last_in_bytes;
							TrayData->s_out_byte = m_out_bytes - TrayData->m_last_out_bytes;
							/*
														s_in_bytes[iBytes] = s_in_byte / 1024;
														s_out_bytes[iBytes] = s_out_byte / 1024;
														if (iBytes == rNum-1)
															iBytes = 0;
														else
															++iBytes;
							*/
						}
						TrayData->m_last_out_bytes = m_out_bytes;
						TrayData->m_last_in_bytes = m_in_bytes;
					}
				}
			}
			/*
					else
					{
							if (hIphlpapi)
							{
								FreeLibrary(hIphlpapi);
								hIphlpapi = NULL;
								GetAdaptersAddressesT = NULL;
								GetIfTableT = NULL;
								getIfTable2 = NULL;
								freeMibTable = NULL;
							}
					}
			*/
			RefreshMonitorSnapshot();
			UnlockMonitorBackend();
			if (settings.bMonitor)
			{
				if (IsWindowVisible(hTaskTips))
					PostMessage(hMain, WM_TRAYS_REFRESH_UI, 0, 0);
				DWORD dm = GetSystemUsesLightTheme();
				if (dm != lastThemeMode)
				{
					PostMessage(hMain, WM_TRAYS_REFRESH_UI, 1, dm);
					lastThemeMode = dm;
				}

			}
		}
		DWORD dTime = GetTickCount() - dStart;
		if (dTime < 988)
			WaitForSingleObject(hStopEvent, 988 - dTime);
	}
	return 0;
}
//
//   函数: InitInstance(HINSTANCE, int)
//
//   目标: 保存实例句柄并创建主窗口
//
//   注释:
//
//        在此函数中，我们在全局变量中保存实例句柄并
//        创建和显示主程序窗口。
//
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
	hMain = ::CreateDialog(hInst, MAKEINTRESOURCE(IDD_MAIN), NULL, (DLGPROC)MainProc);
	if (!hMain)
	{
		return FALSE;
	}
	MemoryStatusEx.dwLength = sizeof MEMORYSTATUSEX;
	////////////////////////////////////////////////////////////当前DPI
	hDesktopDC = GetDC(NULL);
	HDC hdc = GetDC(hMain);
	iDPI = GetDeviceCaps(hdc, LOGPIXELSY);
	::ReleaseDC(hMain, hdc);
//	EnableNonClientDpiScaling(hMain);
	//////////////////////////////////////////////////////////////////创建程序全屏时消息
	APPBARDATA abd;
	abd.cbSize = sizeof(abd);
	abd.hWnd = hMain;
	abd.uCallbackMessage = MSG_APPBAR_MSGID;
	pSHAppBarMessage(ABM_NEW, &abd);
	bThemeMode = GetSystemUsesLightTheme();
	//////////////////////////////////////////////////////////////////////////////////设置通知栏图标
	nid.cbSize = sizeof NOTIFYICONDATA;
	nid.uID = WM_IAWENTRAY;
	nid.hWnd = hMain;
	nid.hIcon = iMain;
	nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	nid.uCallbackMessage = WM_IAWENTRAY;
	//			nid.dwInfoFlags = NIIF_INFO;
	LoadString(hInst, IDS_TIPS, nid.szTip, 88);
	if (TraySave.bTrayIcon)
		pShell_NotifyIcon(NIM_ADD, &nid);

	GlobalMemoryStatusEx(&MemoryStatusEx);
	RefreshMonitorSnapshot();
	if (TraySave.bMonitor)
	{
		AdjustWindowPos();
	}
	UpdateMainRefreshTimer();//自定时间处理监控窗口位置和任务栏透明
	SetTimer(hMain, 6, 1000, NULL);//每秒处理任务栏图标
	SetTimer(hMain, 11, 6000, NULL);//内存释放
	if (!IsWindow(hTray) || !GetShellAllWnd())
		SetTimer(hMain, 3000, 3000, NULL);//Explorer 尚未就绪时后台重试
	hGetDataThread = CreateThread(NULL, 0, GetDataThreadProc, 0, 0, 0);
	return hGetDataThread != NULL;
}
BOOL Find(IAccessible* paccParent, int iRole, IAccessible** paccChild)//查找任务图标UI
{
	HRESULT hr;
	long numChildren;
	unsigned long numFetched;
	VARIANT varChild;
	int indexCount;
	IAccessible* pChild = NULL;
	IEnumVARIANT* pEnum = NULL;
	IDispatch* pDisp = NULL;
	BOOL found = false;
	//Get the IEnumVARIANT interface
	hr = paccParent->QueryInterface(IID_IEnumVARIANT, (PVOID*)&pEnum);
	if (pEnum)
		pEnum->Reset();
	// Get child count
	paccParent->get_accChildCount(&numChildren);
	for (indexCount = 1; indexCount <= numChildren && !found; indexCount++)
	{
		pChild = NULL;
		if (pEnum)
			hr = pEnum->Next(1, &varChild, &numFetched);
		else
		{
			varChild.vt = VT_I4;
			varChild.lVal = indexCount;
		}
		if (varChild.vt == VT_I4)
		{
			pDisp = NULL;
			hr = paccParent->get_accChild(varChild, &pDisp);
		}
		else
			pDisp = varChild.pdispVal;
		if (pDisp)
		{
			hr = pDisp->QueryInterface(IID_IAccessible, (void**)&pChild);
			hr = pDisp->Release();
		}
		if (pChild)
		{
			VariantInit(&varChild);
			varChild.vt = VT_I4;
			varChild.lVal = CHILDID_SELF;
			*paccChild = pChild;
		}
		VARIANT varState;
		pChild->get_accState(varChild, &varState);
		if ((varState.intVal & STATE_SYSTEM_INVISIBLE) == 0)
		{
			VARIANT varRole;
			pChild->get_accRole(varChild, &varRole);
			if (varRole.lVal == iRole)
			{
				paccParent->Release();
				found = true;
				break;
			}
		}
		if (!found && pChild)
		{
			//			found = Find(pCAcc, iRole, paccChild);
			//			if (*paccChild != pCAcc)
			pChild->Release();
		}
	}
	if (pEnum)
		pEnum->Release();
	return found;
}
int oleft=0, otop=0;
int iIconsWidth=0;
void SetTaskBarPos(HWND hTaskListWnd, HWND hTrayWnd, HWND hTaskWnd, HWND hReBarWnd, BOOL bMainTray)//设置任务栏图标位置
{
	if (!IsWindow(hTaskListWnd) || !IsWindow(hTrayWnd) || !IsWindow(hTaskWnd) || !IsWindow(hReBarWnd))
		return;
	if (hOleacc == NULL)
	{
		hOleacc = LoadLibrary(L"oleacc.dll");
		if (hOleacc)
		{
			AccessibleObjectFromWindowT = (pfnAccessibleObjectFromWindow)GetProcAddress(hOleacc, "AccessibleObjectFromWindow");
			AccessibleChildrenT = (pfnAccessibleChildren)GetProcAddress(hOleacc, "AccessibleChildren");
		}
	}
	if (hOleacc == NULL)
		return;
	IAccessible* pAcc = NULL;
	AccessibleObjectFromWindowT(hTaskListWnd, OBJID_WINDOW, IID_IAccessible, (void**)&pAcc);
	IAccessible* paccChlid = NULL;
	if (pAcc)
	{
		if (Find(pAcc, 22, &paccChlid) == FALSE)
		{
			return;
		}
	}
	else
		return;
	long childCount;
	long returnCount;
	LONG left, top, width, height;
	LONG ol = 0, ot = 0;
	int tWidth = 0;
	int tHeight = 0;
	if (paccChlid)
	{
		if (paccChlid->get_accChildCount(&childCount) == S_OK && childCount != 0)
		{
			VARIANT* pArray = (VARIANT*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof VARIANT * childCount);
			if (pArray && AccessibleChildrenT(paccChlid, 0L, childCount, pArray, &returnCount) == S_OK)
			{
				for (int x = 0; x < returnCount; x++)
				{
					VARIANT vtChild = pArray[x];
					{

						VARIANT varState;
						paccChlid->get_accState(vtChild, &varState);
						if ((varState.intVal & STATE_SYSTEM_INVISIBLE) == 0)
						{
							VARIANT varRole;
							paccChlid->get_accRole(vtChild, &varRole);
							if (varRole.intVal == 0x2b || varRole.intVal == 0x39)
							{
								paccChlid->accLocation(&left, &top, &width, &height, vtChild);
								if (ol != left)
								{
									tWidth += width;
									ol = left;
								}
								if (ot != top)
								{
									tHeight += height;
									ot = top;
								}
							}
						}
					}
				}
			}
			HeapFree(GetProcessHeap(), 0, pArray);
		}
		paccChlid->Release();
	}
	else
		return;
	iIconsWidth = tWidth;
	RECT lrc, src, trc;
	GetWindowRect(hTaskListWnd, &lrc);
	GetWindowRect(hTrayWnd, &src);
	GetWindowRect(hTaskWnd, &trc);
	BOOL Vertical = FALSE;
	if (src.right - src.left < src.bottom - src.top)
		Vertical = TRUE;
	SendMessage(hReBarWnd, WM_SETREDRAW, TRUE, 0);
	int lr, tb;
	if (Vertical)
	{
		int t = trc.left - src.left;
		int b = src.bottom - trc.bottom;
		if (bMainTray && TraySave.bMonitor && TraySave.bMonitorFloat == FALSE)
		{
			if (TraySave.bMonitorLeft == FALSE)
				b += mHeight;
			else
				t += mHeight;
		}
		if (t > b)
			tb = t;
		else
			tb = b;
	}
	else
	{
		int l = trc.left - src.left;
		int r = src.right - trc.right;
		if (TraySave.bMonitor && bMainTray && TraySave.bMonitorFloat == FALSE)
		{
			if (TraySave.bMonitorLeft == FALSE)
				r += mWidth;
			else
				l += mWidth;
		}
		if (l > r)
			lr = l;
		else
			lr = r;
	}
	int nleft, ntop;
	if ((TraySave.iPos == 2 || (Vertical == FALSE && tWidth >= trc.right - trc.left - lr) || (Vertical && tHeight >= trc.bottom - trc.top - tb)) && TraySave.iPos != 0)
	{
		if (Vertical)
		{
			ntop = trc.bottom - trc.top - tHeight;
			if (TraySave.bMonitorLeft == FALSE && TraySave.bMonitor && bMainTray && TraySave.bMonitorFloat == FALSE)
				ntop -= mHeight + 2;
		}
		else
		{
			nleft = trc.right - trc.left - tWidth;
			if (TraySave.bMonitorLeft == FALSE && TraySave.bMonitor && bMainTray && TraySave.bMonitorFloat == FALSE)
				nleft -= mWidth + 2;
		}
	}
	else if (TraySave.iPos == 0)
	{
		if (TraySave.bMonitorLeft && TraySave.bMonitor && bMainTray && TraySave.bMonitorFloat == FALSE)
		{
			nleft = mWidth;
			ntop = mHeight;
		}
		else
		{
			nleft = 0;
			ntop = 0;
			if (TraySave.bMonitor == FALSE)
			{
				SetTimer(hMain, 11, 1000, NULL);
			}
		}
	}
	else if (TraySave.iPos == 1)
	{
		if (Vertical)
			ntop = src.top + (src.bottom - src.top) / 2 - trc.top - tHeight / 2;
		else
			nleft = src.left + (src.right - src.left) / 2 - trc.left - tWidth / 2;
		if (bMainTray)
		{
			if (Vertical)
				ntop -= 2;
			else
				nleft -= 2;
		}
	}
	if (Vertical)
	{
		if (bMainTray)
		{
			if (otop == 0)
				lrc.top = ntop;
			else
				lrc.top = otop;
			otop = ntop;
			while (ntop != lrc.top)
			{
				if (ntop > lrc.top)
					++lrc.top;
				else
					--lrc.top;
				SetWindowPos(hTaskListWnd, 0, 0, lrc.top, lrc.right - lrc.left, lrc.bottom - lrc.top, SWP_NOSIZE | SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSENDCHANGING);
			}
		}
		SetWindowPos(hTaskListWnd, 0, 0, ntop, lrc.right - lrc.left, lrc.bottom - lrc.top, SWP_NOSIZE | SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSENDCHANGING);
	}
	else
	{
		if (bMainTray)
		{
			if (oleft == 0)
				lrc.left = nleft;
			else
				lrc.left = oleft;
			oleft = nleft;
			while (nleft != lrc.left)
			{
				if (nleft > lrc.left)
					++lrc.left;
				else
					--lrc.left;
				SetWindowPos(hTaskListWnd, 0, lrc.left, 0, lrc.right - lrc.left, lrc.bottom - lrc.top, SWP_NOSIZE | SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSENDCHANGING);
			}
		}
		SetWindowPos(hTaskListWnd, 0, nleft, 0, lrc.right - lrc.left, lrc.bottom - lrc.top, SWP_NOSIZE | SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSENDCHANGING);
	}
	if (TraySave.iPos != 0)
		SendMessage(hReBarWnd, WM_SETREDRAW, FALSE, 0);
	ShowWindow(hTaskWnd, SW_SHOWNOACTIVATE);
}
int otleft, ottop;
void SetWH()
{
	mWidth = 0;
	mHeight = 0;
	HDC mdc = GetDC(hMain);
	TraySave.TraybarFont.lfHeight = DPI(TraySave.TraybarFontSize);
	DeleteObject(hFont);
	hFont = CreateFontIndirect(&TraySave.TraybarFont); //创建字体
	HFONT oldFont = (HFONT)SelectObject(mdc, hFont);
	SIZE tSize;
	WCHAR sz[16]=L"8";
	::GetTextExtentPoint(mdc, sz, lstrlen(sz), &tSize);
	wSpace = tSize.cx * 6/8;
	wHeight = tSize.cy;
	if (TraySave.bMonitorTraffic)
	{
		if (TraySave.iMonitorSimple == 1)
		{
			WCHAR szT[] = L"M↓:8.88";
			::GetTextExtentPoint(mdc, szT, lstrlen(szT), &tSize);
		}
		else if (TraySave.iMonitorSimple == 2)
		{
			WCHAR szT[] = L"M8.88";
			::GetTextExtentPoint(mdc, szT, lstrlen(szT), &tSize);
		}
		else
		{
			wsprintf(sz, L"M%s8.88", TraySave.szTrafficOut);
			::GetTextExtentPoint(mdc, sz, lstrlen(sz), &tSize);
		}
		wTraffic = tSize.cx + wSpace;
		mWidth += wTraffic;
		mHeight += wHeight * 2;
	}
	else
		wTraffic = 0;
	if (TraySave.bMonitorUsage)
	{
		if (TraySave.iMonitorSimple == 1)
			::GetTextExtentPoint(mdc, L"88%", lstrlen(L"88%"), &tSize);
		else if (TraySave.iMonitorSimple == 2)
			::GetTextExtentPoint(mdc, L"88", lstrlen(L"88"), &tSize);
		else
		{
			wsprintf(sz, L"%s88%s", TraySave.szUsageMEM, TraySave.szUsageMEMUnit);
			::GetTextExtentPoint(mdc, sz, lstrlen(sz), &tSize);
		}
		wUsage = tSize.cx + wSpace;
		mWidth += wUsage;
		mHeight += wHeight * 2;
	}
	else
		wUsage = 0;
	if (TraySave.bMonitorTemperature)
	{
		if (TraySave.iMonitorSimple == 1)
			::GetTextExtentPoint(mdc, L"88℃", lstrlen(L"88℃"), &tSize);
		else if (TraySave.iMonitorSimple == 2)
			::GetTextExtentPoint(mdc, L"88", lstrlen(L"88"), &tSize);
		else
		{
			wsprintf(sz, L"%s88%s", TraySave.szTemperatureGPU, TraySave.szTemperatureGPUUnit);
			::GetTextExtentPoint(mdc, sz, lstrlen(sz), &tSize);
		}
		wTemperature = tSize.cx + wSpace;
		mWidth += wTemperature;
		if (bRing0)
			mHeight += wHeight * 2;
		else
			mHeight += wHeight;
	}
	else
		wTemperature = 0;
	if (TraySave.bMonitorDisk)
	{
		if (TraySave.iMonitorSimple == 1)
		{
			WCHAR szT[] = L" MR:8.88";
			::GetTextExtentPoint(mdc, szT, lstrlen(szT), &tSize);
		}
		else if (TraySave.iMonitorSimple == 2)
		{
			WCHAR szT[] = L" M8.88";
			::GetTextExtentPoint(mdc, szT, lstrlen(szT), &tSize);
		}
		else
		{
			wsprintf(sz, L" M%s8.88", TraySave.szDiskReadSec);
			::GetTextExtentPoint(mdc, sz, lstrlen(sz), &tSize);
		}
		wDisk = tSize.cx + wSpace;
		if (hOHMA&&TraySave.bMonitorTemperature)
		{
			if (TraySave.bMonitorFloatVRow && TraySave.bMonitorFloat)
			{
			}
			else
				wDisk += wTemperature;
			mHeight += wHeight * 2;
		}
		mWidth += wDisk;
		mHeight += wHeight * 2;
	}
	else
		wDisk = 0;
	if (TraySave.bMonitorTime)
	{
		::GetTextExtentPoint(mdc, L"88:88:88", lstrlen(L"88:88:88"), &tSize);
		wTime = tSize.cx + wSpace;
		mWidth += wTime;
		mHeight += wHeight * 2;
	}
	else
		wTime = 0;
//	mWidth += 4;
//	mHeight += 4;
	SelectObject(mdc, oldFont);
	ReleaseDC(hMain, mdc);
	ottop = -1;
	otleft = -1;
}
void AdjustWindowPos()//设置信息窗口位置大小
{	
	if (IsWindow(hTray) == FALSE)//任务栏奔溃时重启
	{
		DestroyWindow(hTime);
		DestroyWindow(hTaskBar);
		bFullScreen = FALSE;
		return;
	}
	int dpi = pGetDpiForWindow(hTray);
	if (dpi != iDPI && dpi != 0)
	{
		iDPI = dpi;
		SendMessage(hMain, WM_DPICHANGED, dpi, dpi);
	}
	if (IsWindow(hTaskBar) == FALSE)
		OpenTaskBar();
	if (TraySave.bSecond && IsWindow(hTime) == FALSE)
		OpenTimeDlg();
	if (TraySave.bMonitorFloat)
	{
		RECT ScreenRect;
		GetScreenRect(hTaskBar, &ScreenRect, FALSE);
		if (TraySave.bMonitorFloatVRow)
		{
			int iWidth = wTraffic;
			if (wTime > iWidth)
				iWidth = wTime;
			if (wTemperature > iWidth)
				iWidth = wTemperature;
			if (wDisk > iWidth)
				iWidth = wDisk;
			if (wUsage > iWidth)
				iWidth = wUsage;
			if (TraySave.dMonitorPoint.x + iWidth > ScreenRect.right)
				TraySave.dMonitorPoint.x = ScreenRect.right - iWidth;
			if (TraySave.dMonitorPoint.y + mHeight + 8 > ScreenRect.bottom)
				TraySave.dMonitorPoint.y = ScreenRect.bottom - mHeight - 8;
			SetWindowPos(hTaskBar, HWND_TOPMOST, TraySave.dMonitorPoint.x, TraySave.dMonitorPoint.y, iWidth, mHeight, SWP_NOACTIVATE);
			VTray = TRUE;
		}
		else
		{
			if (TraySave.dMonitorPoint.x + mWidth > ScreenRect.right)
				TraySave.dMonitorPoint.x = ScreenRect.right - mWidth;
			if (TraySave.dMonitorPoint.y + wHeight * 2 > ScreenRect.bottom)
				TraySave.dMonitorPoint.y = ScreenRect.bottom - wHeight * 2;
			SetWindowPos(hTaskBar, HWND_TOPMOST, TraySave.dMonitorPoint.x, TraySave.dMonitorPoint.y, mWidth, wHeight * 2, SWP_NOACTIVATE);
			VTray = FALSE;
		}		
	}
	else
	{
		/*
			RECT src,frc;
			if (!TraySave.bMonitorTopmost)
			{
				HWND fwnd = GetForegroundWindow();
				GetWindowRect(fwnd, &frc);
				GetScreenRect(GetForegroundWindow(), &src, FALSE);
				DWORD pid1, pid2;
				GetWindowThreadProcessId(hTray, &pid1);
				GetWindowThreadProcessId(fwnd, &pid2);
				if (EqualRect(&src, &frc) && pid1 != pid2)
				{
					ShowWindow(hTaskBar, SW_HIDE);
					return;
				}
			}
		*/
		RECT trayrc;
		GetWindowRect(hTray, &trayrc);
		if (trayrc.right - trayrc.left > trayrc.bottom - trayrc.top)
			VTray = FALSE;
		else
			VTray = TRUE;
		if (VTray == FALSE)
		{
			int nleft;
			if (hWin11UI)
			{
				RECT startrc, tasklistrc;
				startrc.left = 88;
				if (GetWindowRect(hStartWnd, &startrc))
				{
					if (GetWindowRect(hTaskListWnd, &tasklistrc))
					{
						BOOL bLeft = TraySave.bMonitorLeft;
						if (startrc.left == trayrc.left)
							bLeft = FALSE;
						if (TraySave.bNear)
						{
							if (bLeft)
							{
								nleft = startrc.left - mWidth;
							}
							else
							{
								nleft = tasklistrc.right;
							}
						}
						else
						{
							if (!bLeft)
							{
								RECT tnrc;
								GetWindowRect(hTrayNotifyWnd, &tnrc);
								nleft = tnrc.left - mWidth;
							}
							else
							{
								nleft = trayrc.left;
							}
						}
					}
				}
			}
			else
			{
				if (TraySave.bNear)
				{
					RECT tasklistrc;
					GetWindowRect(hTaskListWnd, &tasklistrc);
					if (TraySave.bMonitorLeft)
						nleft = tasklistrc.left - mWidth;
					else
						nleft = tasklistrc.left + iIconsWidth + 2;
				}
				else
				{
					RECT taskrc;
					GetWindowRect(hTaskWnd, &taskrc);
					if (TraySave.bMonitorLeft)
						nleft = taskrc.left + 2;
					else
						nleft = taskrc.right - mWidth;
				}
			}
			int h = wHeight * 2;
			int ntop;
			if (trayrc.bottom - trayrc.top < h)
			{
				h = trayrc.bottom - trayrc.top - 2;
				ntop = trayrc.top;
			}
			else
				ntop = (trayrc.bottom - trayrc.top - h) / 2 + trayrc.top;
			//		if (!hWin11UI)
			if(!bFullScreen)
				ntop -= trayrc.top;
/*
			if (hWin11UI)
				ntop += 1;
*/
			if (nleft != otleft || ottop != ntop)
			{
				/*
							HDC hdc = GetDC(hTaskBar);
							RECT crc;
							GetClientRect(hTaskBar, &crc);
							HBRUSH hb = CreateSolidBrush(RGB(0, 0, 0));
							FillRect(hdc, &crc, hb);
							DeleteObject(hb);
							ReleaseDC(hTaskBar, hdc);
				*/
				otleft = nleft;
				ottop = ntop;
				//			::InvalidateRect(hTaskBar, NULL, TRUE);
				//			if (!hWin11UI)
				if(bFullScreen)
					SetWindowPos(hTaskBar, HWND_TOPMOST, nleft, ntop, mWidth, h, SWP_NOACTIVATE | SWP_NOREDRAW | SWP_SHOWWINDOW);
				else
					MoveWindow(hTaskBar, nleft, ntop, mWidth, h, TRUE);
				
				//			else
				//				SetWindowPos(hTaskBar, HWND_TOPMOST, nleft, ntop, mWidth, h, SWP_NOACTIVATE | SWP_NOREDRAW | SWP_SHOWWINDOW);
			}
			//		else if(hWin11UI)
			//			SetWindowPos(hTaskBar, HWND_TOPMOST, nleft, ntop, mWidth, h, SWP_NOACTIVATE|SWP_NOREDRAW|SWP_NOSIZE|SWP_NOMOVE|SWP_SHOWWINDOW);
			if(bFullScreen)
				SetWindowPos(hTaskBar, HWND_TOPMOST, nleft, ntop, mWidth, h, SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOSIZE | SWP_NOMOVE | SWP_SHOWWINDOW);
		}
		else
		{
			int ntop;
			RECT taskrc;
			GetWindowRect(hTaskWnd, &taskrc);
			if (TraySave.bMonitorLeft)
				ntop = taskrc.top+2;
			else
				ntop = taskrc.bottom - mHeight;
			int nleft = 1;
			int w = trayrc.right - trayrc.left - 2;
			if (bFullScreen)
				nleft = trayrc.left + 1;
			if (ntop != ottop || otleft != w)
			{
				/*
							HDC hdc = GetDC(hTaskBar);
							RECT crc;
							GetClientRect(hTaskBar, &crc);
							HBRUSH hb = CreateSolidBrush(RGB(0, 0, 0));
							FillRect(hdc, &crc, hb);
							DeleteObject(hb);
							ReleaseDC(hTaskBar, hdc);
				*/
				ottop = ntop;
				otleft = w;
				if (bFullScreen)
					SetWindowPos(hTaskBar, HWND_TOPMOST, nleft, ntop, w, mHeight, SWP_NOACTIVATE | SWP_NOREDRAW | SWP_SHOWWINDOW);
				else
					MoveWindow(hTaskBar, nleft, ntop, w, mHeight, TRUE);
			}
		}
	}
	if (TraySave.bSecond)
	{
		if (!hWin11UI)
		{
			if (GetAncestor(hTime, GA_PARENT) != hTrayClockWnd)
			{
				DestroyWindow(hTime);
//				SetParent(hTime, hTrayClockWnd);
			}
			else
			{
				if (VTray)
				{
					ShowWindow(hTime, SW_HIDE);
				}
				else
				{
					RECT rc;
					GetWindowRect(hTrayClockWnd, &rc);
					SetWindowPos(hTime, 0, 0, 0, rc.right - rc.left, (rc.bottom - rc.top) / 2, SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOREDRAW);
				}
			}
		}
		else
		{
			if (GetAncestor(hTime, GA_PARENT) != hTray)
			{
				DestroyWindow(hTime);
//				SetParent(hTime, hTray);
			}
			else
			{
				RECT rc;
				GetWindowRect(hTray, &rc);
				SetWindowPos(hTime, NULL, rc.right - rc.left - ((rc.bottom - rc.top) * 153/100), 1, rc.bottom - rc.top, (rc.bottom - rc.top-2) / 2, SWP_NOACTIVATE | SWP_NOREDRAW);
			}
		}
	}
}
void GetTrafficStr(WCHAR* sz, ULONG64 uByte, BOOL bBit, int iUnit)
{
	if (bBit)
		uByte *= 8;
	if (((uByte < 1024 && !bBit) || (uByte < 1000 && bBit)) && iUnit==0)
		wsprintf(sz, L"%dB", uByte);
	else if (((uByte < 1048576 && !bBit) || (uByte < 1000000 && bBit)) && iUnit != 2)
	{
		ULONG64 k_byte;
		if (bBit)
			k_byte = uByte / 10;
		else
			k_byte = uByte * 100 / 1024;
		if (k_byte >= 10000)
			wsprintf(sz, L"%dK", k_byte / 100);
		else if (k_byte >= 1000)
		{
			k_byte /= 10;
			wsprintf(sz, L"%d.%dK", k_byte / 10, k_byte % 10);
		}
		else
		{
			wsprintf(sz, L"%d.%dK", k_byte / 100, k_byte % 100);
		}
	}
	else if ((uByte < 1073741824 && !bBit) || (uByte < 1000000000 && bBit))
	{
		ULONG64 m_byte;
		if (bBit)
			m_byte = uByte / 10000;
		else
			m_byte = uByte * 100 / 1048576;
		if (m_byte >= 10000)
			wsprintf(sz, L"%dM", m_byte / 100);
		else if (m_byte >= 1000)
		{
			m_byte /= 10;
			wsprintf(sz, L"%d.%dM", m_byte / 10, m_byte % 10);
		}
		else
			wsprintf(sz, L"%d.%dM", m_byte / 100, m_byte % 100);
	}
	else if ((uByte < 1099511627776 && !bBit) || (uByte < 1000000000000 && bBit))
	{
		ULONG64 g_byte;
		if (bBit)
			g_byte = uByte / 10000000;
		else
			g_byte = uByte * 100 / 1073741824;
		if (g_byte >= 10000)
			wsprintf(sz, L"%dG", g_byte / 100);
		else if (g_byte >= 10)
		{
			g_byte /= 10;
			wsprintf(sz, L"%d.%dG", g_byte / 10, g_byte % 10);
		}
		else
			wsprintf(sz, L"%d.%dG", g_byte / 100, g_byte % 100);
	}
	else
	{
		ULONG64 t_byte;
		if (bBit)
			t_byte = uByte / 10000000000;
		else
			t_byte = uByte * 100 / 1099511627776;
		if (t_byte >= 10000)
			wsprintf(sz, L"%dT", t_byte / 100);
		else if (t_byte >= 10)
		{
			t_byte /= 10;
			wsprintf(sz, L"%d.%dT", t_byte / 10, t_byte % 10);
		}
		else
			wsprintf(sz, L"%d.%dT", t_byte / 100, t_byte % 100);
	}
	if (bBit)
		lstrlwr(sz, lstrlen(sz));
}
INT_PTR CALLBACK TaskTipsProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)//提示信息窗口过程
{
	static TRAFFIC trafficData[MAX_TRAFFIC_ADAPTERS] = { 0 };
	static PROCESSMEMORYUSAGE memoryProcesses[6] = { 0 };
	static PROCESSCPUUSAGE cpuProcesses[6] = { 0 };
	static MEMORYSTATUSEX memoryStatus = { 0 };
	static TRAYSAVE displaySettings = { 0 };
	int trafficCount = ReadTaskTipsSnapshot(
		trafficData,
		ARRAYSIZE(trafficData),
		memoryProcesses,
		cpuProcesses,
		&memoryStatus,
		&displaySettings);
	switch (message)
	{
	case WM_INITDIALOG:
		InterlockedExchange(&bTaskTipsActive, TRUE);
		return (INT_PTR)TRUE;
	case WM_DESTROY:
		InterlockedExchange(&bTaskTipsActive, FALSE);
		return (INT_PTR)TRUE;
	case WM_MOUSEMOVE:
	{
		POINT pt;
		pt.x = GET_X_LPARAM(lParam);
		pt.y = GET_Y_LPARAM(lParam);
		RECT rc;
		GetClientRect(hDlg, &rc);
		rc.top = trafficCount * wTipsHeight;
		rc.bottom = (trafficCount + 12) * wTipsHeight;
		rc.left = rc.right * 100 / 178;
		rc.right = rc.right * 100 / 145;
		if (PtInRect(&rc, pt))
		{
			inTipsProcessX = TRUE;
			::InvalidateRect(hDlg, NULL, TRUE);
		}
		else
		{
			inTipsProcessX = FALSE;
		}
	}
	break;
	case WM_LBUTTONDOWN:
	{
		POINT pt;
		pt.x = GET_X_LPARAM(lParam);
		pt.y = GET_Y_LPARAM(lParam);
		if (pt.y == 0)
			pt.y = 1;
		if (pt.y < trafficCount * wTipsHeight)
			RunProcess(NULL, szNetCpl);
		else if (pt.y < (trafficCount + 12) * wTipsHeight)
		{
			RECT rc;
			GetClientRect(hDlg, &rc);
			rc.left = rc.right * 100 / 178;
			rc.right = rc.right * 100 / 145;
			if (PtInRect(&rc, pt))
			{				
				int x = 0;
				if (wTipsHeight != 0)
					x = (pt.y / wTipsHeight) - trafficCount;
				if (x < 0 || x >= 12)
					return TRUE;
				DWORD pid = 0;
				if (x < 6)
					pid = cpuProcesses[x].dwProcessID;
				else
					pid = memoryProcesses[x - 6].dwProcessID;
				if (pid == 0)
					return TRUE;
				rc.left = rc.right * 145 / 156;
				if (PtInRect(&rc, pt))
				{
					HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
					if (hProc)
					{
						TerminateProcess(hProc, 0);
						CloseHandle(hProc);
					}
				}
				else
				{
					OpenProcessPath(pid);
				}
				inTipsProcessX = FALSE;
				GetCursorPos(&pt);
				SetCursorPos(pt.x + 88, pt.y);
			}
			else
			{
				if((pt.y / wTipsHeight) - trafficCount<6)
					RunProcess(NULL, szTaskmgr);
				else
					RunProcess(NULL, szPerfmon);
			}
		}
		else
		{
			RECT rc;
			GetClientRect(hDlg, &rc);
			if (pt.x < rc.right * 8 / 100)
			{
				bSetting = TRUE;
				SendMessage(hMain, WM_TRAYS, 0, 0);				
			}
			else if (pt.x > rc.right * 92 / 100)
			{
				SendMessage(hMain, WM_CLOSE, 0, 0);
			}
			else
			{
				if (pt.y < (trafficCount + 13) * wTipsHeight)
				{
					WCHAR wDrive[MAX_PATH];
					DWORD dwLen = GetLogicalDriveStrings(MAX_PATH, wDrive);
					if (dwLen != 0)
					{
						DWORD driver_number = dwLen / 4;
						DWORD x = pt.x / ((rc.right - rc.right * 8 / 100) / driver_number);
						if (x < driver_number)
						{
							WCHAR sz[24];
							memset(sz, 0, 48);
							wsprintf(sz, L"o%s", &wDrive[x * 4]);
							RunProcess(NULL, sz);
						}
					}
				}
				else
				{
					if(pt.x<rc.right/2)
						RunProcess(NULL, szCompmgmt);
					else
						RunProcess(NULL, szPowerCpl);
				}
			}
		}
		return TRUE;
	}
	break;
	case WM_ERASEBKGND:
		HDC hdc = (HDC)wParam;//BeginPaint(hDlg, &ps);
		RECT rc, crc;
		GetClientRect(hDlg, &rc);
		crc = rc;
		HDC mdc = CreateCompatibleDC(hdc);
		if (mdc)
		{
			HBITMAP hMemBmp = CreateCompatibleBitmap(hdc, rc.right - rc.left, rc.bottom - rc.top);
			HBITMAP oldBmp = (HBITMAP)SelectObject(mdc, hMemBmp);
			//		if (bErasebkgnd)
			{
				displaySettings.TipsFont.lfHeight = DPI(displaySettings.TipsFontSize);
				HFONT hTipsFont = CreateFontIndirect(&displaySettings.TipsFont); //创建字体
				HFONT oldFont = (HFONT)SelectObject(mdc, hTipsFont);
				WCHAR sz[64];
				SetBkMode(mdc, TRANSPARENT);
				COLORREF rgb;
				rgb = RGB(192, 192, 192);
				SetTextColor(mdc, rgb);
				rc.bottom = wTipsHeight;
				HBRUSH hb = CreateSolidBrush(RGB(24, 24, 24));
				for (int i = 0; i < trafficCount / 2 + 8; i++)
				{
					FillRect(mdc, &rc, hb);
					OffsetRect(&rc, 0, wTipsHeight * 2);
				}
				DeleteObject(hb);
				HPEN hp = CreatePen(PS_DOT, 1, RGB(98, 98, 98));
				HPEN oldpen = (HPEN)SelectObject(mdc, hp);
				MoveToEx(mdc, crc.right * 10 / 31, 0, NULL);
				LineTo(mdc, crc.right * 10 / 31, wTipsHeight * trafficCount);

				MoveToEx(mdc, crc.right * 57 / 100, 0, NULL);
				LineTo(mdc, crc.right * 57 / 100, wTipsHeight* trafficCount);
				MoveToEx(mdc, crc.right * 66 / 100, 0, NULL);
				LineTo(mdc, crc.right * 66 / 100, wTipsHeight* trafficCount);
				MoveToEx(mdc, crc.right * 78 / 100, 0, NULL);
				LineTo(mdc, crc.right * 78 / 100, wTipsHeight * trafficCount);
				MoveToEx(mdc, crc.right * 87 / 100, 0, NULL);
				LineTo(mdc, crc.right * 87 / 100, wTipsHeight * trafficCount);

				MoveToEx(mdc, crc.right * 100 / 122, wTipsHeight * trafficCount, NULL);
				LineTo(mdc, crc.right * 100 / 122, wTipsHeight * (trafficCount + 12));
				MoveToEx(mdc, crc.right * 100 / 145, wTipsHeight * trafficCount, NULL);
				LineTo(mdc, crc.right * 100 / 145, wTipsHeight * (trafficCount + 12));
				MoveToEx(mdc, crc.right * 100 / 156, wTipsHeight * trafficCount, NULL);
				LineTo(mdc, crc.right * 100 / 156, wTipsHeight * (trafficCount + 12));
				MoveToEx(mdc, crc.right * 100 / 178, wTipsHeight* trafficCount, NULL);
				LineTo(mdc, crc.right * 100 / 178, wTipsHeight* (trafficCount + 12));


				MoveToEx(mdc, 0, wTipsHeight * trafficCount, NULL);
				LineTo(mdc, crc.right, wTipsHeight * trafficCount);
				MoveToEx(mdc, 0, wTipsHeight * (trafficCount + 6), NULL);
				LineTo(mdc, crc.right, wTipsHeight * (trafficCount + 6));
				MoveToEx(mdc, 0, wTipsHeight * (trafficCount + 12), NULL);
				LineTo(mdc, crc.right, wTipsHeight * (trafficCount + 12));
				MoveToEx(mdc, crc.right * 8 / 100, wTipsHeight * (trafficCount + 12), NULL);
				LineTo(mdc, crc.right * 8 / 100, wTipsHeight * (trafficCount + 12 + 2));
				MoveToEx(mdc, crc.right * 92 / 100, wTipsHeight * (trafficCount + 12), NULL);
				LineTo(mdc, crc.right * 92 / 100, wTipsHeight * (trafficCount + 12 + 2));

				/*
							DeleteObject(hp);
							rc.bottom = wTipsHeight*nTraffic;
							rc.top = 1;
							rc.left = 5;
							rc.right = crc.right-5;
							DWORD max = 0;
							for (int in=0;in<rNum;in++)
							{
								if (max < s_in_bytes[in])
									max = s_in_bytes[in];
							}
							for (int out = 0; out < rNum; out++)
							{
								if (max < s_out_bytes[out])
									max = s_out_bytes[out];
							}
							if (max == 0)
								max = 100;
							hp = CreatePen(PS_DOT, 1, RGB(128, 255, 0));
							oldpen = (HPEN)SelectObject(mdc, hp);
							MoveToEx(mdc, rc.left, rc.bottom, NULL);
							for (int e=1;e<= rNum;e++)
							{
								int x = iBytes + e-1;
								if (x >= rNum)
									x -= rNum;
								LineTo(mdc, rc.left + (rc.right - rc.left) * e / rNum, rc.bottom - double((rc.bottom - rc.top) * s_out_bytes[x] / max ));
							}
							SelectObject(mdc, oldpen);
							DeleteObject(hp);

							hp = CreatePen(PS_SOLID, 1, RGB(255, 128, 0));
							oldpen = (HPEN)SelectObject(mdc, hp);
							MoveToEx(mdc, rc.left, rc.bottom, NULL);
							for (int e = 1; e <= rNum; e++)
							{
								int x = iBytes + e - 1;
								if (x >= rNum)
									x -= rNum;
								LineTo(mdc, rc.left + (rc.right - rc.left) *e / rNum, rc.bottom - double((rc.bottom - rc.top) * s_in_bytes[x] / max ));
							}
				*/

				SelectObject(mdc, oldpen);
				DeleteObject(hp);
				rc.top = 0;
				/*
							int cx;
							if (wTipsHeight < 24)
								cx = 16;
							else if (wTipsHeight < 32)
								cx = 24;
							else if (wTipsHeight < 48)
								cx = 32;
							else if (wTipsHeight < 64)
								cx = 48;
							else if (wTipsHeight < 128)
								cx = 64;
							else
								cx = 128;
				*/
				//			OffsetRect(&rc, 0, DPI(16)*3);
							//PIP_ADAPTER_INFO pai = &ipinfo[0];
				//			PIP_ADAPTER_ADDRESSES paa = &piaa[0];
				rc.bottom = wTipsHeight;
				for (int i = 0; i < trafficCount; i++)
				{
					rc.left = 5;// +wTipsHeight;
					DrawText(mdc, trafficData[i].FriendlyName, lstrlen(trafficData[i].FriendlyName), &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
					/*
									HICON hIcon = GetIconForCSIDL(CSIDL_CONNECTIONS);

									UINT size = MAKELPARAM(cx, cx);
									WCHAR szGuid[64];// = L":::";
									MultiByteToWideChar(CP_ACP, 0, traffic[i].AdapterName, 64, szGuid , 64);
									SHFILEINFO shfi;
									SHGetFileInfo(szGuid, 0, &shfi, sizeof(shfi), SHGFI_ICON | SHGFI_USEFILEATTRIBUTES);
									hIcon = shfi.hIcon;

									if (wTipsHeight >= 16)
										DrawIconEx(mdc, 2, rc.top + (rc.bottom - rc.top - cx) / 2, hIcon, cx, cx, 0, NULL, DI_NORMAL);
									else
										DrawIconEx(mdc, 1, rc.top + 1, hIcon, wTipsHeight - 2, wTipsHeight - 2, 0, NULL, DI_NORMAL);
									DestroyIcon(hIcon);
					*/					
					rc.left = crc.right * 10 / 31;
					rc.right = crc.right * 57 / 100;
					DrawText(mdc, trafficData[i].IP4, lstrlen(trafficData[i].IP4), &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
					rc.left = crc.right * 57 / 100;
					rc.right = crc.right * 66 / 100 - 2;
					GetTrafficStr(sz, trafficData[i].in_bytes, HIWORD(displaySettings.iUnit));
					DrawText(mdc, sz, lstrlen(sz), &rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
					GetTrafficStr(sz, trafficData[i].in_byte, HIWORD(displaySettings.iUnit));
					rc.left = crc.right * 66 / 100 + 2;
					rc.right = crc.right * 78 / 100-2;
					DrawText(mdc, L"↓:", 2, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
					DrawText(mdc, sz, lstrlen(sz), &rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
					rc.left = crc.right * 78 / 100 + 2;
					rc.right = crc.right * 87 / 100 - 2;
					GetTrafficStr(sz, trafficData[i].out_bytes, HIWORD(displaySettings.iUnit));
					DrawText(mdc, sz, lstrlen(sz), &rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
					GetTrafficStr(sz, trafficData[i].out_byte, HIWORD(displaySettings.iUnit));
					rc.left = crc.right * 87 / 100 + 2;
					rc.right = crc.right - 5;
					DrawText(mdc, L"↑:", 2, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
					DrawText(mdc, sz, lstrlen(sz), &rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
					OffsetRect(&rc, 0, wTipsHeight);
				}
				rc.left = 5;// +wTipsHeight;
				rc.right = crc.right - 5;
				POINT pt;
				GetCursorPos(&pt);
				ScreenToClient(hDlg, &pt);
				for (int i = 0; i < 6; i++)
				{
					SetTextColor(mdc, RGB(192, 192, 0));
					DrawText(mdc, cpuProcesses[i].szExe, lstrlen(cpuProcesses[i].szExe), &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
					/*
									HICON hIcon = OpenProcessIcon(cpuProcesses[i].dwProcessID,cx);
									if(wTipsHeight>=16)
										DrawIconEx(mdc, 2, rc.top + (rc.bottom - rc.top - cx) / 2, hIcon, cx, cx, 0, NULL, DI_NORMAL);
									else
										DrawIconEx(mdc, 1, rc.top+1, hIcon, wTipsHeight-2, wTipsHeight-2, 0, NULL, DI_NORMAL);
									DestroyIcon(hIcon);
					*/
					int iCpuUsage = int(cpuProcesses[i].fCpuUsage * 100);
					wsprintf(sz, L"%d.%.2d%%", iCpuUsage / 100, iCpuUsage % 100);
					DrawText(mdc, sz, lstrlen(sz), &rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
					RECT cr = rc;
					cr.left = crc.right * 100 / 145;
					cr.right = crc.right * 100 / 122;
					wsprintf(sz, L"%d", cpuProcesses[i].dwProcessID);
					DrawText(mdc, sz, lstrlen(sz), &cr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
					cr.left = crc.right * 100 / 156;
					cr.right = crc.right * 100 / 145;
					if (PtInRect(&cr, pt))
						SetTextColor(mdc, RGB(255, 255, 255));
					DrawText(mdc, L"X", 1, &cr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
					SetTextColor(mdc, RGB(192, 192, 0));
					cr.left = crc.right * 100 / 178;
					cr.right = crc.right * 100 / 156;
					if (PtInRect(&cr, pt))
						SetTextColor(mdc, RGB(255, 255, 255));
					DrawText(mdc, L"路径", 2, &cr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
					OffsetRect(&rc, 0, wTipsHeight);
				}
				for (int i = 0; i < 6; i++)
				{
					SetTextColor(mdc, RGB(0, 192, 192));
					DrawText(mdc, memoryProcesses[i].szExe, lstrlen(memoryProcesses[i].szExe), &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
					/*
									HICON hIcon = OpenProcessIcon(memoryProcesses[i].dwProcessID,cx);
									if(wTipsHeight>=16)
										DrawIconEx(mdc, 2, rc.top+(rc.bottom-rc.top-cx)/2, hIcon, cx, cx, 0, NULL, DI_NORMAL);
									else
										DrawIconEx(mdc, 1, rc.top+1, hIcon, wTipsHeight-2, wTipsHeight-2, 0, NULL, DI_NORMAL);
									DestroyIcon(hIcon);
					*/
					if (memoryProcesses[i].dwMemUsage >= 1048576000)
					{
						SIZE_T iMemUsage = (memoryProcesses[i].dwMemUsage * 100 / 1073741824);
						wsprintf(sz, L"%d.%.2dGB", iMemUsage / 100, iMemUsage % 100);
					}
					else
					{
						SIZE_T iMemUsage = (memoryProcesses[i].dwMemUsage * 100 / 1048576);
						wsprintf(sz, L"%d.%.2dMB", iMemUsage / 100, iMemUsage % 100);
					}
					DrawText(mdc, sz, lstrlen(sz), &rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
					RECT cr = rc;
					cr.left = crc.right * 100 / 145;
					cr.right = crc.right * 100 / 122;
					wsprintf(sz, L"%d", memoryProcesses[i].dwProcessID);
					DrawText(mdc, sz, lstrlen(sz), &cr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
					cr.left = crc.right * 100 / 156;
					cr.right = crc.right * 100 / 145;
					if (PtInRect(&cr, pt))
						SetTextColor(mdc, RGB(255, 255, 255));
					DrawText(mdc, L"X", 1, &cr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
					SetTextColor(mdc, RGB(0, 192, 192));
					cr.left = crc.right * 100 / 178;
					cr.right = crc.right * 100 / 156;
					if (PtInRect(&cr, pt))
						SetTextColor(mdc, RGB(255, 255, 255));
					DrawText(mdc, L"路径", 2, &cr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
					OffsetRect(&rc, 0, wTipsHeight);
				}

				HBRUSH hb1, hb2, hb3, hb4;
				hb1 = CreateSolidBrush(RGB(168, 168, 168));
				hb2 = CreateSolidBrush(RGB(128, 0, 0));
				hb3 = CreateSolidBrush(RGB(0, 128, 198));
				hb4 = CreateSolidBrush(RGB(0, 148, 0));
				SetTextColor(mdc, RGB(255, 255, 255));
				/*
							HBRUSH hb;
							hb=CreateSolidBrush(RGB(0,38,0));
							FillRect(mdc, &rc, hb);
							DeleteObject(hb);
				*/
				WCHAR wDrive[MAX_PATH];
				DWORD dwLen = GetLogicalDriveStrings(MAX_PATH, wDrive);
				if (dwLen != 0)
				{
					DWORD driver_number = dwLen / 4;
					rc.left = crc.right * 8 / 100 + 3;
					int dw = crc.right * 84 / 100 / driver_number - 2;
					rc.right = rc.left + dw;
					rc.top += 3;
					rc.bottom -= 1;
					for (DWORD nIndex = 0; nIndex < driver_number; nIndex++)
					{
						LPWSTR dName = wDrive + nIndex * 4;
						UINT64 lpFreeBytesAvailable = 0;
						UINT64 lpTotalNumberOfBytes = 0;
						UINT64 lpTotalNumberOfFreeBytes = 0;
						if (GetDriveType(dName) != DRIVE_CDROM && dName[0] != L'A')
						{
							if (GetDiskFreeSpaceEx(dName, (PULARGE_INTEGER)&lpFreeBytesAvailable, (PULARGE_INTEGER)&lpTotalNumberOfBytes, (PULARGE_INTEGER)&lpTotalNumberOfFreeBytes))
							{
								RECT frc = rc;
								if (nIndex + 1 == driver_number)
									rc.right = crc.right * 92 / 100 - 2;
								frc.right = frc.left + (LONG)((LONGLONG)(rc.right - rc.left) *
									(lpTotalNumberOfBytes - lpTotalNumberOfFreeBytes) / lpTotalNumberOfBytes);
								FillRect(mdc, &rc, hb1);
								if (lpTotalNumberOfFreeBytes < lpTotalNumberOfBytes * 1 / 10)
									FillRect(mdc, &frc, hb2);
								else
									FillRect(mdc, &frc, hb3);
							}
							dName[2] = 0;
						}
						DrawText(mdc, dName, lstrlen(dName), &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
						OffsetRect(&rc, dw + 2, 0);
						if (rc.right - 3 > crc.right * 92 / 100)
							break;
					}
				}
				rc.top -= 2;
				rc.bottom -= 2;
				OffsetRect(&rc, 0, wTipsHeight);
				rc.left = crc.right * 50 / 100 + 1;
				rc.right = crc.right * 92 / 100 - 2;
				FillRect(mdc, &rc, hb1);

				rc.left = crc.right * 8 / 100 + 3;
				rc.right = crc.right * 50 / 100 - 1;
				FillRect(mdc, &rc, hb1);
				/*
							PROCESSOR_POWER_INFORMATION* pi = (PROCESSOR_POWER_INFORMATION*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof PROCESSOR_POWER_INFORMATION*dNumProcessor);
							if (pCallNtPowerInformation(ProcessorInformation, NULL, 0, &pi[0], sizeof PROCESSOR_POWER_INFORMATION * dNumProcessor) == 0)
							{
								int iCGhz = pi[0].CurrentMhz / 10;
								int iMGhz = pi[0].MaxMhz / 10;
								wsprintf(sz,  L"%d个逻辑处理器 当前频率%d.%.2dGHz 最大频率%d.%.2dGHz", dNumProcessor, iCGhz/100,iCGhz%100, iMGhz / 100, iMGhz % 100);
							}
							HeapFree(GetProcessHeap(), 0,pi);
							DrawText(mdc, sz, lstrlen(sz), &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
				*/

				DWORDLONG iaPage = memoryStatus.ullAvailPageFile * 100 / 1073741824;
				DWORDLONG itPage = memoryStatus.ullTotalPageFile * 100 / 1073741824;
				DWORDLONG ia = memoryStatus.ullAvailPhys * 100 / 1073741824;
				DWORDLONG it = memoryStatus.ullTotalPhys * 100 / 1073741824;
				if (itPage == 0)
					itPage = 1;
				if (it == 0)
					it = 1;

				RECT frc = rc;
				frc.right = frc.left + (LONG)((LONGLONG)(rc.right - rc.left) * (itPage - iaPage) / itPage);
				if (iaPage < itPage * 2 / 10)
					FillRect(mdc, &frc, hb2);
				else
					FillRect(mdc, &frc, hb4);
				wsprintf(sz, L"虚拟内存:%d.%.2d/%d.%.2dGB", iaPage / 100, iaPage % 100, itPage / 100, itPage % 100);
				DrawText(mdc, sz, lstrlen(sz), &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
				rc.left = crc.right * 50 / 100 + 1;
				rc.right = crc.right * 92 / 100 - 2;
				frc = rc;
				frc.right = frc.left + (LONG)((LONGLONG)(rc.right - rc.left) * (it - ia) / it);
				if (ia < it * 2 / 10)
					FillRect(mdc, &frc, hb2);
				else
					FillRect(mdc, &frc, hb4);
				DeleteObject(hb1);
				DeleteObject(hb2);
				DeleteObject(hb3);
				DeleteObject(hb4);
				wsprintf(sz, L"物理内存:%d.%.2d/%d.%.2dGB", ia / 100, ia % 100, it / 100, it % 100);
				DrawText(mdc, sz, lstrlen(sz), &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

				WCHAR set[] = L"设置";
				WCHAR sexit[] = L"退出";
				rc.top -= wTipsHeight;
				rc.right = crc.right * 8 / 100;
				rc.left = 0;
				DrawText(mdc, set, lstrlen(set), &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
				rc.right = crc.right;
				rc.left = crc.right * 92 / 100;
				DrawText(mdc, sexit, lstrlen(sexit), &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
				DeleteObject(hTipsFont);
				SelectObject(mdc, oldFont);
			}
			GetClientRect(hDlg, &rc);
			BitBlt(hdc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, mdc, 0, 0, SRCCOPY);
			SelectObject(mdc, oldBmp);
			DeleteObject(hMemBmp);
			DeleteDC(mdc);
		}
		return TRUE;
		break;
	}
	return (INT_PTR)FALSE;
}
void GetProcessCpuUsage()//获取进程CPU占用前五
{
	for (int i = 0; i < 6; ++i)
		ppcuWork[i] = &pcuWork[i];
	memset(pcuWork, 0, sizeof pcuWork);
	DWORD dCurID = GetCurrentProcessId();
	PROCESSENTRY32 pe;
	pe.dwSize = sizeof(PROCESSENTRY32);
	HANDLE hs = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (hs != INVALID_HANDLE_VALUE)
	{
		BOOL ret = Process32First(hs, &pe);
		while (ret)
		{
			if (pe.th32ProcessID != dCurID)
			{
				HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
				if (hProc)
				{
					int n = -1;
					for (int i = 0; i < nProcessTimeCapacity; i++)
					{
						if (pProcessTime[i].dwProcessID == pe.th32ProcessID)
						{
							n = i;
							break;
						}
						else if (n == -1 && pProcessTime[i].dwProcessID == NULL)
							n = i;
					}
					FILETIME CreateTime, ExitTime, KernelTime, UserTime;
					if (n >= 0 && pProcessTime && GetProcessTimes(hProc, &CreateTime, &ExitTime, &KernelTime, &UserTime))
					{
						float nProcCpuPercent = 0;
						BOOL bRetCode = FALSE;
						FILETIME CreateTime, ExitTime, KernelTime, UserTime;
						LARGE_INTEGER lgKernelTime;
						LARGE_INTEGER lgUserTime;
						LARGE_INTEGER lgCurTime;

						bRetCode = GetProcessTimes(hProc, &CreateTime, &ExitTime, &KernelTime, &UserTime);
						if (bRetCode)
						{
							lgKernelTime.HighPart = KernelTime.dwHighDateTime;
							lgKernelTime.LowPart = KernelTime.dwLowDateTime;

							lgUserTime.HighPart = UserTime.dwHighDateTime;
							lgUserTime.LowPart = UserTime.dwLowDateTime;

							lgCurTime.QuadPart = (lgKernelTime.QuadPart + lgUserTime.QuadPart) / 10000;
							if (pProcessTime[n].g_slgProcessTimeOld.QuadPart == 0)
								nProcCpuPercent = 0;
							else
								nProcCpuPercent = (float)((lgCurTime.QuadPart - pProcessTime[n].g_slgProcessTimeOld.QuadPart) * 100 / 1000);
							pProcessTime[n].g_slgProcessTimeOld = lgCurTime;
							pProcessTime[n].dwProcessID = pe.th32ProcessID;
							nProcCpuPercent = nProcCpuPercent / dNumProcessor;
						}
						else
						{
							nProcCpuPercent = 0;
						}
						if (nProcCpuPercent > 100)
							nProcCpuPercent = 0;
						if (TRUE)
						{
							PROCESSCPUUSAGE* ppc;
							if (ppcuWork[0]->fCpuUsage <= nProcCpuPercent)
							{
								ppc = ppcuWork[5];
								ppcuWork[5] = ppcuWork[4];
								ppcuWork[4] = ppcuWork[3];
								ppcuWork[3] = ppcuWork[2];
								ppcuWork[2] = ppcuWork[1];
								ppcuWork[1] = ppcuWork[0];
								ppcuWork[0] = ppc;
								ppcuWork[0]->dwProcessID = pe.th32ProcessID;
								ppcuWork[0]->fCpuUsage = nProcCpuPercent;
								lstrcpyn(ppcuWork[0]->szExe, pe.szExeFile, 36);
							}
							else if (ppcuWork[1]->fCpuUsage <= nProcCpuPercent)
							{
								ppc = ppcuWork[5];
								ppcuWork[5] = ppcuWork[4];
								ppcuWork[4] = ppcuWork[3];
								ppcuWork[3] = ppcuWork[2];
								ppcuWork[2] = ppcuWork[1];
								ppcuWork[1] = ppc;
								ppcuWork[1]->dwProcessID = pe.th32ProcessID;
								ppcuWork[1]->fCpuUsage = nProcCpuPercent;
								lstrcpyn(ppcuWork[1]->szExe, pe.szExeFile, 36);
							}
							else if (ppcuWork[2]->fCpuUsage <= nProcCpuPercent)
							{
								ppc = ppcuWork[5];
								ppcuWork[5] = ppcuWork[4];
								ppcuWork[4] = ppcuWork[3];
								ppcuWork[3] = ppcuWork[2];
								ppcuWork[2] = ppc;
								ppcuWork[2]->dwProcessID = pe.th32ProcessID;
								ppcuWork[2]->fCpuUsage = nProcCpuPercent;
								lstrcpyn(ppcuWork[2]->szExe, pe.szExeFile, 36);
							}
							else if (ppcuWork[3]->fCpuUsage <= nProcCpuPercent)
							{
								ppc = ppcuWork[5];
								ppcuWork[5] = ppcuWork[4];
								ppcuWork[4] = ppcuWork[3];
								ppcuWork[3] = ppc;
								ppcuWork[3]->dwProcessID = pe.th32ProcessID;
								ppcuWork[3]->fCpuUsage = nProcCpuPercent;
								lstrcpyn(ppcuWork[3]->szExe, pe.szExeFile, 36);
							}
							else if (ppcuWork[4]->fCpuUsage <= nProcCpuPercent)
							{
								ppc = ppcuWork[5];
								ppcuWork[5] = ppcuWork[4];
								ppcuWork[4] = ppc;
								ppcuWork[4]->dwProcessID = pe.th32ProcessID;
								ppcuWork[4]->fCpuUsage = nProcCpuPercent;
								lstrcpyn(ppcuWork[4]->szExe, pe.szExeFile, 36);
							}
							else if (ppcuWork[5]->fCpuUsage <= nProcCpuPercent)
							{
								ppcuWork[5]->dwProcessID = pe.th32ProcessID;
								ppcuWork[5]->fCpuUsage = nProcCpuPercent;
								lstrcpyn(ppcuWork[5]->szExe, pe.szExeFile, 36);
							}
						}
					}
					CloseHandle(hProc);
				}
			}
			ret = Process32Next(hs, &pe);
		}
		CloseHandle(hs);
	}
}
int GetProcessMemUsage()//获取进程内存占用前五
{
	for (int i = 0; i < 6; ++i)
		ppmuWork[i] = &pmuWork[i];
	memset(pmuWork, 0, sizeof pmuWork);
	PROCESSENTRY32 pe;
	pe.dwSize = sizeof(PROCESSENTRY32);
	int n = 0;
	HANDLE hs = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (hs != INVALID_HANDLE_VALUE)
	{
		BOOL ret = Process32First(hs, &pe);
		while (ret)
		{
			++n;
			if (lstrcmp(pe.szExeFile, L"Memory Compression") != 0)
			{
				HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
				if (hProc)
				{
					PROCESS_MEMORY_COUNTERS_EX pmc;
					if (GetProcessMemoryInfo(hProc, (PPROCESS_MEMORY_COUNTERS)&pmc, sizeof(pmc)))
					{
						//QueryWorkingSet()
						PROCESSMEMORYUSAGE* ppm;
						if (ppmuWork[0]->dwMemUsage <= pmc.WorkingSetSize)
						{
							ppm = ppmuWork[5];
							ppmuWork[5] = ppmuWork[4];
							ppmuWork[4] = ppmuWork[3];
							ppmuWork[3] = ppmuWork[2];
							ppmuWork[2] = ppmuWork[1];
							ppmuWork[1] = ppmuWork[0];
							ppmuWork[0] = ppm;
							ppmuWork[0]->dwProcessID = pe.th32ProcessID;
							ppmuWork[0]->dwMemUsage = pmc.WorkingSetSize;
							lstrcpyn(ppmuWork[0]->szExe, pe.szExeFile, 36);

						}
						else if (ppmuWork[1]->dwMemUsage <= pmc.WorkingSetSize)
						{
							ppm = ppmuWork[5];
							ppmuWork[5] = ppmuWork[4];
							ppmuWork[4] = ppmuWork[3];
							ppmuWork[3] = ppmuWork[2];
							ppmuWork[2] = ppmuWork[1];
							ppmuWork[1] = ppm;
							ppmuWork[1]->dwProcessID = pe.th32ProcessID;
							ppmuWork[1]->dwMemUsage = pmc.WorkingSetSize;
							lstrcpyn(ppmuWork[1]->szExe, pe.szExeFile, 36);

						}
						else if (ppmuWork[2]->dwMemUsage <= pmc.WorkingSetSize)
						{
							ppm = ppmuWork[5];
							ppmuWork[5] = ppmuWork[4];
							ppmuWork[4] = ppmuWork[3];
							ppmuWork[3] = ppmuWork[2];
							ppmuWork[2] = ppm;
							ppmuWork[2]->dwProcessID = pe.th32ProcessID;
							ppmuWork[2]->dwMemUsage = pmc.WorkingSetSize;
							lstrcpyn(ppmuWork[2]->szExe, pe.szExeFile, 36);

						}
						else if (ppmuWork[3]->dwMemUsage <= pmc.WorkingSetSize)
						{
							ppm = ppmuWork[5];
							ppmuWork[5] = ppmuWork[4];
							ppmuWork[4] = ppmuWork[3];
							ppmuWork[3] = ppm;
							ppmuWork[3]->dwProcessID = pe.th32ProcessID;
							ppmuWork[3]->dwMemUsage = pmc.WorkingSetSize;
							lstrcpyn(ppmuWork[3]->szExe, pe.szExeFile, 36);
						}
						else if (ppmuWork[4]->dwMemUsage <= pmc.WorkingSetSize)
						{
							ppm = ppmuWork[5];
							ppmuWork[5] = ppmuWork[4];
							ppmuWork[4] = ppm;
							ppmuWork[4]->dwProcessID = pe.th32ProcessID;
							ppmuWork[4]->dwMemUsage = pmc.WorkingSetSize;
							lstrcpyn(ppmuWork[4]->szExe, pe.szExeFile, 36);
						}
						else if (ppmuWork[5]->dwMemUsage <= pmc.WorkingSetSize)
						{
							ppmuWork[5]->dwProcessID = pe.th32ProcessID;
							ppmuWork[5]->dwMemUsage = pmc.WorkingSetSize;
							lstrcpyn(ppmuWork[5]->szExe, pe.szExeFile, 36);
						}
					}
					CloseHandle(hProc);
				}
			}
			ret = Process32Next(hs, &pe);
		}
		CloseHandle(hs);
	}
	return n;
}
void DrawDisk(HDC mdc, LPRECT lpRect, double dwByte, BOOL bReadWrite, const TRAYSAVE& settings)
{
	const WCHAR szWriteS[] = L"X:";
	const WCHAR szWriteS2[] = L"";
	const WCHAR szReadS[] = L"R:";
	const WCHAR szReadS2[] = L"";
	const WCHAR* szT;
	if (bReadWrite)
	{
		if (settings.iMonitorSimple == 1)
			szT = szWriteS;
		else if (settings.iMonitorSimple == 2)
			szT = szWriteS2;
		else
			szT = settings.szDiskWriteSec;
	}
	else
	{
		if (settings.iMonitorSimple == 1)
			szT = szReadS;
		else if (settings.iMonitorSimple == 2)
			szT = szReadS2;
		else
			szT = settings.szDiskReadSec;
	}
	WCHAR sz[24];
	COLORREF rgb;
	if (dwByte/1024/1024 < settings.dNumValues2[0])
		rgb = settings.cMonitorColor[1];
	else if (dwByte/1024/1024 < settings.dNumValues2[1])
		rgb = settings.cMonitorColor[2];
	else
		rgb = settings.cMonitorColor[3];
	SetTextColor(mdc, rgb);
	double f_byte = dwByte;
	if (dwByte < 1048576000)
	{		
		f_byte /= 1048576;
		int m_byte = int(f_byte * 100);
		if (f_byte >= 100)
			wsprintf(sz, L"%dM",  m_byte / 100);
		else if (f_byte >= 10)
			wsprintf(sz, L"%d.%.1dM", m_byte / 100, (m_byte / 10) % 10);
		else
			wsprintf(sz, L"%d.%.2dM", m_byte / 100, m_byte % 100);
	}
	else
	{
		f_byte /= 1073741824;
		int g_byte = int(f_byte * 100);
		if (f_byte >= 100)
			wsprintf(sz, L"%dG", g_byte / 100);
		else if (f_byte >= 10)
			wsprintf(sz, L"%d.%.1dG", g_byte / 100, (g_byte / 10) % 10);
		else
			wsprintf(sz, L"%d.%.2dG", g_byte / 100, g_byte % 100);
	}
	DrawShadowText(mdc, szT, lstrlen(szT), lpRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
	DrawShadowText(mdc, sz, lstrlen(sz), lpRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
}
void DrawTraffic(HDC mdc, LPRECT lpRect, ULONG64 dwByte, BOOL bInOut, const TRAYSAVE& settings)
{
	const WCHAR szInS[] = L"↓:";
	const WCHAR szInS2[] = L"";
	const WCHAR szOutS[] = L"↑:";
	const WCHAR szOutS2[] = L"";
	const WCHAR* szT;
	if (bInOut)
	{
		if (settings.iMonitorSimple == 1)
			szT = szInS;
		else if (settings.iMonitorSimple == 2)
			szT = szInS2;
		else
			szT = settings.szTrafficIn;
	}
	else
	{
		if (settings.iMonitorSimple == 1)
			szT = szOutS;
		else if (settings.iMonitorSimple == 2)
			szT = szOutS2;
		else
			szT = settings.szTrafficOut;
	}
	WCHAR sz[24];
	COLORREF rgb;
	if (dwByte < settings.dNumValues[0])
		rgb = settings.cMonitorColor[1];
	else if (dwByte < settings.dNumValues[1])
		rgb = settings.cMonitorColor[2];
	else
		rgb = settings.cMonitorColor[3];
	SetTextColor(mdc, rgb);
	GetTrafficStr(sz, dwByte, HIWORD(settings.iUnit),LOWORD(settings.iUnit));
	DrawShadowText(mdc, szT, lstrlen(szT), lpRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
	DrawShadowText(mdc, sz, lstrlen(sz), lpRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
}
//extern "C" WINUSERAPI BOOL WINAPI TrackMouseEvent(LPTRACKMOUSEEVENT lpEventTrack);
BOOL bEvent = FALSE;//
BOOL SetTrackMouseEvent(HWND hWnd, DWORD dwFlags)
{
	TRACKMOUSEEVENT csTME;
	csTME.cbSize = sizeof(csTME);
	csTME.dwFlags = dwFlags;
	csTME.hwndTrack = hWnd;// 指定要 追踪 的窗口
	csTME.dwHoverTime = 300;  // 鼠标在按钮上停留超过 300ms ，才认为状态为 HOVER
	return TrackMouseEvent(&csTME);
}
INT_PTR CALLBACK TimeProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)//任务栏信息窗口过程
{
	switch (message)
	{
	case WM_INITDIALOG:
		return (INT_PTR)TRUE;
	case WM_ERASEBKGND:
	{
		//		PAINTSTRUCT ps;
		HDC hdc = (HDC)wParam;//BeginPaint(hDlg, &ps);		
		RECT rc;
		GetClientRect(hDlg, &rc);
		HDC mdc = CreateCompatibleDC(hdc);
		if (mdc)
		{
			HBITMAP hMemBmp = CreateCompatibleBitmap(hdc, rc.right - rc.left, rc.bottom - rc.top);
			HBITMAP oldBmp = (HBITMAP)SelectObject(mdc, hMemBmp);

			COLORREF rgb(RGB(255, 255, 255));
			COLORREF cBack = RGB(0, 0, 1);
			if (bThemeMode != 0)
			{
				rgb = RGB(8, 8, 8);
				//			if(rovi.dwBuildNumber>22000)
				//				cBack = RGB(254, 254, 255);
			}
			if (hWin11UI)
			{
				HBRUSH hb = CreateSolidBrush(cBack);
				FillRect(mdc, &rc, hb);
				DeleteObject(hb);
			}
			SYSTEMTIME systm;
			GetLocalTime(&systm);
			WCHAR sz[16];
			TCHAR szWeek[7][2] = { L"日",L"一",L"二",L"三",L"四",L"五",L"六" };

			int fsize;
			if (hWin11UI)
			{
				fsize = DPI(-11);
				wsprintf(sz, L"%.2d'%s", systm.wSecond, szWeek[systm.wDayOfWeek]);
			}
			else
			{
				fsize = DPI(-12);
				wsprintf(sz, L"%s%.2d:%.2d:%.2d", szWeek[systm.wDayOfWeek], systm.wHour, systm.wMinute, systm.wSecond);
			}
			HFONT hFont = CreateFont(fsize, 0, 0, 0, 0, false, false, false,
				DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
				CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
				DEFAULT_PITCH, L"微软雅黑");
			HFONT oldFont = (HFONT)SelectObject(mdc, hFont);
			SetBkMode(mdc, TRANSPARENT);
			SetTextColor(mdc, rgb);
			if (!hWin11UI)
				DrawText(mdc, sz, lstrlen(sz), &rc, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
			else
				DrawText(mdc, sz, 4, &rc, DT_LEFT | DT_SINGLELINE | DT_BOTTOM);
			SelectObject(mdc, oldFont);
			DeleteObject(hFont);

			if (!hWin11UI)
			{
				BYTE* lpvBits = NULL;
				BITMAPINFO binfo;
				memset(&binfo, 0, sizeof(BITMAPINFO));
				binfo.bmiHeader.biBitCount = 32;     //每个像素多少位，也可直接写24(RGB)或者32(RGBA)
				binfo.bmiHeader.biCompression = 0;
				binfo.bmiHeader.biHeight = rc.bottom - rc.top;
				binfo.bmiHeader.biPlanes = 1;
				binfo.bmiHeader.biSizeImage = (rc.bottom - rc.top) * (rc.right - rc.left) * 4;
				binfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
				binfo.bmiHeader.biWidth = rc.right - rc.left;
				lpvBits = (BYTE*)HeapAlloc(GetProcessHeap(), NULL, binfo.bmiHeader.biSizeImage);
				//		GetDIBits(mdc, hMemBmp, 0, rc.bottom - rc.top, lpvBits, &bmpInfo, DIB_RGB_COLORS);
				if (lpvBits)
				{
					GetDIBits(mdc, hMemBmp, 0, rc.bottom - rc.top, lpvBits, &binfo, DIB_RGB_COLORS);
					for (DWORD i = 0; i < binfo.bmiHeader.biSizeImage - 4; i += 4)
					{
						if (lpvBits[i] > 3 || lpvBits[i + 1] != 0 || lpvBits[i + 2] != 0)
							lpvBits[i + 3] = 0x80;
					}
					SetDIBits(mdc, hMemBmp, 0, rc.bottom - rc.top, lpvBits, &binfo, DIB_RGB_COLORS);
					HeapFree(GetProcessHeap(), 0, lpvBits);
				}
			}
			BitBlt(hdc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, mdc, 0, 0, SRCCOPY);
			SelectObject(mdc, oldBmp);
			DeleteObject(hMemBmp);
			DeleteDC(mdc);
		}
		return TRUE;
	}
	break;
	}
	return FALSE;
}

INT_PTR CALLBACK TaskBarProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)//任务栏信息窗口过程
{
	TRAYDATA monitorData = { 0 };
	MEMORYSTATUSEX memoryStatus = { 0 };
	DWORD cpuUsage = 0;
	ReadMonitorDataSnapshot(&monitorData, &memoryStatus, &cpuUsage);
	TRAYSAVE displaySettings = { 0 };
	ReadMonitorSettings(&displaySettings);
	switch (message)
	{
	case WM_INITDIALOG:		
		return (INT_PTR)TRUE;
	case WM_COMMAND:
		if (LOWORD(wParam) >= IDC_SELECT_ALL && LOWORD(wParam) <= IDC_SELECT_ALL + 99)
		{
			if (LOWORD(wParam) == IDC_SELECT_ALL)
				TraySave.AdpterName[0] = '\0';
			else
			{
				TRAFFIC* trafficData = (TRAFFIC*)HeapAlloc(
					GetProcessHeap(),
					HEAP_ZERO_MEMORY,
					sizeof(TRAFFIC) * MAX_TRAFFIC_ADAPTERS);
				if (!trafficData)
					break;
				int trafficCount = ReadTrafficSnapshot(trafficData, MAX_TRAFFIC_ADAPTERS);
				int x = LOWORD(wParam) - IDC_SELECT_ALL - 1;
				if (x < 0 || x >= trafficCount)
				{
					HeapFree(GetProcessHeap(), 0, trafficData);
					break;
				}
				lstrcpynA(TraySave.AdpterName, trafficData[x].AdapterName, ARRAYSIZE(TraySave.AdpterName));
				HeapFree(GetProcessHeap(), 0, trafficData);
			}
			WriteReg();
			ResetTrafficMonitorData();
		}
		else if (LOWORD(wParam) >= IDC_DISK_ALL && LOWORD(wParam) <= IDC_DISK_ALL + 99)
		{
			if (LOWORD(wParam) == IDC_DISK_ALL)
				TraySave.szDisk = L'\0';
			else
			{
				TraySave.szDisk = LOWORD(wParam) - IDC_DISK_ALL;
			}
			WriteReg();
			SwitchPDH(FALSE);
			SwitchPDH(TRUE);
			ResetDiskMonitorData();
		}
		break;
	case WM_MOUSEMOVE:
		if (bEvent == FALSE && TraySave.bMonitorTips)
		{
			SetTrackMouseEvent(hTaskBar, TME_LEAVE | TME_HOVER);
			bEvent = TRUE;
		}
		break;
	case WM_MOUSEHOVER:
	{
		POINT pt;
		RECT rc;
		GetCursorPos(&pt);
		GetWindowRect(hTaskBar,&rc);
		if (!IsWindowVisible(hTaskTips)&&TraySave.bMonitorTips)
		{
/*
			if (s_in_byte == 0)
				return FALSE;
*/			
			if (!IsWindow(hTaskTips))
			{
				hTaskTips = ::CreateDialog(hInst, MAKEINTRESOURCE(IDD_TIPS), NULL, (DLGPROC)TaskTipsProc);
				SetLayeredWindowAttributes(hTaskTips, 0, 255, LWA_ALPHA);
			}
				HDC mdc = GetDC(hMain);
			TraySave.TipsFont.lfHeight = DPI(TraySave.TipsFontSize);
			HFONT hTipsFont = CreateFontIndirect(&TraySave.TipsFont); //创建字体
			HFONT oldFont = (HFONT)SelectObject(mdc, hTipsFont);
			SIZE tSize;
			::GetTextExtentPoint(mdc, L"虚拟内存虚拟内存虚拟内存虚拟内存虚拟内存虚拟内存虚拟内存虚拟内存虚拟内存", 36, &tSize);
			SelectObject(mdc, oldFont);
			DeleteObject(hTipsFont);
			::ReleaseDC(hMain, mdc);
			int x, y, w, h;
			w = tSize.cx;
			wTipsHeight = tSize.cy;
			h = wTipsHeight * (ReadTrafficSnapshot(NULL, 0) + 14);
			RECT wrc, src;
			GetWindowRect(hDlg, &wrc);
			GetScreenRect(hDlg, &src, TRUE);
			if (wrc.bottom + h > src.bottom)
				y = wrc.top - h;
			else
				y = wrc.bottom;
			if (wrc.right - (wrc.right - wrc.left) / 2 + w / 2 > src.right)
				x = src.right - w;
			else if (wrc.right - (wrc.right - wrc.left) / 2 - w / 2 < src.left)
				x = src.left;
			else
				x = wrc.right - (wrc.right - wrc.left) / 2 - w / 2;
			SetWindowPos(hTaskTips, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
			HRGN hRgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, 11, 11);
			SetWindowRgn(hTaskTips, hRgn, FALSE);
//			SetCursorPos(2498, 1398);
//			mouse_event(MOUSEEVENTF_MOVE, 2380, 1398, 0, 0);
//			mouse_event(MOUSEEVENTF_LEFTDOWN, 2500, 1398, 0, 0);
//			mouse_event(MOUSEEVENTF_LEFTUP, 2500, 1398, 0, 0);
		}
	}
	break;
	case WM_MOUSELEAVE:
		bEvent = FALSE;
		break;
	case  WM_RBUTTONDOWN:
	{
		POINT pt;
		RECT rc;
		GetCursorPos(&pt);
		ScreenToClient(hDlg, &pt);
		GetClientRect(hTaskBar, &rc);
		BOOL bV = FALSE;
		if (rc.bottom - rc.top > rc.right - rc.left)
		{
			bV = TRUE;
			rc.bottom = wHeight * 2;
		}
		else
			rc.right = wTraffic;
		if (TraySave.bMonitorTraffic&&PtInRect(&rc,pt))
		{
			ShowSelectMenu(TRUE);
			return true;
		}
		if (bV)
		{
			OffsetRect(&rc, 0, (TraySave.bMonitorTraffic + TraySave.bMonitorUsage + TraySave.bMonitorTemperature) * 2 * wHeight);
			if (!bRing0)
				OffsetRect(&rc, 0, -wHeight);
			if (hOHMA && TraySave.bMonitorTemperature)
				rc.bottom += wHeight * 2;
		}
		else
		{
			OffsetRect(&rc, wTraffic + wUsage + wTemperature, 0);
			rc.right = rc.left + wDisk;
		}
		if (TraySave.bMonitorDisk && PtInRect(&rc, pt))
		{
			ShowSelectMenu(FALSE);
			return true;
		}
		if (bV)
		{
			OffsetRect(&rc, 0, wHeight*2);
		}
		else
		{
			OffsetRect(&rc, wDisk, 0);
			rc.right = rc.left + wTime;
		}
		if (TraySave.bMonitorTime && PtInRect(&rc, pt))
			RunProcess(NULL, szTimeDateCpl);
		else
			OpenSetting();
		return TRUE;
	}
	break;
	case WM_LBUTTONDOWN:
	{
		if (TraySave.bMonitorFloat)
		{
			bTaskBarMoveing = TRUE;
			PostMessage(hDlg, WM_NCLBUTTONDOWN, HTCAPTION, lParam);
			SetTimer(hDlg, 11, 1000, NULL);
		}
		return TRUE;
		/*
				else
					RunProcess(szNetCpl);
		*/
	}
	break;
	case WM_LBUTTONUP:
		if (!TraySave.bMonitorFloat)
		{
/*
			ShowWindow(hDlg, SW_HIDE);
			Sleep(100);
			POINT pt;
			GetCursorPos(&pt);
			mouse_event(MOUSEEVENTF_LEFTDOWN, pt.x, pt.y, 0, 0);
			mouse_event(MOUSEEVENTF_LEFTUP, pt.x, pt.y, 0, 0);
			SetTimer(hDlg, 9, 3000, NULL);
			ShowWindow(hTaskTips,SW_HIDE);
			return TRUE;
*/
		}
		break;
	case WM_TIMER:
		if (wParam == 11)
		{
			if (!KEYDOWN(VK_LBUTTON))
			{
				if (TraySave.bMonitorFloat && bTaskBarMoveing)
				{
					RECT wrc;
					GetWindowRect(hDlg, &wrc);
					TraySave.dMonitorPoint.x = wrc.left;
					TraySave.dMonitorPoint.y = wrc.top;
					WriteReg();
					bTaskBarMoveing = FALSE;
					KillTimer(hDlg, wParam);
				}
			}
		}
		else if (wParam == 9)
		{
			KillTimer(hDlg, wParam);
			ShowWindow(hDlg, SW_SHOWNOACTIVATE);
		}
		else if (wParam == 5)////////////////////////////////////////////////光标移出弹出式菜单自动隐藏菜单
		{

			HWND hMenu = FindWindow(L"#32768", NULL);
			POINT pt;
			GetCursorPos(&pt);
			if (WindowFromPoint(pt) != hMenu)
			{
				KillTimer(hDlg, wParam);
				PostMessage(hMenu, WM_CLOSE, NULL, NULL);
			}
		}
		else if (wParam == 3)
		{
			if (TraySave.bSecond)
			{
				if (IsWindow(hTime))
					::InvalidateRect(hTime, NULL, TRUE);
			}
			if (IsWindow(hTaskBar))
				::InvalidateRect(hTaskBar, NULL, TRUE);
			if (TraySave.bSound)
			{
				if (TraySave.bMonitorTraffic)
					if (TraySave.dNumValues[8] != 0 && (monitorData.s_in_byte > TraySave.dNumValues[8] || monitorData.s_out_byte > TraySave.dNumValues[8]))
						MessageBeep(MB_ICONHAND);
				if (TraySave.bMonitorTemperature)
					if (TraySave.dNumValues[9] != 0 && ((DWORD)monitorData.iTemperature1 > TraySave.dNumValues[9] || (DWORD)monitorData.iTemperature2 > TraySave.dNumValues[9]))
						MessageBeep(MB_ICONHAND);
				if (TraySave.bMonitorUsage)
					if ((TraySave.dNumValues[10] != 0 && cpuUsage > TraySave.dNumValues[10]) || (TraySave.dNumValues[11] != 0 && memoryStatus.dwMemoryLoad > TraySave.dNumValues[11]))
						MessageBeep(MB_ICONHAND);
				if (TraySave.bMonitorDisk)
					if (TraySave.dNumValues2[2] != 0 && (monitorData.diskreadbyte / 1024 / 1024 > TraySave.dNumValues2[2] || monitorData.diskwritebyte / 1024 / 1024 > TraySave.dNumValues2[2]))
						MessageBeep(MB_ICONHAND);
			}
			POINT pt;
			GetCursorPos(&pt);
			RECT brc;
			GetWindowRect(hTaskBar, &brc);
			if (PtInRect(&brc, pt))
			{
				if (rovi.dwBuildNumber >= 22000 && !TraySave.bMonitorFloat)//&&TraySave.cMonitorColor[0]==RGB(0,0,1))
				{
					if (!bFullScreen)
					{

						if (!KEYDOWN(VK_LBUTTON) && !IsWindow(hTaskTips))
						{
							SendMessage(hTaskBar, WM_MOUSEHOVER, 0, 0);
						}
					}
				}
				break;
			}
			if (TraySave.bMonitorTips)
			{
				if (IsWindow(hTaskTips))
				{
					GetWindowRect(hTaskTips, &brc);
					if (!PtInRect(&brc, pt))
					{
						DestroyWindow(hTaskTips);
					}
				}
			}
		}
	case WM_ERASEBKGND:
	{
		TRAYDATA* displayData = &monitorData;
		TRAYSAVE* displayConfig = &displaySettings;
		//		PAINTSTRUCT ps;
		HDC hdc = (HDC)wParam;//BeginPaint(hDlg, &ps);		
		HDC mdc = CreateCompatibleDC(hdc);
		if (mdc)
		{
			RECT rc;
			GetClientRect(hDlg, &rc);
			HBITMAP hMemBmp = CreateCompatibleBitmap(hdc, rc.right - rc.left, rc.bottom - rc.top);
			HBITMAP oldBmp = (HBITMAP)SelectObject(mdc, hMemBmp);
			//		if (TraySave.cMonitorColor[0] != 0)
			{
				HBRUSH hb;
				if (TraySave.cMonitorColor[0] != RGB(0, 0, 1)&& TraySave.cMonitorColor[0] != RGB(0, 0, 2))
					hb = CreateSolidBrush(TraySave.cMonitorColor[0]);
				else
				{

					/*
									if(bThemeMode&&!TraySave.bMonitorFloat&&rovi.dwBuildNumber>22000)
										hb=CreateSolidBrush(RGB(222,222,223));
									else
					*/
					if ((rovi.dwBuildNumber > 25000||!TraySave.bTrayStyle)&&!TraySave.bMonitorFloat)
					{
						COLORREF cPixel = GetWindowPixel(hTray);
						if (cPixel != oPixelColor)
						{
							oPixelColor = cPixel;
							SetLayeredWindowAttributes(hTaskBar, cPixel, 0, LWA_COLORKEY);
						}
						hb = CreateSolidBrush(oPixelColor);
					}
					else
						hb = CreateSolidBrush(RGB(0, 0, 1));
				}
				FillRect(mdc, &rc, hb);
				DeleteObject(hb);
			}
			//		if (bErasebkgnd)
			{
//				InflateRect(&rc, -1, -1);
				if(VTray)
					InflateRect(&rc, -wSpace/2,0);
				HFONT oldFont = (HFONT)SelectObject(mdc, hFont);
				WCHAR sz[16];
				SetBkMode(mdc, TRANSPARENT);
				COLORREF rgb;
				if (TraySave.bMonitorTraffic)
				{
					RECT crc = rc;
					if (VTray)
					{
						crc.bottom = wHeight;
						if (TraySave.bMonitorTrafficUpDown)
							DrawTraffic(mdc, &crc, displayData->s_out_byte, FALSE, *displayConfig);
						else
							DrawTraffic(mdc, &crc, displayData->s_in_byte, TRUE, *displayConfig);
						OffsetRect(&crc, 0, wHeight);
						if (TraySave.bMonitorTrafficUpDown)
							DrawTraffic(mdc, &crc, displayData->s_in_byte, TRUE, *displayConfig);
						else
							DrawTraffic(mdc, &crc, displayData->s_out_byte, FALSE, *displayConfig);
					}
					else
					{
						crc.right = crc.left + wTraffic;
						crc.bottom /= 2;
						InflateRect(&crc, -wSpace / 2, 0);
						if (TraySave.bMonitorTrafficUpDown)
							DrawTraffic(mdc, &crc, displayData->s_out_byte, FALSE, *displayConfig);
						else
							DrawTraffic(mdc, &crc, displayData->s_in_byte, TRUE, *displayConfig);
						OffsetRect(&crc, 0, crc.bottom);
						if (TraySave.bMonitorTrafficUpDown)
							DrawTraffic(mdc, &crc, displayData->s_in_byte, TRUE, *displayConfig);
						else
							DrawTraffic(mdc, &crc, displayData->s_out_byte, FALSE, *displayConfig);
					}
				}
				if (TraySave.bMonitorUsage)
				{
					if (cpuUsage <= TraySave.dNumValues[4])
						rgb = TraySave.cMonitorColor[4];
					else if (cpuUsage <= TraySave.dNumValues[5])
						rgb = TraySave.cMonitorColor[5];
					else
						rgb = TraySave.cMonitorColor[6];
					SetTextColor(mdc, rgb);
					/*
								if(bRing0)
									swprintf_s(sz, 16, L"%.2d%%", iCPU);
								else
					*/
					if (TraySave.iMonitorSimple == 1)
						wsprintf(sz, L"%.2d%%", cpuUsage);
					else if (TraySave.iMonitorSimple == 2)
						wsprintf(sz, L"%.2d", cpuUsage);
					else
						wsprintf(sz, L"%.2d%s",  cpuUsage, TraySave.szUsageCPUUnit);

					int sLen = lstrlen(sz);
					RECT crc = rc;
					if (VTray)
					{
						if (TraySave.bMonitorTraffic)
							crc.top = wHeight * 2;
						crc.bottom = crc.top + wHeight;
						
					}
					else
					{
						crc.left = crc.left + wTraffic;
						crc.right = crc.left + wUsage;
						crc.bottom /= 2;
						InflateRect(&crc, -(wSpace / 2), 0);
					}
					if (TraySave.iMonitorSimple == 0)
						DrawShadowText(mdc, TraySave.szUsageCPU, lstrlen(TraySave.szUsageCPU), &crc, DT_LEFT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
					DrawShadowText(mdc, sz, sLen, &crc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
					/*
								if(bRing0)
									swprintf_s(sz, 16, L"%.2d%%", MemoryStatusEx.dwMemoryLoad);
								else
					*/
					if (TraySave.iMonitorSimple == 1)
						wsprintf(sz, L"%.2d%%", memoryStatus.dwMemoryLoad);
					else if (TraySave.iMonitorSimple == 2)
						wsprintf(sz, L"%.2d", memoryStatus.dwMemoryLoad);
					else
						wsprintf(sz, L"%.2d%s", memoryStatus.dwMemoryLoad, TraySave.szUsageMEMUnit);
					sLen = lstrlen(sz);
					if (memoryStatus.dwMemoryLoad <= TraySave.dNumValues[6])
						rgb = TraySave.cMonitorColor[4];
					else if (memoryStatus.dwMemoryLoad <= TraySave.dNumValues[7])
						rgb = TraySave.cMonitorColor[5];
					else
						rgb = TraySave.cMonitorColor[6];
					SetTextColor(mdc, rgb);
					if (VTray)
						OffsetRect(&crc, 0, wHeight);						
					else
						OffsetRect(&crc, 0, crc.bottom);
					if (TraySave.iMonitorSimple == 0)
						DrawShadowText(mdc, TraySave.szUsageMEM, lstrlen(TraySave.szUsageMEM), &crc, DT_LEFT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
					DrawShadowText(mdc, sz, (int)sLen, &crc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
				}
				if (TraySave.bMonitorTemperature)
				{
					DWORD temperature1 = displayData->iTemperature1;
					DWORD temperature2 = displayData->iTemperature2;
					if ((hATIDLL != NULL || hNVDLL != NULL) && temperature1 == 0 && TraySave.bMonitorDisk && !hOHMA)
						temperature1 = displayData->disktime;
					if (hATIDLL == NULL && hNVDLL == NULL && TraySave.bMonitorDisk && !hOHMA && displayData->disktime != 0)
						temperature2 = displayData->disktime;
					RECT crc = rc;
					if (VTray)
					{
						if (TraySave.bMonitorTraffic)
							crc.top = wHeight * 2;
						if (TraySave.bMonitorUsage)
							crc.top += wHeight * 2;
						crc.bottom = crc.top + wHeight;
					}
					else
					{
						crc.left += wTraffic + wUsage;
						crc.right = crc.left + wTemperature;
						crc.bottom /= 2;
						InflateRect(&crc, -(wSpace / 2), 0);
					}
					if (bRing0)
					{
						if (temperature1 <= TraySave.dNumValues[2])
							rgb = TraySave.cMonitorColor[4];
						else if (temperature1 <= TraySave.dNumValues[3])
							rgb = TraySave.cMonitorColor[5];
						else
							rgb = TraySave.cMonitorColor[6];
						SetTextColor(mdc, rgb);
						if ((hATIDLL != NULL || hNVDLL != NULL )&& temperature1 == displayData->disktime && TraySave.bMonitorDisk&&!hOHMA)
						{
							if (TraySave.iMonitorSimple == 0)
							{
								DrawShadowText(mdc, TraySave.szDiskName, lstrlen(TraySave.szDiskName), &crc, DT_LEFT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
								wsprintf(sz, L"%.2d%s", temperature1, TraySave.szUsageMEMUnit);
							}
							else if (TraySave.iMonitorSimple == 1)
								wsprintf(sz, L"%.2d%%", temperature1);
							else
								wsprintf(sz, L"%.2d", temperature1);
						}
						else
						{
							if (TraySave.iMonitorSimple == 1)
								wsprintf(sz, L"%.2d℃", temperature1);
							else if (TraySave.iMonitorSimple == 2)
								wsprintf(sz, L"%.2d", temperature1);
							else
							{
								DrawShadowText(mdc, TraySave.szTemperatureCPU, lstrlen(TraySave.szTemperatureCPU), &crc, DT_LEFT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
								wsprintf(sz, L"%.2d%s", temperature1, TraySave.szTemperatureCPUUnit);
							}
						}
						DrawShadowText(mdc, sz, lstrlen(sz), &crc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
					}
					if (bRing0)
					{
						if (VTray)
							OffsetRect(&crc, 0, wHeight);
						else
							OffsetRect(&crc, 0, crc.bottom);
					}
					else
					{
						if (VTray)
						{
							//						crc.bottom += wHeight;
						}
						else
							crc.bottom += (crc.bottom - crc.top);
					}
					if (temperature2 <= TraySave.dNumValues[2])
						rgb = TraySave.cMonitorColor[4];
					else if (temperature2 <= TraySave.dNumValues[3])
						rgb = TraySave.cMonitorColor[5];
					else
						rgb = TraySave.cMonitorColor[6];
					SetTextColor(mdc, rgb);
					if (hATIDLL == NULL && hNVDLL == NULL && TraySave.bMonitorDisk&&!hOHMA)
					{
						if (TraySave.iMonitorSimple == 0)
						{
							DrawShadowText(mdc, TraySave.szDiskName, lstrlen(TraySave.szDiskName), &crc, DT_LEFT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
							wsprintf(sz, L"%.2d%s" , temperature2, TraySave.szUsageMEMUnit);
						}
						else if (TraySave.iMonitorSimple == 1)
							wsprintf(sz, L"%.2d%%", temperature2);
						else
							wsprintf(sz, L"%.2d", temperature2);
					}
					else
					{
						if (TraySave.iMonitorSimple == 0)
						{
							DrawShadowText(mdc, TraySave.szTemperatureGPU, lstrlen(TraySave.szTemperatureGPU), &crc, DT_LEFT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
							wsprintf(sz, L"%.2d%s", temperature2, TraySave.szTemperatureGPUUnit);
						}
						else if (TraySave.iMonitorSimple == 1)
							wsprintf(sz, L"%.2d℃", temperature2);
						else
							wsprintf(sz, L"%.2d", temperature2);
					}
					DrawShadowText(mdc, sz, lstrlen(sz), &crc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);

				}
				if (TraySave.bMonitorDisk)
				{
					RECT crc = rc;
					if (VTray)
					{
						if (TraySave.bMonitorTraffic)
							crc.top = wHeight * 2;
						if (TraySave.bMonitorTemperature)
						{
							crc.top += wHeight;
							if (bRing0)
								crc.top += wHeight;
						}
						if (TraySave.bMonitorUsage)
							crc.top += wHeight * 2;
						crc.bottom = crc.top + wHeight;
						if (hOHMA && TraySave.bMonitorTemperature)
						{
							if (displayData->iHddTemperature <= TraySave.dNumValues[2])
								rgb = TraySave.cMonitorColor[4];
							else if (displayData->iHddTemperature <= TraySave.dNumValues[3])
								rgb = TraySave.cMonitorColor[5];
							else
								rgb = TraySave.cMonitorColor[6];
							SetTextColor(mdc, rgb);
							if (TraySave.iMonitorSimple == 0)
							{
								DrawShadowText(mdc, TraySave.szDiskName, lstrlen(TraySave.szDiskName), &crc, DT_LEFT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
								wsprintf(sz, L"%.2d%s", displayData->iHddTemperature, TraySave.szTemperatureCPUUnit);
							}
							else if (TraySave.iMonitorSimple == 1)
								wsprintf(sz, L"%.2d℃", displayData->iHddTemperature);
							else
								wsprintf(sz, L"%.2d", displayData->iHddTemperature);
							DrawShadowText(mdc, sz, lstrlen(sz), &crc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
							OffsetRect(&crc, 0, wHeight);
							if (displayData->disktime <= TraySave.dNumValues[6])
								rgb = TraySave.cMonitorColor[4];
							else if (displayData->disktime <= TraySave.dNumValues[7])
								rgb = TraySave.cMonitorColor[5];
							else
								rgb = TraySave.cMonitorColor[6];
							SetTextColor(mdc, rgb);
							if (TraySave.iMonitorSimple == 0)
							{
								DrawShadowText(mdc, TraySave.szDiskName, lstrlen(TraySave.szDiskName), &crc, DT_LEFT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
								wsprintf(sz, L"%.2d%s", displayData->disktime, TraySave.szUsageMEMUnit);
							}
							else if (TraySave.iMonitorSimple == 1)
								wsprintf(sz, L"%.2d%%", displayData->disktime);
							else
								wsprintf(sz, L"%.2d", displayData->disktime);
							DrawShadowText(mdc, sz, lstrlen(sz), &crc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
							OffsetRect(&crc, 0, wHeight);
						}
						if (TraySave.bMonitorTrafficUpDown)
							DrawDisk(mdc, &crc, displayData->diskreadbyte, FALSE, *displayConfig);
						else
							DrawDisk(mdc, &crc, displayData->diskwritebyte, TRUE, *displayConfig);
						OffsetRect(&crc, 0, wHeight);
						if (TraySave.bMonitorTrafficUpDown)
							DrawDisk(mdc, &crc, displayData->diskwritebyte, TRUE, *displayConfig);
						else
							DrawDisk(mdc, &crc, displayData->diskreadbyte, FALSE, *displayConfig);
					}
					else
					{
						crc.left = crc.left + wTraffic + wTemperature + wUsage;
						crc.right = crc.left + wDisk;
						crc.bottom /= 2;
						if (hOHMA&&TraySave.bMonitorTemperature)
						{
							crc.right -= (wDisk-wTemperature);
							InflateRect(&crc, -(wSpace / 2), 0);
							if (displayData->iHddTemperature <= TraySave.dNumValues[2])
								rgb = TraySave.cMonitorColor[4];
							else if (displayData->iHddTemperature <= TraySave.dNumValues[3])
								rgb = TraySave.cMonitorColor[5];
							else
								rgb = TraySave.cMonitorColor[6];
							SetTextColor(mdc, rgb);
							if (TraySave.iMonitorSimple == 0)
							{
								DrawShadowText(mdc, TraySave.szDiskName, lstrlen(TraySave.szDiskName), &crc, DT_LEFT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
								wsprintf(sz, L"%.2d%s", displayData->iHddTemperature, TraySave.szTemperatureCPUUnit);
							}
							else if (TraySave.iMonitorSimple == 1)
								wsprintf(sz, L"%.2d℃", displayData->iHddTemperature);
							else
								wsprintf(sz, L"%.2d", displayData->iHddTemperature);
							DrawShadowText(mdc, sz, lstrlen(sz), &crc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
							OffsetRect(&crc, 0, crc.bottom);
							if (displayData->disktime <= TraySave.dNumValues[6])
								rgb = TraySave.cMonitorColor[4];
							else if (displayData->disktime <= TraySave.dNumValues[7])
								rgb = TraySave.cMonitorColor[5];
							else
								rgb = TraySave.cMonitorColor[6];
							SetTextColor(mdc, rgb);
							if (TraySave.iMonitorSimple == 0)
							{
								DrawShadowText(mdc, TraySave.szDiskName, lstrlen(TraySave.szDiskName), &crc, DT_LEFT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
								wsprintf(sz, L"%.2d%s", displayData->disktime, TraySave.szUsageMEMUnit);
							}
							else if (TraySave.iMonitorSimple == 1)
								wsprintf(sz, L"%.2d%%", displayData->disktime);
							else
								wsprintf(sz, L"%.2d", displayData->disktime);
							DrawShadowText(mdc, sz, lstrlen(sz), &crc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
							OffsetRect(&crc, wTemperature, -(crc.bottom - crc.top));
							crc.right = crc.left + (wDisk - wTemperature)-wSpace;
						}
						else
							InflateRect(&crc, -(wSpace / 2), 0);
						if (TraySave.bMonitorTrafficUpDown)
							DrawDisk(mdc, &crc, displayData->diskreadbyte, FALSE, *displayConfig);
						else
							DrawDisk(mdc, &crc, displayData->diskwritebyte, TRUE, *displayConfig);
						OffsetRect(&crc, 0, crc.bottom);
						if (TraySave.bMonitorTrafficUpDown)
							DrawDisk(mdc, &crc, displayData->diskwritebyte, TRUE, *displayConfig);
						else
							DrawDisk(mdc, &crc, displayData->diskreadbyte, FALSE, *displayConfig);
					}
				}
				if (TraySave.bMonitorTime)
				{
					SetTextColor(mdc, TraySave.cMonitorColor[1]);
					SYSTEMTIME systm;
					GetLocalTime(&systm);
					RECT crc = rc;
					TCHAR szWeek[7][2] = { L"日",L"一",L"二",L"三",L"四",L"五",L"六" };
					wsprintf(sz, L"%.2d/%.2d'%s", systm.wMonth, systm.wDay, szWeek[systm.wDayOfWeek]);
					int sLen = lstrlen(sz);
					if (VTray)
					{
						if (TraySave.bMonitorTraffic)
							crc.top = wHeight * 2;
						if (TraySave.bMonitorTemperature)
						{
							crc.top += wHeight;
							if (bRing0)
								crc.top += wHeight;
						}
						if (TraySave.bMonitorUsage)
							crc.top += wHeight * 2;
						if (TraySave.bMonitorDisk)
						{
							if(hOHMA && TraySave.bMonitorTemperature)
								crc.top += wHeight * 2;
							crc.top += wHeight * 2;
						}
						crc.bottom = crc.top + wHeight;
					}
					else
					{
						crc.left = crc.left + wTraffic + wTemperature + wUsage + wDisk;
						crc.right = crc.left + wTime;
						crc.bottom /= 2;
						InflateRect(&crc, -(wSpace / 2), 0);
					}
					if (VTray)
					{
						if(TraySave.iMonitorSimple == 0)
							DrawShadowText(mdc, L"D", 1, &crc, DT_LEFT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
						DrawShadowText(mdc, sz, sLen, &crc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
					}
					else
						DrawShadowText(mdc, sz, sLen, &crc, DT_CENTER | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
					wsprintf(sz, L"%.2d:%.2d:%.2d", systm.wHour, systm.wMinute, systm.wSecond);
					sLen = lstrlen(sz);
					if (VTray)
						OffsetRect(&crc, 0, wHeight);
					else
						OffsetRect(&crc, 0, crc.bottom);
					if (VTray)
					{
						if(TraySave.iMonitorSimple == 0)
							DrawShadowText(mdc, L"T", 1, &crc, DT_LEFT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
						DrawShadowText(mdc, sz, (int)sLen, &crc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
					}
				else
					DrawShadowText(mdc, sz, sLen, &crc, DT_CENTER | DT_VCENTER | DT_SINGLELINE, bColor, bShadow);
				}
				SelectObject(mdc, oldFont);
			}
			//		GetClientRect(hDlg, &rc);
			if (VTray)
				InflateRect(&rc, wSpace / 2, 0);
			if(TraySave.bMonitorFuse)/////////////////背景融合
			{
				BYTE* lpvBits = NULL;

				BITMAPINFO binfo;
				memset(&binfo, 0, sizeof(BITMAPINFO));
				binfo.bmiHeader.biBitCount = 32;     //每个像素多少位，也可直接写24(RGB)或者32(RGBA)
				binfo.bmiHeader.biCompression = 0;
				binfo.bmiHeader.biHeight = rc.bottom - rc.top;
				binfo.bmiHeader.biPlanes = 1;
				binfo.bmiHeader.biSizeImage = (rc.bottom - rc.top) * (rc.right - rc.left) * 4;
				binfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
				binfo.bmiHeader.biWidth = rc.right - rc.left;
				lpvBits = (BYTE*)HeapAlloc(GetProcessHeap(), NULL, binfo.bmiHeader.biSizeImage);
				//		GetDIBits(mdc, hMemBmp, 0, rc.bottom - rc.top, lpvBits, &bmpInfo, DIB_RGB_COLORS);
				if (lpvBits)
				{
					GetDIBits(mdc, hMemBmp, 0, rc.bottom - rc.top, lpvBits, &binfo, DIB_RGB_COLORS);
					for (DWORD i = 0; i < binfo.bmiHeader.biSizeImage - 4; i += 4)
					{
						if (lpvBits[i] > 3 || lpvBits[i + 1] > 3 || lpvBits[i + 2] > 3)
						{
							if (TraySave.bMonitorFuse)
								lpvBits[i + 3] = 0x80;
							else if (TraySave.bMonitorFloat)
								lpvBits[i + 3] = 0xff;
						}
					}
					SetDIBits(mdc, hMemBmp, 0, rc.bottom - rc.top, lpvBits, &binfo, DIB_RGB_COLORS);
					HeapFree(GetProcessHeap(), 0, lpvBits);
				}
			}
			BitBlt(hdc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, mdc, 0, 0, SRCCOPY);
			SelectObject(mdc, oldBmp);
			DeleteObject(hMemBmp);
			DeleteDC(mdc);
			//		EndPaint(hDlg, &ps);
		}
		return TRUE;
	}
	break;
	}
	return FALSE;
}
INT_PTR CALLBACK MainProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)//主窗口过程
{
	UNREFERENCED_PARAMETER(lParam);
	if (uTaskbarCreated && message == uTaskbarCreated)
	{
		if (RestoreShellIntegration())
			KillTimer(hMain, 3000);
		else
			SetTimer(hMain, 3000, 3000, NULL);
		return TRUE;
	}
	if (message == WM_TRAYS_REFRESH_UI)
	{
		if (wParam != 0)
		{
			bThemeMode = (DWORD)lParam;
			CloseTaskBar();
		}
		else
		{
			if (IsWindow(hTaskBar))
				InvalidateRect(hTaskBar, NULL, TRUE);
			if (IsWindowVisible(hTaskTips))
				InvalidateRect(hTaskTips, NULL, TRUE);
		}
		return TRUE;
	}
	switch (message)
	{
	case MSG_APPBAR_MSGID:
		if (wParam == ABN_FULLSCREENAPP)
		{
			if (TraySave.bMonitorTopmost && !TraySave.bMonitorFloat)
			{
				if (lParam == FALSE)
				{
					if (bFullScreen)
					{
						DestroyWindow(hTaskBar);
					}
				}
				else
				{
					if (!bFullScreen)
					{
						DestroyWindow(hTaskBar);
					}
				}
			}
			bFullScreen = (BOOL)lParam;
		}
		break;
	case WM_INITDIALOG:
		SetTimer(hDlg, 88, 8888,NULL);
		return (INT_PTR)TRUE;
		/*
			case WM_ENDSESSION:
				if (lParam == ENDSESSION_LOGOFF)
				{
					DestroyWindow(hTray);
					RunProcess(NULL);
					return TRUE;
				}
				break;
		*/
	case WM_TRAYS:
		if(bSetting)
			OpenSetting();
		break;
	case 0x02e0://WM_DPICHANGED:
	{
		iDPI = LOWORD(wParam);
		DestroyWindow(hTime);
		DestroyWindow(hTaskBar);
		SetWH();
/*
		bResetRun = TRUE;
		PostQuitMessage(0);
*/
	}
	break;
	case WM_CLOSE:
	{
		KillTimer(hDlg, 6);
		KillTimer(hDlg, 3);
		KillTimer(hDlg, 3000);
		if (IsWindow(hReBarWnd))
			SendMessage(hReBarWnd, WM_SETREDRAW, TRUE, 0);
		HWND hSecondaryTray;
		hSecondaryTray = FindWindow(szSecondaryTray, NULL);
		while (hSecondaryTray)
		{
			HWND hSReBarWnd = FindWindowEx(hSecondaryTray, 0, L"WorkerW", NULL);
			SendMessage(hSReBarWnd, WM_SETREDRAW, TRUE, 0);
			ShowWindow(hSReBarWnd, SW_SHOWNOACTIVATE);
			hSecondaryTray = FindWindowEx(NULL, hSecondaryTray, szSecondaryTray, NULL);
		}
		if (IsWindow(hTaskListWnd))
			ShowWindow(hTaskListWnd, SW_SHOW);
		PostQuitMessage(0);
	}
	break;
	case WM_TIMER:
	{
		if(wParam==88)
		{
			KillTimer(hDlg,wParam);
			bSetting = TRUE;
		}
		else if(wParam==3000)
		{
			if (RestoreShellIntegration())
				KillTimer(hDlg, 3000);
			else
				SetTimer(hDlg, 3000, 3000, NULL);
		}
		else if (wParam == 11)//释放内存
		{
			KillTimer(hDlg, wParam);
			SetTimer(hDlg, wParam, 60000, NULL);
			EmptyProcessMemory();
		}
		else if (wParam == 6)//处理任务栏图标与信息窗口
		{
			if (!IsWindow(hTray))
			{
				SetTimer(hDlg, 3000, 3000, NULL);
				break;
			}
			if (TraySave.bTrayStyle)
			{
				if ((TraySave.iPos != 0 || TraySave.bMonitor) && hWin11UI == NULL && rovi.dwBuildNumber < 22000)
				{
					//				if (TraySave.bTaskIcon == FALSE)
					{
						SetTaskBarPos(hTaskListWnd, hTray, hTaskWnd, hReBarWnd, TRUE);
					}
					HWND hSecondaryTray;
					hSecondaryTray = FindWindow(szSecondaryTray, NULL);
					while (hSecondaryTray)
					{
						HWND hSReBarWnd = FindWindowEx(hSecondaryTray, 0, L"WorkerW", NULL);
						if (hSReBarWnd)
						{
							HWND hSTaskListWnd = FindWindowEx(hSReBarWnd, NULL, L"MSTaskListWClass", NULL);
							if (hSTaskListWnd)
							{
								SetTaskBarPos(hSTaskListWnd, hSecondaryTray, hSReBarWnd, hSReBarWnd, FALSE);
							}
						}
						hSecondaryTray = FindWindowEx(NULL, hSecondaryTray, szSecondaryTray, NULL);
					}
				}
			}
		}
		else if (wParam == 3)//处理任务栏风格
		{
			UINT nextRefresh = TraySave.bMonitor ? TraySave.FlushTime : 250;
			if (TraySave.bMonitor)
			{
				if (!bTaskBarMoveing)
				{
					AdjustWindowPos();
				}
			}
			//			if (TraySave.aMode[0] != ACCENT_DISABLED || TraySave.aMode[1] != ACCENT_DISABLED)
			if(TraySave.bTrayStyle)
			{
				int oldWindowMode = iWindowMode;
		if (hTray)
		{
					if (iProject == 0)
						iWindowMode = 0;
					else if (iProject == 1)
						iWindowMode = 1;
					else
					{
						iWindowMode = 0;
						EnumWindows(IsZoomedFunc, (LPARAM)MonitorFromWindow(hTray, MONITOR_DEFAULTTONEAREST));
					}
					if (TraySave.aMode[iWindowMode] != ACCENT_DISABLED || oldWindowMode != iWindowMode)
					{
						SetWindowCompositionAttribute(hTray, TraySave.aMode[iWindowMode], TraySave.dAlphaColor[iWindowMode], hWin11UI != NULL);
//						HWND hTray11=FindWindowEx(hTray, 0, L"Windows.UI.Composition.DesktopWindowContentBridge",NULL);
//						SetWindowCompositionAttribute(hTray11, TraySave.aMode[iWindowMode], TraySave.dAlphaColor[iWindowMode]);
					}
					LONG_PTR exStyle = GetWindowLongPtr(hTray, GWL_EXSTYLE);
					exStyle |= WS_EX_LAYERED;
					SetWindowLongPtr(hTray, GWL_EXSTYLE, exStyle);
					SetLayeredWindowAttributes(hTray, 0, (BYTE)TraySave.bAlpha[iWindowMode], LWA_ALPHA);
				}
				HWND hSecondaryTray = FindWindow(szSecondaryTray, NULL);
				while (hSecondaryTray)
				{
					if (iProject == 0)
						iWindowMode = 0;
					else if (iProject == 1)
						iWindowMode = 1;
					else
					{
						iWindowMode = 0;
						EnumWindows(IsZoomedFunc, (LPARAM)MonitorFromWindow(hSecondaryTray, MONITOR_DEFAULTTONEAREST));
					}
					if (TraySave.aMode[iWindowMode] != ACCENT_DISABLED || oldWindowMode != iWindowMode)
						SetWindowCompositionAttribute(hSecondaryTray, TraySave.aMode[iWindowMode], TraySave.dAlphaColor[iWindowMode], hWin11UI != NULL);
					LONG_PTR exStyle = GetWindowLongPtr(hSecondaryTray, GWL_EXSTYLE);
					exStyle |= WS_EX_LAYERED;
					SetWindowLongPtr(hSecondaryTray, GWL_EXSTYLE, exStyle);
					SetLayeredWindowAttributes(hSecondaryTray, 0, (BYTE)TraySave.bAlpha[iWindowMode], LWA_ALPHA);
					hSecondaryTray = FindWindowEx(NULL, hSecondaryTray, szSecondaryTray, NULL);
				}
			}
			if (TraySave.bMonitor && TraySave.bTrayStyle)
				nextRefresh = TraySave.FlushTime < 250 ? TraySave.FlushTime : 250;
			SetTimer(hDlg, 3, nextRefresh, NULL);
			//			if (TraySave.aMode[0] == ACCENT_DISABLED && TraySave.aMode[1] == ACCENT_DISABLED)//默认则关闭定时器
			//				KillTimer(hDlg, 3);
		}
	}
	break;
	case WM_IAWENTRAY://////////////////////////////////////////////////////////////////////////////////通知栏左右键处理
	{
		if (LOWORD(lParam) == WM_LBUTTONDOWN || LOWORD(lParam) == WM_RBUTTONDOWN)
		{
			OpenSetting();
		}
		break;
	}
	break;
	}
	return FALSE;
}
INT_PTR CALLBACK SettingProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)//设置窗口过程
{
	UNREFERENCED_PARAMETER(lParam);
	switch (message)
	{
	case WM_INITDIALOG:
		return (INT_PTR)TRUE;
	case WM_NOTIFY:
		switch (((LPNMHDR)lParam)->code)
		{
		case NM_CLICK:
		case NM_RETURN:
		{
			HWND g_hLink = GetDlgItem(hDlg, IDC_SYSLINK);
			PNMLINK pNMLink = (PNMLINK)lParam;
			LITEM item = pNMLink->item;
			if ((((LPNMHDR)lParam)->hwndFrom == g_hLink) && (item.iLink == 0))
			{
				CloseHandle(pShellExecute(NULL, L"open", L"https://github.com/cgbsmy/TrayS", NULL, NULL, SW_SHOW));
				//CloseHandle(pShellExecute(NULL, L"open", L"https://gitee.com/cgbsmy/TrayS", NULL, NULL, SW_SHOW));
				//mailto:cgbsmy@live.com?subject=TrayS
			}
			else
			{
				CloseHandle(pShellExecute(NULL, L"open", L"https://www.52pojie.cn/thread-1182669-1-1.html", NULL, NULL, SW_SHOW));
			}
			break;
		}
		}
		break;
	case WM_HSCROLL://////////////////////////////////////////////////////////////////////////////////透明度处理
	{
		HWND hSlider = GetDlgItem(hDlg, IDC_SLIDER_ALPHA);
		HWND hSliderB = GetDlgItem(hDlg, IDC_SLIDER_ALPHA_B);
		if (hSlider == (HWND)lParam)
		{
			TraySave.bAlpha[iProject] = (int)SendDlgItemMessage(hDlg, IDC_SLIDER_ALPHA, TBM_GETPOS, 0, 0);
		}
		else if (hSliderB == (HWND)lParam)
		{
			DWORD bAlphaB = (int)SendDlgItemMessage(hDlg, IDC_SLIDER_ALPHA_B, TBM_GETPOS, 0, 0);
			bAlphaB = bAlphaB << 24;
			TraySave.dAlphaColor[iProject] = bAlphaB + (TraySave.dAlphaColor[iProject] & 0xffffff);
		}
		SetTimer(hDlg, 3, 500, NULL);
		break;
	}
	case WM_TIMER:
		if (wParam == 3)
		{
			KillTimer(hDlg, wParam);
			WriteReg();
		}
		break;
	case WM_COMMAND:
		if (HIWORD(wParam) == EN_CHANGE && !bSettingInit)
		{
			if (LOWORD(wParam) >= IDC_EDIT1 && LOWORD(wParam) <= IDC_EDIT12)
			{
				int index = LOWORD(wParam) - IDC_EDIT1;
				TraySave.dNumValues[index] = GetDlgItemInt(hDlg, LOWORD(wParam), NULL, 0);
				if (index == 0 || index == 1 || index == 8)
					TraySave.dNumValues[index] *= 1048576;
				SetTimer(hDlg, 3, 500, NULL);
			}
			else if (LOWORD(wParam) >= IDC_EDIT24 && LOWORD(wParam) <= IDC_EDIT26)
			{
				int index = LOWORD(wParam) - IDC_EDIT24;
				TraySave.dNumValues2[index] = GetDlgItemInt(hDlg, LOWORD(wParam), NULL, 0);
				SetTimer(hDlg, 3, 500, NULL);
			}
			else if (LOWORD(wParam) == IDC_EDIT_TIME)
			{
				TraySave.FlushTime = GetDlgItemInt(hDlg, LOWORD(wParam), NULL, 0);
				if (TraySave.FlushTime < 100)
					TraySave.FlushTime = 100;
				else if (TraySave.FlushTime > 5000)
					TraySave.FlushTime = 5000;
				UpdateMainRefreshTimer();
				SetTimer(hDlg, 3, 500, NULL);
			}
			else if ((LOWORD(wParam) >= IDC_EDIT14 && LOWORD(wParam) <= IDC_EDIT23)||LOWORD(wParam)==IDC_EDIT27|| LOWORD(wParam) == IDC_EDIT29)
			{
				GetDlgItemText(hDlg, IDC_EDIT14, TraySave.szTrafficOut, 8);
				GetDlgItemText(hDlg, IDC_EDIT15, TraySave.szTrafficIn, 8);
				GetDlgItemText(hDlg, IDC_EDIT16, TraySave.szTemperatureCPU, 8);
				GetDlgItemText(hDlg, IDC_EDIT17, TraySave.szTemperatureGPU, 8);
				GetDlgItemText(hDlg, IDC_EDIT18, TraySave.szTemperatureCPUUnit, 4);
				GetDlgItemText(hDlg, IDC_EDIT19, TraySave.szTemperatureGPUUnit, 4);
				GetDlgItemText(hDlg, IDC_EDIT20, TraySave.szUsageCPU, 8);
				GetDlgItemText(hDlg, IDC_EDIT21, TraySave.szUsageMEM, 8);
				GetDlgItemText(hDlg, IDC_EDIT22, TraySave.szUsageCPUUnit, 4);
				GetDlgItemText(hDlg, IDC_EDIT23, TraySave.szUsageMEMUnit, 4);
				GetDlgItemText(hDlg, IDC_EDIT27, TraySave.szDiskReadSec, 8);
				GetDlgItemText(hDlg, IDC_EDIT28, TraySave.szDiskWriteSec, 8);
				GetDlgItemText(hDlg, IDC_EDIT29, TraySave.szDiskName, 8);
				SetTimer(hDlg, 3, 1500, NULL);
				if (TraySave.iMonitorSimple == 0)
				{
					SetWH();
					AdjustWindowPos();
				}
			}			
		}
		else if (LOWORD(wParam) >= IDC_RADIO_DEFAULT && LOWORD(wParam) <= IDC_RADIO_ACRYLIC)
		{
			iWindowMode = !iWindowMode;
			if (IsDlgButtonChecked(hDlg, IDC_RADIO_DEFAULT))
				TraySave.aMode[iProject] = ACCENT_DISABLED;
			else if (IsDlgButtonChecked(hDlg, IDC_RADIO_TRANSPARENT))
				TraySave.aMode[iProject] = ACCENT_ENABLE_TRANSPARENTGRADIENT;
			else if (IsDlgButtonChecked(hDlg, IDC_RADIO_BLURBEHIND))
				TraySave.aMode[iProject] = ACCENT_ENABLE_BLURBEHIND;
			else if (IsDlgButtonChecked(hDlg, IDC_RADIO_ACRYLIC))
				TraySave.aMode[iProject] = ACCENT_ENABLE_ACRYLICBLURBEHIND;
			WriteReg();
			UpdateMainRefreshTimer();

		}
		else if (LOWORD(wParam) >= IDC_RADIO_LEFT && LOWORD(wParam) <= IDC_RADIO_RIGHT)
		{
			if (IsDlgButtonChecked(hDlg, IDC_RADIO_LEFT))
			{
				TraySave.iPos = 0;
			}
			else if (IsDlgButtonChecked(hDlg, IDC_RADIO_CENTER))
			{
				TraySave.iPos = 1;
			}
			else if (IsDlgButtonChecked(hDlg, IDC_RADIO_RIGHT))
			{
				TraySave.iPos = 2;
			}
			WriteReg();
		}
		else if (LOWORD(wParam) >= IDC_RADIO_BYTE && LOWORD(wParam) <= IDC_RADIO_MB)
		{
			if (IsDlgButtonChecked(hDlg, IDC_RADIO_AUTO))
				TraySave.iUnit = 0;
			else if (IsDlgButtonChecked(hDlg, IDC_RADIO_KB))
				TraySave.iUnit = 1;
			else if (IsDlgButtonChecked(hDlg, IDC_RADIO_MB))
				TraySave.iUnit = 2;
			if (IsDlgButtonChecked(hDlg, IDC_RADIO_BIT))
				TraySave.iUnit |= 0x10000;
			WriteReg();
		}
		if (LOWORD(wParam) == IDC_RADIO_NORMAL || LOWORD(wParam) == IDC_RADIO_MAXIMIZE)
		{
			if (IsDlgButtonChecked(hDlg, IDC_RADIO_NORMAL))
				iProject = 0;
			else
				iProject = 1;
			if (TraySave.aMode[iProject] == ACCENT_DISABLED)
				CheckRadioButton(hSetting, IDC_RADIO_DEFAULT, IDC_RADIO_ACRYLIC, IDC_RADIO_DEFAULT);
			else if (TraySave.aMode[iProject] == ACCENT_ENABLE_TRANSPARENTGRADIENT)
				CheckRadioButton(hSetting, IDC_RADIO_DEFAULT, IDC_RADIO_ACRYLIC, IDC_RADIO_TRANSPARENT);
			else if (TraySave.aMode[iProject] == ACCENT_ENABLE_BLURBEHIND)
				CheckRadioButton(hSetting, IDC_RADIO_DEFAULT, IDC_RADIO_ACRYLIC, IDC_RADIO_BLURBEHIND);
			else if (TraySave.aMode[iProject] == ACCENT_ENABLE_ACRYLICBLURBEHIND)
				CheckRadioButton(hSetting, IDC_RADIO_DEFAULT, IDC_RADIO_ACRYLIC, IDC_RADIO_ACRYLIC);
			SendDlgItemMessage(hSetting, IDC_SLIDER_ALPHA, TBM_SETPOS, TRUE, TraySave.bAlpha[iProject]);
			BYTE bAlphaB = TraySave.dAlphaColor[iProject] >> 24;
			SendDlgItemMessage(hSetting, IDC_SLIDER_ALPHA_B, TBM_SETPOS, TRUE, bAlphaB);
			::InvalidateRect(GetDlgItem(hSetting, IDC_BUTTON_COLOR), NULL, FALSE);
		}
		else if (LOWORD(wParam) == IDC_CHECK_SOUND)
		{
			TraySave.bSound = IsDlgButtonChecked(hDlg, IDC_CHECK_SOUND);
			WriteReg();
		}
		else if (LOWORD(wParam) == IDC_CHECK_TIPS)
		{
			TraySave.bMonitorTips = IsDlgButtonChecked(hDlg, IDC_CHECK_TIPS);
			WriteReg();
		}
		else if (LOWORD(wParam) == IDC_CHECK_FUSE)
		{
			TraySave.bMonitorFuse = IsDlgButtonChecked(hDlg, IDC_CHECK_FUSE);
			WriteReg();
			CloseTaskBar();
		}
		else if (LOWORD(wParam) == IDC_CHECK_TRAYICON)
		{
			TraySave.bTrayIcon = IsDlgButtonChecked(hDlg, IDC_CHECK_TRAYICON);
			WriteReg();
			CloseTaskBar();
			if (TraySave.bTrayIcon)
				pShell_NotifyIcon(NIM_ADD, &nid);
			else
				pShell_NotifyIcon(NIM_DELETE, &nid);
		}
		else if (LOWORD(wParam) == IDC_CHECK_TRAY_STYLE)
		{
			TraySave.bTrayStyle = IsDlgButtonChecked(hDlg, IDC_CHECK_TRAY_STYLE);
			WriteReg();
			if (TraySave.bTrayStyle == FALSE)
			{
				SetWindowCompositionAttribute(hTray, ACCENT_DISABLED, 0, hWin11UI != NULL);
				SetLayeredWindowAttributes(hTray, 0, 255, LWA_ALPHA);
				InvalidateRect(hTray, NULL, TRUE);
			}
			CloseTaskBar();
			UpdateMainRefreshTimer();
		}
		else if (LOWORD(wParam) == IDC_CHECK_MONITOR)
		{
			TraySave.bMonitor = IsDlgButtonChecked(hDlg, IDC_CHECK_MONITOR);
			WriteReg();
			if (!TraySave.bMonitor)
			{
				CloseTaskBar();
			}
			UpdateMainRefreshTimer();
		}
		else if (LOWORD(wParam) == IDC_CHECK_TRAFFIC)
		{
			TraySave.bMonitorTraffic = IsDlgButtonChecked(hDlg, IDC_CHECK_TRAFFIC);
			WriteReg();
			SetWH();
			AdjustWindowPos();
		}
		else if (LOWORD(wParam) == IDC_CHECK_MONITOR_UPDOWN)
		{
			TraySave.bMonitorTrafficUpDown = IsDlgButtonChecked(hDlg, IDC_CHECK_MONITOR_UPDOWN);
			WriteReg();
			SetWH();
			AdjustWindowPos();
		}
		else if (LOWORD(wParam) == IDC_CHECK_TEMPERATURE)
		{
			TraySave.bMonitorTemperature = IsDlgButtonChecked(hDlg, IDC_CHECK_TEMPERATURE);
			if (TraySave.bMonitorTemperature)
				LoadTemperatureDLL();
			else
				FreeTemperatureDLL();
			WriteReg();
			SetWH();
			AdjustWindowPos();
		}
		else if (LOWORD(wParam) == IDC_CHECK_MONITOR_SIMPLE)
		{
			TraySave.iMonitorSimple = IsDlgButtonChecked(hDlg, IDC_CHECK_MONITOR_SIMPLE);
			WriteReg();
			SetWH();
			AdjustWindowPos();
		}
		else if (LOWORD(wParam) == IDC_CHECK_USAGE)
		{
			TraySave.bMonitorUsage = IsDlgButtonChecked(hDlg, IDC_CHECK_USAGE);
			WriteReg();
			SetWH();
			AdjustWindowPos();
		}
		else if (LOWORD(wParam) == IDC_CHECK_DISK)
		{
			TraySave.bMonitorDisk = IsDlgButtonChecked(hDlg, IDC_CHECK_DISK);
			WriteReg();
			SwitchPDH(FALSE);
			if (TraySave.bMonitorDisk || TraySave.bMonitorPDH)
				SwitchPDH(TRUE);
			SetWH();
			AdjustWindowPos();
		}
		else if (LOWORD(wParam) == IDC_CHECK_MONITOR_PDH)
		{
			TraySave.bMonitorPDH = IsDlgButtonChecked(hDlg, IDC_CHECK_MONITOR_PDH);
			WriteReg();
			SwitchPDH(FALSE);
			if (TraySave.bMonitorDisk || TraySave.bMonitorPDH)
				SwitchPDH(TRUE);
		}
		else if (LOWORD(wParam) == IDC_CHECK_MONITOR_LEFT)
		{
			TraySave.bMonitorLeft = IsDlgButtonChecked(hDlg, IDC_CHECK_MONITOR_LEFT);
			WriteReg();
			AdjustWindowPos();
		}
		else if (LOWORD(wParam) == IDC_CHECK_MONITOR_NEAR)
		{
			TraySave.bNear = IsDlgButtonChecked(hDlg, IDC_CHECK_MONITOR_NEAR);
			WriteReg();
			AdjustWindowPos();
		}
		else if (LOWORD(wParam) == IDC_CHECK_MONITOR_FLOAT)
		{
			TraySave.bMonitorFloat = IsDlgButtonChecked(hDlg, IDC_CHECK_MONITOR_FLOAT);
			WriteReg();
			CloseTaskBar();
		}
		else if (LOWORD(wParam) == IDC_CHECK_MONITOR_FLOAT_VROW)
		{
			TraySave.bMonitorFloatVRow = IsDlgButtonChecked(hDlg, IDC_CHECK_MONITOR_FLOAT_VROW);
			WriteReg();
			CloseTaskBar();
		}
		else if (LOWORD(wParam) == IDC_CHECK_MONITOR_TIME)
		{
			TraySave.bMonitorTime = IsDlgButtonChecked(hDlg, IDC_CHECK_MONITOR_TIME);
			WriteReg();
			SetWH();
			AdjustWindowPos();
		}
		else if (LOWORD(wParam) == IDC_CHECK_TIME)
		{
			TraySave.bSecond = IsDlgButtonChecked(hDlg, IDC_CHECK_TIME);
			WriteReg();
			if(TraySave.bSecond)
				OpenTimeDlg();
			else
				DestroyWindow(hTime);
		}
		else if (LOWORD(wParam) == IDC_CHECK_TRANSPARENT)
		{
			TraySave.bMonitorTransparent = IsDlgButtonChecked(hDlg, IDC_CHECK_TRANSPARENT);
			if (TraySave.bMonitorTransparent)
				SetWindowLongPtr(hTaskBar, GWL_EXSTYLE, GetWindowLongPtr(hTaskBar, GWL_EXSTYLE) | WS_EX_TRANSPARENT | WS_EX_LAYERED);
			else
				SetWindowLongPtr(hTaskBar, GWL_EXSTYLE, GetWindowLongPtr(hTaskBar, GWL_EXSTYLE) & ~WS_EX_TRANSPARENT);
			WriteReg();
		}
		else if (LOWORD(wParam) == IDC_CHECK_TOPMOST)
		{
			TraySave.bMonitorTopmost = IsDlgButtonChecked(hDlg, IDC_CHECK_TOPMOST);
			WriteReg();
		}
		else if (LOWORD(wParam) == IDC_CHECK_AUTORUN)
		{
			if (IsDlgButtonChecked(hDlg, IDC_CHECK_AUTORUN))
				AutoRun(TRUE, TRUE, szAppName);
			else
				AutoRun(TRUE, FALSE, szAppName);
		}
		else if (LOWORD(wParam) == IDC_RESTORE_DEFAULT)
		{
			DeleteFile(szTraySave);
			//			RegDeleteKey(HKEY_CURRENT_USER, szSubKey);
			SendMessage(hDlg, WM_COMMAND, IDCANCEL, 0);
		}
		else if (LOWORD(wParam) == IDCANCEL)
		{
			/*
						SendMessage(hMain, WM_TIMER, 11, 1000);
						DestroyWindow(hDlg);
						return (INT_PTR)TRUE;
			*/
			//			SendMessage(hReBarWnd, WM_SETREDRAW, TRUE, 0);
			SendMessage(hMain, WM_CLOSE, NULL, NULL);
			return (INT_PTR)TRUE;
		}
		else if (LOWORD(wParam) == IDC_CLOSE)
		{
			SendMessage(hMain, WM_CLOSE, NULL, NULL);
		}
		else if (LOWORD(wParam) == IDC_BUTTON_SELECT_NET)
		{
			ShowSelectMenu(TRUE);
		}
		else if (LOWORD(wParam) == IDC_BUTTON_SELECT_DISK)
		{
			ShowSelectMenu(FALSE);
		}
		else if (LOWORD(wParam) == IDC_BUTTON_FONT || LOWORD(wParam) == IDC_BUTTON_TIPS_FONT)
		{
			typedef UINT_PTR(CALLBACK* LPCFHOOKPROC) (HWND, UINT, WPARAM, LPARAM);
			typedef struct tagCHOOSEFONTW {
				DWORD           lStructSize;
				HWND            hwndOwner;          // caller's window handle
				HDC             hDC;                // printer DC/IC or NULL
				LPLOGFONTW      lpLogFont;          // ptr. to a LOGFONT struct
				INT             iPointSize;         // 10 * size in points of selected font
				DWORD           Flags;              // enum. type flags
				COLORREF        rgbColors;          // returned text color
				LPARAM          lCustData;          // data passed to hook fn.
				LPCFHOOKPROC    lpfnHook;           // ptr. to hook function
				LPCWSTR         lpTemplateName;     // custom template name
				HINSTANCE       hInstance;          // instance handle of.EXE that
													//   contains cust. dlg. template
				LPWSTR          lpszStyle;          // return the style field here
													// must be LF_FACESIZE or bigger
				WORD            nFontType;          // same value reported to the EnumFonts
													//   call back with the extra FONTTYPE_
													//   bits added
				WORD            ___MISSING_ALIGNMENT__;
				INT             nSizeMin;           // minimum pt size allowed &
				INT             nSizeMax;           // max pt size allowed if
													//   CF_LIMITSIZE is used
			} CHOOSEFONT;
			TraySave.TraybarFont.lfHeight = TraySave.TraybarFontSize;
			TraySave.TipsFont.lfHeight = TraySave.TipsFontSize;
			CHOOSEFONT cf;
			cf.lStructSize = sizeof cf;
			cf.hwndOwner = hDlg;
			cf.hDC = NULL;
			if (LOWORD(wParam) == IDC_BUTTON_FONT)
				cf.lpLogFont = &TraySave.TraybarFont;
			else
				cf.lpLogFont = &TraySave.TipsFont;
			cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_EFFECTS;
			cf.nFontType = SCREEN_FONTTYPE;
			cf.rgbColors = RGB(0, 0, 0);
			typedef BOOL(WINAPI* pfnChooseFont)(CHOOSEFONT* lpcf);
			HMODULE hComdlg32 = LoadLibrary(L"comdlg32.dll");
			if (hComdlg32)
			{
				pfnChooseFont ChooseFont = (pfnChooseFont)GetProcAddress(hComdlg32, "ChooseFontW");
				if (ChooseFont)
				{
					if (ChooseFont(&cf))
					{
						if (LOWORD(wParam) == IDC_BUTTON_FONT)
						{
							TraySave.TraybarFontSize = TraySave.TraybarFont.lfHeight;
							otleft = -1;
							SetWH();
							AdjustWindowPos();
						}
						else
							TraySave.TipsFontSize = TraySave.TipsFont.lfHeight;
						WriteReg();
					}
				}
				FreeLibrary(hComdlg32);
			}
		}
		else if (LOWORD(wParam) == IDC_BUTTON_COLOR || (LOWORD(wParam) >= IDC_BUTTON_COLOR_BACKGROUND && LOWORD(wParam) <= IDC_BUTTON_COLOR_HIGH))
		{
			CHOOSECOLOR stChooseColor;
			stChooseColor.lStructSize = sizeof(CHOOSECOLOR);
			stChooseColor.hwndOwner = hDlg;
			if (LOWORD(wParam) == IDC_BUTTON_COLOR)
			{
				stChooseColor.rgbResult = TraySave.dAlphaColor[iProject];
				stChooseColor.lpCustColors = (LPDWORD)&TraySave.dAlphaColor[iProject];
			}
			else
			{
				stChooseColor.rgbResult = TraySave.cMonitorColor[LOWORD(wParam) - IDC_BUTTON_COLOR_BACKGROUND];
				stChooseColor.lpCustColors = TraySave.cMonitorColor;
			}
			stChooseColor.Flags = CC_RGBINIT | CC_FULLOPEN;
			stChooseColor.lCustData = 0;
			stChooseColor.lpfnHook = NULL;
			stChooseColor.lpTemplateName = NULL;
			typedef BOOL(WINAPI* pfnChooseColor)(LPCHOOSECOLOR lpcc);
			HMODULE hComdlg32 = LoadLibrary(L"comdlg32.dll");
			if (hComdlg32)
			{
				pfnChooseColor ChooseColor = (pfnChooseColor)GetProcAddress(hComdlg32, "ChooseColorW");
				if (ChooseColor)
				{
					if (ChooseColor(&stChooseColor))
					{
						if (LOWORD(wParam) == IDC_BUTTON_COLOR)
						{
							TraySave.dAlphaColor[iProject] = stChooseColor.rgbResult;
							DWORD bAlphaB = (int)SendDlgItemMessage(hDlg, IDC_SLIDER_ALPHA_B, TBM_GETPOS, 0, 0);
							bAlphaB = bAlphaB << 24;
							TraySave.dAlphaColor[iProject] = bAlphaB + (TraySave.dAlphaColor[iProject] & 0xffffff);
						}
						else
						{
							TraySave.cMonitorColor[LOWORD(wParam - IDC_BUTTON_COLOR_BACKGROUND)] = stChooseColor.rgbResult;
							if (TraySave.cMonitorColor[0] == 0 || TraySave.cMonitorColor[0] == RGB(0, 0, 1))
							{
								TraySave.cMonitorColor[0] = RGB(0, 0, 1);
//								if (TraySave.bMonitorFloat||rovi.dwBuildNumber<=22000)
								{
									bShadow = TRUE;
									TraySave.bMonitorFuse = TRUE;
								}
							}
							else
								bShadow = FALSE;
							CheckDlgButton(hSetting, IDC_CHECK_FUSE, TraySave.bMonitorFuse);
							CloseTaskBar();
						}
						::InvalidateRect(GetDlgItem(hMain, LOWORD(wParam)), NULL, FALSE);
					}
				}
				FreeLibrary(hComdlg32);
			}
			WriteReg();
			//			SendMessage(hMain, WM_TRAYS, NULL, NULL);
		}
		break;
	}
	return (INT_PTR)FALSE;
}
INT_PTR CALLBACK ColorButtonProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)//颜色按钮控件过程
{
	switch (message)
	{
	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hWnd, &ps);
		RECT rc;
		GetClientRect(hWnd, &rc);
		HBRUSH hb;
		int id = GetDlgCtrlID(hWnd);
		if (id >= IDC_BUTTON_COLOR_BACKGROUND && id <= IDC_BUTTON_COLOR_HIGH)
		{
			hb = CreateSolidBrush(TraySave.cMonitorColor[id - IDC_BUTTON_COLOR_BACKGROUND]);
		}
		else
			hb = CreateSolidBrush(TraySave.dAlphaColor[iProject] & 0xffffff);
		FillRect(hdc, &rc, hb);
		DeleteObject(hb);
		SetBkMode(hdc, TRANSPARENT);
		SetTextColor(hdc, RGB(255, 255, 255));

		HFONT oldFont = (HFONT)SelectObject(hdc, hFont);
		WCHAR sz[8];
		GetWindowText(hWnd, sz, 8);
		DrawShadowText(hdc, sz, lstrlen(sz), &rc, DT_CENTER | DT_VCENTER| DT_SINGLELINE,bColor,TRUE);
		SelectObject(hdc, oldFont);
		EndPaint(hWnd, &ps);
		return TRUE;
	}
	}
	return CallWindowProc(oldColorButtonPoroc, hWnd, message, wParam, lParam);
}
 void ShowSelectMenu(BOOL bNet)
{
	HMENU hMenu = LoadMenu(hInst, MAKEINTRESOURCEW(IDR_MENU));
	if (!hMenu)
		return;
	HMENU subMenu;
	if (bNet)
	{
		subMenu = GetSubMenu(hMenu, 0);
		TRAFFIC* trafficData = (TRAFFIC*)HeapAlloc(
			GetProcessHeap(),
			HEAP_ZERO_MEMORY,
			sizeof(TRAFFIC) * MAX_TRAFFIC_ADAPTERS);
		if (!trafficData)
		{
			DestroyMenu(hMenu);
			return;
		}
		int trafficCount = ReadTrafficSnapshot(trafficData, MAX_TRAFFIC_ADAPTERS);
		CheckMenuRadioItem(subMenu, IDC_SELECT_ALL, IDC_SELECT_ALL + 99, IDC_SELECT_ALL, MF_BYCOMMAND);
		for (int i = 0; i < trafficCount && i < 99; ++i)
		{
			AppendMenu(subMenu, MF_BYCOMMAND, IDC_SELECT_ALL + i + 1, trafficData[i].FriendlyName);
			if (lstrcmpA(trafficData[i].AdapterName, TraySave.AdpterName) == 0)
				CheckMenuRadioItem(subMenu, IDC_SELECT_ALL, IDC_SELECT_ALL + 99, IDC_SELECT_ALL + i + 1, MF_BYCOMMAND);
		}
		HeapFree(GetProcessHeap(), 0, trafficData);
	}
	else
	{
		subMenu = GetSubMenu(hMenu, 1);
		WCHAR wDrive[MAX_PATH];
		DWORD dwLen = GetLogicalDriveStrings(MAX_PATH, wDrive);
		if (dwLen)
		{
			CheckMenuRadioItem(subMenu, IDC_DISK_ALL, IDC_DISK_ALL + 99, IDC_DISK_ALL, MF_BYCOMMAND);
			DWORD driver_number = dwLen / 4;
			for (DWORD nIndex = 0; nIndex < driver_number; nIndex++)
			{
				LPWSTR dName = wDrive + nIndex * 4;
				if (GetDriveType(dName) != DRIVE_CDROM)
				{
					if (GetPhysicalDriveFromPartitionLetter(dName[0]) != -1)
					{
						dName[2] = 0;
						AppendMenu(subMenu, MF_BYCOMMAND, IDC_DISK_ALL + dName[0], dName);
						if (TraySave.szDisk == dName[0])
							CheckMenuRadioItem(subMenu, IDC_DISK_ALL, IDC_DISK_ALL + 99, IDC_DISK_ALL + dName[0], MF_BYCOMMAND);
					}
				}
			}
		}
	}
	POINT point;
	GetCursorPos(&point);
	SetTimer(hTaskBar, 5, 1200, NULL);
	TrackPopupMenu(subMenu, TPM_LEFTALIGN, point.x, point.y, NULL, hTaskBar, NULL);
	DestroyMenu(hMenu);
}
