#include "ProcessMonitor.h"
#include "ProcessMonitorResource.h"

#include <commctrl.h>
#include <commdlg.h>
#include <objbase.h>
#include <psapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <pdh.h>

#ifndef PDH_MORE_DATA
#define PDH_MORE_DATA ((PDH_STATUS)0x800007D2L)
#endif
#ifndef PDH_CSTATUS_VALID_DATA
#define PDH_CSTATUS_VALID_DATA ((DWORD)0x00000000)
#endif
#ifndef PDH_CSTATUS_NEW_DATA
#define PDH_CSTATUS_NEW_DATA ((DWORD)0x00000001)
#endif
#ifndef PDH_CSTATUS_NO_INSTANCE
#define PDH_CSTATUS_NO_INSTANCE ((PDH_STATUS)0x800007D1L)
#endif
#ifndef PDH_NO_DATA
#define PDH_NO_DATA ((PDH_STATUS)0x800007D5L)
#endif

#define PM_MAX_RULES 64
#define PM_MAX_PATH_CHARS 32768
#define PM_MAX_INSTANCE_STATES 512
#define PM_MAX_GPU_PIDS 512
#define PM_MAX_UI_EVENTS 64
#define PM_MAX_ACTIONS 32
#define PM_MAX_ALERTS 32
#define PM_CONFIG_MAX_PAYLOAD (1024 * 1024)
#define PM_LOG_MAX_BYTES (1024 * 1024)

#define PM_METRIC_CPU 0x1
#define PM_METRIC_MEMORY 0x2
#define PM_METRIC_GPU 0x4

#define PM_ACTION_PROMPT 0
#define PM_ACTION_AUTO_TERMINATE 1

#define PM_TASK_ALERT_TERMINATE 1600
#define PM_TASK_ALERT_IGNORE 1601
#define PM_ALERT_TIMEOUT_MS 15000

#define PM_UI_ALERT 1
#define PM_UI_CLEAR 2
#define PM_UI_RESULT 3
#define PM_UI_CLEAR_ALL 4

#define PM_CONFIG_MAGIC 0x31524D50
#define PM_CONFIG_VERSION 1

typedef struct _PM_RULE
{
	GUID id;
	BOOL enabled;
	DWORD metricMask;
	DWORD cpuThreshold;
	DWORD memoryThreshold;
	DWORD gpuThreshold;
	DWORD holdSeconds;
	DWORD action;
	WCHAR exeName[MAX_PATH];
	WCHAR* path;
} PM_RULE;

typedef struct _PM_CONFIG
{
	BOOL enabled;
	DWORD count;
	ULONGLONG generation;
	PM_RULE rules[PM_MAX_RULES]; // Paths are allocated to their actual lengths; unused slots stay small.
} PM_CONFIG;

typedef struct _PM_INCIDENT
{
	GUID ruleId;
	ULONGLONG generation;
	DWORD pid;
	ULONGLONG createTime;
	DWORD metricMask;
	DWORD cpuValue;
	DWORD memoryValue;
	DWORD gpuValue;
	DWORD cpuThreshold;
	DWORD memoryThreshold;
	DWORD gpuThreshold;
	DWORD holdSeconds;
	DWORD action;
	WCHAR* path;
} PM_INCIDENT;

typedef struct _PM_UI_EVENT
{
	DWORD type;
	PM_INCIDENT incident;
	WCHAR message[768];
} PM_UI_EVENT;

typedef struct _PM_INSTANCE_STATE
{
	BOOL used;
	DWORD ruleIndex;
	DWORD pid;
	ULONGLONG createTime;
	ULONGLONG previousCpuTime;
	ULONGLONG previousTick;
	BOOL cpuBaseline;
	ULONGLONG overSince[3];
	BOOL latched;
	DWORD triggeredMask;
	DWORD recoveryCount;
	DWORD seenCycle;
} PM_INSTANCE_STATE;

typedef struct _PM_GPU_USAGE
{
	DWORD pid;
	double value;
} PM_GPU_USAGE;

typedef struct _PM_RULE_RUNTIME
{
	GUID ruleId;
	DWORD runningInstances;
	BOOL gpuRequested;
	BOOL gpuAvailable;
} PM_RULE_RUNTIME;

typedef struct _PM_PICK_ENTRY
{
	DWORD pid;
	WCHAR* path;
} PM_PICK_ENTRY;

typedef struct _PM_EDIT_CONTEXT
{
	BOOL editing;
	DWORD index;
	PM_RULE* candidate;
	BOOL accepted;
} PM_EDIT_CONTEXT;

typedef PDH_STATUS(WINAPI* PM_PDH_OPEN_QUERY)(LPCWSTR, DWORD_PTR, PDH_HQUERY*);
typedef PDH_STATUS(WINAPI* PM_PDH_ADD_ENGLISH_COUNTER)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER*);
typedef PDH_STATUS(WINAPI* PM_PDH_COLLECT_QUERY_DATA)(PDH_HQUERY);
typedef PDH_STATUS(WINAPI* PM_PDH_GET_FORMATTED_COUNTER_ARRAY)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD, PPDH_FMT_COUNTERVALUE_ITEM_W);
typedef PDH_STATUS(WINAPI* PM_PDH_CLOSE_QUERY)(PDH_HQUERY);
typedef BOOL(WINAPI* PM_GET_PROCESS_MEMORY_INFO)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
typedef BOOL(WINAPI* PM_IS_PROCESS_CRITICAL)(HANDLE, PBOOL);
typedef HRESULT(WINAPI* PM_TASK_DIALOG_INDIRECT)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);

static HWND g_mainWindow;
static HWND g_rulesWindow;
static HWND g_alertWindow;
static HWND g_resultWindow;
static HINSTANCE g_instance;
static HICON g_appIcon;
static UINT g_trayIconId;
static HANDLE g_stopEvent;
static HANDLE g_configChangedEvent;
static HANDLE g_actionEvent;
static HANDLE g_monitorThread;
static HANDLE g_actionThread;
static CRITICAL_SECTION g_configLock;
static CRITICAL_SECTION g_uiLock;
static CRITICAL_SECTION g_actionLock;
static CRITICAL_SECTION g_logLock;
static BOOL g_locksInitialized;
static PM_CONFIG g_config;
static WCHAR g_configStatus[256];
static WCHAR* g_moduleDirectory;
static WCHAR* g_selfPath;

static PM_UI_EVENT g_uiEvents[PM_MAX_UI_EVENTS];
static DWORD g_uiHead;
static DWORD g_uiTail;
static DWORD g_uiCount;
static PM_INCIDENT g_actions[PM_MAX_ACTIONS];
static DWORD g_actionHead;
static DWORD g_actionTail;
static DWORD g_actionCount;
static PM_INCIDENT g_alerts[PM_MAX_ALERTS];
static DWORD g_alertHead;
static DWORD g_alertTail;
static DWORD g_alertCount;
static PM_INCIDENT g_currentAlert;
static BOOL g_hasCurrentAlert;
static BOOL g_taskDialogRunning;
static BOOL g_alertNotificationActive;
static BOOL g_alertTemporaryTrayIcon;
static BOOL g_trayIconAvailable;
static UINT g_alertNotificationIconId;
static UINT_PTR g_alertTimerId;
static PM_UI_EVENT g_dispatchEvent;
static WCHAR g_resultMessage[768];
static PM_RULE_RUNTIME g_runtime[PM_MAX_RULES];
static DWORD g_runtimeCount;
static CRITICAL_SECTION g_runtimeLock;
static PM_PICK_ENTRY* g_pickerEntries[PM_MAX_INSTANCE_STATES];
static DWORD g_pickerEntryCount;

static PM_CONFIG* g_workerConfig;
static PM_INSTANCE_STATE g_states[PM_MAX_INSTANCE_STATES];
static DWORD g_sampleCycle;

static HMODULE g_pdhModule;
static PM_PDH_OPEN_QUERY g_pdhOpenQuery;
static PM_PDH_ADD_ENGLISH_COUNTER g_pdhAddEnglishCounter;
static PM_PDH_COLLECT_QUERY_DATA g_pdhCollectQueryData;
static PM_PDH_GET_FORMATTED_COUNTER_ARRAY g_pdhGetFormattedCounterArray;
static PM_PDH_CLOSE_QUERY g_pdhCloseQuery;
static BOOL g_pdhLoadAttempted;
static PDH_HQUERY g_gpuQuery;
static PDH_HCOUNTER g_gpuCounter;
static BOOL g_gpuHasBaseline;
static BOOL g_gpuAvailable;
static PM_GPU_USAGE g_gpuUsage[PM_MAX_GPU_PIDS];
static DWORD g_gpuUsageCount;
static DWORD g_gpuFailureCount;

static HMODULE g_psapiModule;
static PM_GET_PROCESS_MEMORY_INFO g_getProcessMemoryInfo;
static PM_IS_PROCESS_CRITICAL g_isProcessCritical;

static INT_PTR CALLBACK RulesDialogProc(HWND, UINT, WPARAM, LPARAM);
static INT_PTR CALLBACK RuleEditDialogProc(HWND, UINT, WPARAM, LPARAM);
static INT_PTR CALLBACK ProcessPickerDialogProc(HWND, UINT, WPARAM, LPARAM);
static INT_PTR CALLBACK AlertDialogProc(HWND, UINT, WPARAM, LPARAM);
static INT_PTR CALLBACK ResultDialogProc(HWND, UINT, WPARAM, LPARAM);
static HRESULT CALLBACK AlertTaskDialogCallback(HWND, UINT, WPARAM, LPARAM, LONG_PTR);
static void QueueUiEvent(DWORD, const PM_INCIDENT*, const WCHAR*);
static void ShowNextAlert();
static void FinishCurrentAlert(BOOL);
static void DismissAlertNotification();
static VOID CALLBACK AlertNotificationTimerProc(HWND, UINT, UINT_PTR, DWORD);

static ULONGLONG FileTimeToUInt64(const FILETIME* value)
{
	return ((ULONGLONG)value->dwHighDateTime << 32) | value->dwLowDateTime;
}

static BOOL GuidEqual(const GUID* left, const GUID* right)
{
	return left->Data1 == right->Data1 && left->Data2 == right->Data2 && left->Data3 == right->Data3 &&
		left->Data4[0] == right->Data4[0] && left->Data4[1] == right->Data4[1] &&
		left->Data4[2] == right->Data4[2] && left->Data4[3] == right->Data4[3] &&
		left->Data4[4] == right->Data4[4] && left->Data4[5] == right->Data4[5] &&
		left->Data4[6] == right->Data4[6] && left->Data4[7] == right->Data4[7];
}

static void CopyText(WCHAR* destination, DWORD destinationCount, const WCHAR* source)
{
	if (!destination || destinationCount == 0)
		return;
	if (!source)
	{
		destination[0] = 0;
		return;
	}
	DWORD index = 0;
	while (index + 1 < destinationCount && source[index])
	{
		destination[index] = source[index];
		++index;
	}
	destination[index] = 0;
}

static WCHAR* DuplicateText(const WCHAR* source)
{
	if (!source)
		return NULL;
	SIZE_T characters = (SIZE_T)lstrlenW(source) + 1;
	if (characters > PM_MAX_PATH_CHARS)
		return NULL;
	WCHAR* copy = (WCHAR*)HeapAlloc(GetProcessHeap(), 0, characters * sizeof(WCHAR));
	if (copy)
		CopyText(copy, (DWORD)characters, source);
	return copy;
}

static void FreeRule(PM_RULE* rule)
{
	if (!rule)
		return;
	if (rule->path)
		HeapFree(GetProcessHeap(), 0, rule->path);
	ZeroMemory(rule, sizeof(*rule));
}

static BOOL CopyRule(PM_RULE* destination, const PM_RULE* source)
{
	if (!destination || !source)
		return FALSE;
	if (destination == source)
		return TRUE;
	WCHAR* path = source->path ? DuplicateText(source->path) : NULL;
	if (source->path && !path)
		return FALSE;
	FreeRule(destination);
	*destination = *source;
	destination->path = path;
	return TRUE;
}

static void FreeConfiguration(PM_CONFIG* config)
{
	if (!config)
		return;
	for (DWORD index = 0; index < config->count && index < PM_MAX_RULES; ++index)
		FreeRule(&config->rules[index]);
	ZeroMemory(config, sizeof(*config));
}

static PM_CONFIG* CloneConfiguration(const PM_CONFIG* source)
{
	if (!source || source->count > PM_MAX_RULES)
		return NULL;
	PM_CONFIG* copy = (PM_CONFIG*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PM_CONFIG));
	if (!copy)
		return NULL;
	copy->enabled = source->enabled;
	copy->count = source->count;
	copy->generation = source->generation;
	for (DWORD index = 0; index < source->count; ++index)
	{
		if (!CopyRule(&copy->rules[index], &source->rules[index]))
		{
			FreeConfiguration(copy);
			HeapFree(GetProcessHeap(), 0, copy);
			return NULL;
		}
	}
	return copy;
}

static void MoveConfiguration(PM_CONFIG* destination, PM_CONFIG* source)
{
	if (!destination || !source || destination == source)
		return;
	FreeConfiguration(destination);
	destination->enabled = source->enabled;
	destination->count = source->count;
	destination->generation = source->generation;
	for (DWORD index = 0; index < source->count; ++index)
	{
		destination->rules[index] = source->rules[index];
		source->rules[index].path = NULL;
	}
	ZeroMemory(source, sizeof(*source));
}

static void FreeIncident(PM_INCIDENT* incident)
{
	if (!incident)
		return;
	if (incident->path)
		HeapFree(GetProcessHeap(), 0, incident->path);
	ZeroMemory(incident, sizeof(*incident));
}

static BOOL CopyIncident(PM_INCIDENT* destination, const PM_INCIDENT* source)
{
	if (!destination || !source)
		return FALSE;
	if (destination == source)
		return TRUE;
	WCHAR* path = source->path ? DuplicateText(source->path) : NULL;
	if (source->path && !path)
		return FALSE;
	FreeIncident(destination);
	*destination = *source;
	destination->path = path;
	return TRUE;
}

static void MoveIncident(PM_INCIDENT* destination, PM_INCIDENT* source)
{
	if (!destination || !source || destination == source)
		return;
	FreeIncident(destination);
	*destination = *source;
	source->path = NULL;
	ZeroMemory(source, sizeof(*source));
}

static void AppendText(WCHAR* destination, DWORD destinationCount, const WCHAR* source)
{
	DWORD length = lstrlenW(destination);
	if (length >= destinationCount - 1)
		return;
	CopyText(destination + length, destinationCount - length, source);
}

static const WCHAR* BaseName(const WCHAR* path)
{
	const WCHAR* result = path;
	if (!path)
		return L"";
	for (const WCHAR* current = path; *current; ++current)
		if (*current == L'\\' || *current == L'/')
			result = current + 1;
	return result;
}

static BOOL HasExeExtension(const WCHAR* path)
{
	const WCHAR* name = BaseName(path);
	DWORD length = lstrlenW(name);
	return length > 4 && lstrcmpiW(name + length - 4, L".exe") == 0;
}

static void RemoveExtendedPrefix(WCHAR* path)
{
	if (!path)
		return;
	if (lstrlenW(path) > 8 && path[0] == L'\\' && path[1] == L'\\' && path[2] == L'?' && path[3] == L'\\')
	{
		if ((path[4] == L'U' || path[4] == L'u') && (path[5] == L'N' || path[5] == L'n') &&
			(path[6] == L'C' || path[6] == L'c') && path[7] == L'\\')
		{
			DWORD length = lstrlenW(path + 8);
			for (DWORD index = 0; index <= length; ++index)
				path[index + 2] = path[index + 8];
			path[0] = L'\\';
			path[1] = L'\\';
		}
		else
		{
			DWORD length = lstrlenW(path + 4);
			for (DWORD index = 0; index <= length; ++index)
				path[index] = path[index + 4];
		}
	}
}

static BOOL NormalizePath(const WCHAR* input, WCHAR* output, DWORD outputCount)
{
	if (!input || !output || outputCount < 2)
		return FALSE;
	WCHAR* first = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, PM_MAX_PATH_CHARS * sizeof(WCHAR));
	WCHAR* second = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, PM_MAX_PATH_CHARS * sizeof(WCHAR));
	if (!first || !second)
	{
		HeapFree(GetProcessHeap(), 0, first);
		HeapFree(GetProcessHeap(), 0, second);
		return FALSE;
	}
	const WCHAR* begin = input;
	while (*begin == L' ' || *begin == L'\t' || *begin == L'"')
		++begin;
	CopyText(first, PM_MAX_PATH_CHARS, begin);
	DWORD length = lstrlenW(first);
	while (length > 0 && (first[length - 1] == L' ' || first[length - 1] == L'\t' || first[length - 1] == L'"'))
		first[--length] = 0;
	for (DWORD index = 0; first[index]; ++index)
		if (first[index] == L'/')
			first[index] = L'\\';
	DWORD fullLength = GetFullPathNameW(first, PM_MAX_PATH_CHARS, second, NULL);
	if (fullLength == 0 || fullLength >= PM_MAX_PATH_CHARS)
		CopyText(second, PM_MAX_PATH_CHARS, first);
	DWORD longLength = GetLongPathNameW(second, first, PM_MAX_PATH_CHARS);
	if (longLength == 0 || longLength >= PM_MAX_PATH_CHARS)
		CopyText(first, PM_MAX_PATH_CHARS, second);
	RemoveExtendedPrefix(first);
	if ((DWORD)lstrlenW(first) >= outputCount)
	{
		HeapFree(GetProcessHeap(), 0, first);
		HeapFree(GetProcessHeap(), 0, second);
		return FALSE;
	}
	CopyText(output, outputCount, first);
	HeapFree(GetProcessHeap(), 0, first);
	HeapFree(GetProcessHeap(), 0, second);
	return output[0] != 0;
}

static BOOL QueryProcessPath(HANDLE process, WCHAR* path, DWORD pathCount)
{
	DWORD length = pathCount;
	if (!QueryFullProcessImageNameW(process, 0, path, &length))
		return FALSE;
	path[pathCount - 1] = 0;
	RemoveExtendedPrefix(path);
	for (DWORD index = 0; path[index]; ++index)
		if (path[index] == L'/')
			path[index] = L'\\';
	return TRUE;
}

static void BuildModulePath(const WCHAR* fileName, WCHAR* output, DWORD outputCount)
{
	CopyText(output, outputCount, g_moduleDirectory);
	if (output[0] && output[lstrlenW(output) - 1] != L'\\')
		AppendText(output, outputCount, L"\\");
	AppendText(output, outputCount, fileName);
}

static DWORD CalculateCrc32(const BYTE* data, DWORD size)
{
	DWORD crc = 0xffffffff;
	for (DWORD index = 0; index < size; ++index)
	{
		crc ^= data[index];
		for (DWORD bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ (0xedb88320 & (0 - (crc & 1)));
	}
	return ~crc;
}

static BOOL WriteDword(BYTE* buffer, DWORD capacity, DWORD* offset, DWORD value)
{
	if (*offset > capacity || capacity - *offset < sizeof(value))
		return FALSE;
	CopyMemory(buffer + *offset, &value, sizeof(value));
	*offset += sizeof(value);
	return TRUE;
}

static BOOL ReadDword(const BYTE* buffer, DWORD size, DWORD* offset, DWORD* value)
{
	if (*offset > size || size - *offset < sizeof(*value))
		return FALSE;
	CopyMemory(value, buffer + *offset, sizeof(*value));
	*offset += sizeof(*value);
	return TRUE;
}

static void SetConfigStatus(const WCHAR* status)
{
	CopyText(g_configStatus, ARRAYSIZE(g_configStatus), status);
}

static BOOL ValidateRule(const PM_RULE* rule)
{
	if (!rule || !rule->path || !rule->path[0] || !rule->exeName[0] || !HasExeExtension(rule->path))
		return FALSE;
	if ((rule->metricMask & (PM_METRIC_CPU | PM_METRIC_MEMORY | PM_METRIC_GPU)) == 0 ||
		(rule->metricMask & ~(PM_METRIC_CPU | PM_METRIC_MEMORY | PM_METRIC_GPU)) != 0)
		return FALSE;
	if (rule->cpuThreshold < 1 || rule->cpuThreshold > 100 ||
		rule->memoryThreshold < 1 || rule->memoryThreshold > 100 ||
		rule->gpuThreshold < 1 || rule->gpuThreshold > 100 ||
		rule->holdSeconds < 1 || rule->holdSeconds > 300)
		return FALSE;
	if (rule->action != PM_ACTION_PROMPT && rule->action != PM_ACTION_AUTO_TERMINATE)
		return FALSE;
	return rule->enabled == FALSE || rule->enabled == TRUE;
}

static BOOL SaveConfigurationLocked(const PM_CONFIG* config)
{
	if (!config || config->count > PM_MAX_RULES)
	{
		SetLastError(ERROR_INVALID_DATA);
		return FALSE;
	}
	BYTE* payload = (BYTE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, PM_CONFIG_MAX_PAYLOAD);
	if (!payload)
		return FALSE;
	DWORD offset = 0;
	BOOL valid = TRUE;
	for (DWORD index = 0; index < config->count && valid; ++index)
	{
		const PM_RULE* rule = &config->rules[index];
		if (!ValidateRule(rule))
		{
			valid = FALSE;
			break;
		}
		DWORD pathChars = lstrlenW(rule->path);
		DWORD recordSize = sizeof(DWORD) + sizeof(GUID) + sizeof(DWORD) * 8 + pathChars * sizeof(WCHAR);
		valid = pathChars > 0 && pathChars < PM_MAX_PATH_CHARS &&
			WriteDword(payload, PM_CONFIG_MAX_PAYLOAD, &offset, recordSize);
		if (valid && offset <= PM_CONFIG_MAX_PAYLOAD && PM_CONFIG_MAX_PAYLOAD - offset >= sizeof(GUID))
		{
			CopyMemory(payload + offset, &rule->id, sizeof(GUID));
			offset += sizeof(GUID);
		}
		else
			valid = FALSE;
		valid = valid && WriteDword(payload, PM_CONFIG_MAX_PAYLOAD, &offset, rule->enabled) &&
			WriteDword(payload, PM_CONFIG_MAX_PAYLOAD, &offset, rule->metricMask) &&
			WriteDword(payload, PM_CONFIG_MAX_PAYLOAD, &offset, rule->cpuThreshold) &&
			WriteDword(payload, PM_CONFIG_MAX_PAYLOAD, &offset, rule->memoryThreshold) &&
			WriteDword(payload, PM_CONFIG_MAX_PAYLOAD, &offset, rule->gpuThreshold) &&
			WriteDword(payload, PM_CONFIG_MAX_PAYLOAD, &offset, rule->holdSeconds) &&
			WriteDword(payload, PM_CONFIG_MAX_PAYLOAD, &offset, rule->action) &&
			WriteDword(payload, PM_CONFIG_MAX_PAYLOAD, &offset, pathChars);
		DWORD pathBytes = pathChars * sizeof(WCHAR);
		if (valid && offset <= PM_CONFIG_MAX_PAYLOAD && PM_CONFIG_MAX_PAYLOAD - offset >= pathBytes)
		{
			CopyMemory(payload + offset, rule->path, pathBytes);
			offset += pathBytes;
		}
		else
			valid = FALSE;
	}
	if (!valid)
	{
		HeapFree(GetProcessHeap(), 0, payload);
		SetLastError(ERROR_INVALID_DATA);
		return FALSE;
	}
	DWORD header[6];
	header[0] = PM_CONFIG_MAGIC;
	header[1] = PM_CONFIG_VERSION;
	header[2] = config->enabled;
	header[3] = config->count;
	header[4] = offset;
	header[5] = CalculateCrc32(payload, offset);
	WCHAR* destination = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, PM_MAX_PATH_CHARS * sizeof(WCHAR));
	WCHAR* temporary = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, PM_MAX_PATH_CHARS * sizeof(WCHAR));
	if (!destination || !temporary)
	{
		HeapFree(GetProcessHeap(), 0, destination);
		HeapFree(GetProcessHeap(), 0, temporary);
		HeapFree(GetProcessHeap(), 0, payload);
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	BuildModulePath(L"TrayS.ProcessRules.dat", destination, PM_MAX_PATH_CHARS);
	BuildModulePath(L"TrayS.ProcessRules.dat.tmp", temporary, PM_MAX_PATH_CHARS);
	HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
	BOOL result = FALSE;
	if (file != INVALID_HANDLE_VALUE)
	{
		DWORD written = 0;
		result = WriteFile(file, header, sizeof(header), &written, NULL) && written == sizeof(header);
		if (result && offset > 0)
			result = WriteFile(file, payload, offset, &written, NULL) && written == offset;
		if (result)
			result = FlushFileBuffers(file);
		CloseHandle(file);
	}
	if (result)
	{
		if (GetFileAttributesW(destination) != INVALID_FILE_ATTRIBUTES)
			result = ReplaceFileW(destination, temporary, NULL, REPLACEFILE_IGNORE_MERGE_ERRORS, NULL, NULL);
		else
			result = MoveFileExW(temporary, destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	}
	if (!result)
		DeleteFileW(temporary);
	HeapFree(GetProcessHeap(), 0, destination);
	HeapFree(GetProcessHeap(), 0, temporary);
	HeapFree(GetProcessHeap(), 0, payload);
	return result;
}

static BOOL CommitConfiguration(PM_CONFIG* candidate)
{
	if (!candidate)
		return FALSE;
	BOOL result = FALSE;
	EnterCriticalSection(&g_configLock);
	candidate->generation = g_config.generation + 1;
	result = SaveConfigurationLocked(candidate);
	if (result)
	{
		MoveConfiguration(&g_config, candidate);
		SetConfigStatus(L"");
	}
	LeaveCriticalSection(&g_configLock);
	FreeConfiguration(candidate);
	HeapFree(GetProcessHeap(), 0, candidate);
	if (result)
	{
		SetEvent(g_configChangedEvent);
		QueueUiEvent(PM_UI_CLEAR_ALL, NULL, NULL);
	}
	return result;
}

static void LoadConfiguration()
{
	FreeConfiguration(&g_config);
	g_config.generation = 1;
	SetConfigStatus(L"");
	WCHAR* path = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, PM_MAX_PATH_CHARS * sizeof(WCHAR));
	if (!path)
	{
		SetConfigStatus(L"内存不足，进程监控已安全关闭。");
		return;
	}
	BuildModulePath(L"TrayS.ProcessRules.dat", path, PM_MAX_PATH_CHARS);
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD openError = file == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
	HeapFree(GetProcessHeap(), 0, path);
	if (file == INVALID_HANDLE_VALUE)
	{
		if (openError != ERROR_FILE_NOT_FOUND && openError != ERROR_PATH_NOT_FOUND)
			SetConfigStatus(L"无法读取进程规则文件，监控已安全关闭。");
		return;
	}
	LARGE_INTEGER fileSize;
	if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart < (LONGLONG)sizeof(DWORD) * 6 ||
		fileSize.QuadPart > (LONGLONG)sizeof(DWORD) * 6 + PM_CONFIG_MAX_PAYLOAD)
	{
		CloseHandle(file);
		SetConfigStatus(L"进程规则文件大小无效，监控已安全关闭。");
		return;
	}
	DWORD header[6];
	DWORD read = 0;
	BOOL ok = ReadFile(file, header, sizeof(header), &read, NULL) && read == sizeof(header);
	BYTE* payload = NULL;
	if (ok && header[0] == PM_CONFIG_MAGIC && header[1] == PM_CONFIG_VERSION &&
		header[2] <= 1 && header[3] <= PM_MAX_RULES && header[4] <= PM_CONFIG_MAX_PAYLOAD &&
		fileSize.QuadPart == (LONGLONG)sizeof(header) + header[4])
	{
		payload = (BYTE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, header[4] ? header[4] : 1);
		ok = payload != NULL;
		if (ok && header[4] > 0)
			ok = ReadFile(file, payload, header[4], &read, NULL) && read == header[4];
		if (ok)
			ok = header[5] == CalculateCrc32(payload, header[4]);
	}
	else
		ok = FALSE;
	CloseHandle(file);
	DWORD offset = 0;
	if (ok)
	{
		g_config.enabled = header[2];
		g_config.count = header[3];
		for (DWORD index = 0; index < g_config.count && ok; ++index)
		{
			PM_RULE* rule = &g_config.rules[index];
			DWORD recordStart = offset;
			DWORD recordSize = 0;
			DWORD pathChars = 0;
			ok = ReadDword(payload, header[4], &offset, &recordSize);
			if (ok && offset <= header[4] && header[4] - offset >= sizeof(GUID))
			{
				CopyMemory(&rule->id, payload + offset, sizeof(GUID));
				offset += sizeof(GUID);
			}
			else
				ok = FALSE;
			ok = ok && ReadDword(payload, header[4], &offset, (DWORD*)&rule->enabled) &&
				ReadDword(payload, header[4], &offset, &rule->metricMask) &&
				ReadDword(payload, header[4], &offset, &rule->cpuThreshold) &&
				ReadDword(payload, header[4], &offset, &rule->memoryThreshold) &&
				ReadDword(payload, header[4], &offset, &rule->gpuThreshold) &&
				ReadDword(payload, header[4], &offset, &rule->holdSeconds) &&
				ReadDword(payload, header[4], &offset, &rule->action) &&
				ReadDword(payload, header[4], &offset, &pathChars);
			DWORD pathBytes = pathChars * sizeof(WCHAR);
			if (!ok || pathChars == 0 || pathChars >= PM_MAX_PATH_CHARS || offset > header[4] || header[4] - offset < pathBytes)
				ok = FALSE;
			else
			{
				rule->path = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ((SIZE_T)pathChars + 1) * sizeof(WCHAR));
				ok = rule->path != NULL;
				if (ok)
				{
					CopyMemory(rule->path, payload + offset, pathBytes);
					rule->path[pathChars] = 0;
					offset += pathBytes;
					CopyText(rule->exeName, ARRAYSIZE(rule->exeName), BaseName(rule->path));
					ok = ValidateRule(rule) && recordSize == offset - recordStart;
				}
				for (DWORD previous = 0; previous < index && ok; ++previous)
					if (lstrcmpiW(g_config.rules[previous].path, rule->path) == 0 ||
						GuidEqual(&g_config.rules[previous].id, &rule->id))
						ok = FALSE;
			}
		}
		if (offset != header[4])
			ok = FALSE;
	}
	HeapFree(GetProcessHeap(), 0, payload);
	if (!ok)
	{
		FreeConfiguration(&g_config);
		g_config.generation = 1;
		SetConfigStatus(L"进程规则文件已损坏或版本不受支持，监控已安全关闭。");
	}
}

static void RotateLogIfNeeded(const WCHAR* path)
{
	WIN32_FILE_ATTRIBUTE_DATA data;
	if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data))
		return;
	ULONGLONG size = ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow;
	if (size < PM_LOG_MAX_BYTES)
		return;
	WCHAR* paths = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
		(SIZE_T)PM_MAX_PATH_CHARS * 3 * sizeof(WCHAR));
	if (!paths)
		return;
	WCHAR* path1 = paths;
	WCHAR* path2 = paths + PM_MAX_PATH_CHARS;
	WCHAR* path3 = paths + PM_MAX_PATH_CHARS * 2;
	CopyText(path1, PM_MAX_PATH_CHARS, path); AppendText(path1, PM_MAX_PATH_CHARS, L".1");
	CopyText(path2, PM_MAX_PATH_CHARS, path); AppendText(path2, PM_MAX_PATH_CHARS, L".2");
	CopyText(path3, PM_MAX_PATH_CHARS, path); AppendText(path3, PM_MAX_PATH_CHARS, L".3");
	DeleteFileW(path3);
	MoveFileExW(path2, path3, MOVEFILE_REPLACE_EXISTING);
	MoveFileExW(path1, path2, MOVEFILE_REPLACE_EXISTING);
	MoveFileExW(path, path1, MOVEFILE_REPLACE_EXISTING);
	HeapFree(GetProcessHeap(), 0, paths);
}

static BOOL WriteUtf8(HANDLE file, const WCHAR* text)
{
	if (!text || !*text)
		return TRUE;
	int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
	if (bytes <= 1)
		return FALSE;
	CHAR* buffer = (CHAR*)HeapAlloc(GetProcessHeap(), 0, bytes);
	if (!buffer)
		return FALSE;
	BOOL result = WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, bytes, NULL, NULL) > 0;
	DWORD written = 0;
	if (result)
		result = WriteFile(file, buffer, bytes - 1, &written, NULL) && written == (DWORD)(bytes - 1);
	HeapFree(GetProcessHeap(), 0, buffer);
	return result;
}

static void Audit(const WCHAR* eventName, const PM_INCIDENT* incident, const WCHAR* message)
{
	if (!g_locksInitialized)
		return;
	EnterCriticalSection(&g_logLock);
	WCHAR* path = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, PM_MAX_PATH_CHARS * sizeof(WCHAR));
	if (!path)
	{
		LeaveCriticalSection(&g_logLock);
		return;
	}
	BuildModulePath(L"TrayS.ProcessMonitor.log", path, PM_MAX_PATH_CHARS);
	RotateLogIfNeeded(path);
	HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file != INVALID_HANDLE_VALUE)
	{
		SYSTEMTIME time;
		GetLocalTime(&time);
		WCHAR prefix[256];
		DWORD pid = incident ? incident->pid : 0;
		wsprintfW(prefix, L"%04u-%02u-%02u %02u:%02u:%02u\t%s\tPID=%u\t", time.wYear, time.wMonth,
			time.wDay, time.wHour, time.wMinute, time.wSecond, eventName ? eventName : L"event", pid);
		WriteUtf8(file, prefix);
		if (incident)
			WriteUtf8(file, incident->path ? incident->path : L"");
		WriteUtf8(file, L"\t");
		WriteUtf8(file, message ? message : L"");
		WriteUtf8(file, L"\r\n");
		CloseHandle(file);
	}
	HeapFree(GetProcessHeap(), 0, path);
	LeaveCriticalSection(&g_logLock);
}

static void QueueUiEvent(DWORD type, const PM_INCIDENT* incident, const WCHAR* message)
{
	EnterCriticalSection(&g_uiLock);
	if (g_uiCount == PM_MAX_UI_EVENTS)
	{
		FreeIncident(&g_uiEvents[g_uiHead].incident);
		ZeroMemory(&g_uiEvents[g_uiHead], sizeof(g_uiEvents[g_uiHead]));
		g_uiHead = (g_uiHead + 1) % PM_MAX_UI_EVENTS;
		--g_uiCount;
	}
	PM_UI_EVENT* event = &g_uiEvents[g_uiTail];
	FreeIncident(&event->incident);
	ZeroMemory(event, sizeof(*event));
	event->type = type;
	if (incident && !CopyIncident(&event->incident, incident))
	{
		ZeroMemory(event, sizeof(*event));
		LeaveCriticalSection(&g_uiLock);
		return;
	}
	CopyText(event->message, ARRAYSIZE(event->message), message);
	g_uiTail = (g_uiTail + 1) % PM_MAX_UI_EVENTS;
	++g_uiCount;
	LeaveCriticalSection(&g_uiLock);
	if (g_mainWindow)
		PostMessageW(g_mainWindow, WM_PROCESS_MONITOR_UI, 0, 0);
}

static BOOL PopUiEvent(PM_UI_EVENT* event)
{
	BOOL result = FALSE;
	EnterCriticalSection(&g_uiLock);
	if (g_uiCount > 0)
	{
		PM_UI_EVENT* queued = &g_uiEvents[g_uiHead];
		FreeIncident(&event->incident);
		*event = *queued;
		queued->incident.path = NULL;
		ZeroMemory(queued, sizeof(*queued));
		g_uiHead = (g_uiHead + 1) % PM_MAX_UI_EVENTS;
		--g_uiCount;
		result = TRUE;
	}
	LeaveCriticalSection(&g_uiLock);
	return result;
}

static void QueueAction(const PM_INCIDENT* incident)
{
	BOOL queued = FALSE;
	EnterCriticalSection(&g_actionLock);
	if (g_actionCount < PM_MAX_ACTIONS)
	{
		queued = CopyIncident(&g_actions[g_actionTail], incident);
		if (queued)
		{
			g_actionTail = (g_actionTail + 1) % PM_MAX_ACTIONS;
			++g_actionCount;
			SetEvent(g_actionEvent);
		}
	}
	LeaveCriticalSection(&g_actionLock);
	if (!queued)
	{
		QueueUiEvent(PM_UI_RESULT, incident, L"终止任务队列已满或内存不足，本次未执行。请检查规则数量。");
		Audit(L"terminate-queue-full", incident, L"action queue full or allocation failed");
	}
}

static BOOL PopAction(PM_INCIDENT* incident)
{
	BOOL result = FALSE;
	EnterCriticalSection(&g_actionLock);
	if (g_actionCount > 0)
	{
		MoveIncident(incident, &g_actions[g_actionHead]);
		g_actionHead = (g_actionHead + 1) % PM_MAX_ACTIONS;
		--g_actionCount;
		result = TRUE;
	}
	if (g_actionCount == 0)
		ResetEvent(g_actionEvent);
	LeaveCriticalSection(&g_actionLock);
	return result;
}

static BOOL ConfigurationHasWork(const PM_CONFIG* config)
{
	if (!config || !config->enabled)
		return FALSE;
	for (DWORD index = 0; index < config->count; ++index)
		if (config->rules[index].enabled)
			return TRUE;
	return FALSE;
}

static BOOL CopyWorkerConfiguration()
{
	if (!g_workerConfig)
		return FALSE;
	EnterCriticalSection(&g_configLock);
	PM_CONFIG* copy = CloneConfiguration(&g_config);
	LeaveCriticalSection(&g_configLock);
	if (copy)
	{
		MoveConfiguration(g_workerConfig, copy);
		HeapFree(GetProcessHeap(), 0, copy);
		ZeroMemory(g_states, sizeof(g_states));
		g_sampleCycle = 0;
		return TRUE;
	}
	FreeConfiguration(g_workerConfig);
	return FALSE;
}

static BOOL RuleStillCurrent(const PM_INCIDENT* incident)
{
	if (!incident || !incident->path)
		return FALSE;
	BOOL found = FALSE;
	EnterCriticalSection(&g_configLock);
	if (g_config.enabled && g_config.generation == incident->generation)
	{
		for (DWORD index = 0; index < g_config.count; ++index)
		{
			PM_RULE* rule = &g_config.rules[index];
			if (rule->enabled && GuidEqual(&rule->id, &incident->ruleId) &&
				lstrcmpiW(rule->path, incident->path) == 0)
			{
				found = TRUE;
				break;
			}
		}
	}
	LeaveCriticalSection(&g_configLock);
	return found;
}

static BOOL IncidentStillCurrentForUi(const PM_INCIDENT* incident)
{
	if (!incident)
		return FALSE;
	if (incident->pid == 1234 && incident->createTime == 0)
		return TRUE;
	return RuleStillCurrent(incident);
}

static BOOL QueryProcessIdentity(HANDLE process, DWORD expectedPid, ULONGLONG expectedCreateTime,
	const WCHAR* expectedPath, WCHAR* actualPath, DWORD actualPathCount)
{
	FILETIME createTime, exitTime, kernelTime, userTime;
	if (!process || !GetProcessTimes(process, &createTime, &exitTime, &kernelTime, &userTime))
		return FALSE;
	if (expectedCreateTime && FileTimeToUInt64(&createTime) != expectedCreateTime)
		return FALSE;
	if (expectedPid && GetProcessId(process) != expectedPid)
		return FALSE;
	WCHAR* path = actualPath;
	BOOL allocated = FALSE;
	if (!path)
	{
		path = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, PM_MAX_PATH_CHARS * sizeof(WCHAR));
		actualPathCount = PM_MAX_PATH_CHARS;
		allocated = TRUE;
	}
	BOOL result = path && QueryProcessPath(process, path, actualPathCount) &&
		(!expectedPath || lstrcmpiW(path, expectedPath) == 0);
	if (allocated)
		HeapFree(GetProcessHeap(), 0, path);
	return result;
}

static BOOL IsForbiddenName(const WCHAR* name)
{
	const WCHAR* forbidden[] = {
		L"system", L"system idle process", L"registry", L"smss.exe", L"csrss.exe", L"wininit.exe",
		L"services.exe", L"lsass.exe", L"winlogon.exe", L"svchost.exe", L"fontdrvhost.exe",
		L"dwm.exe", L"secure system"
	};
	for (DWORD index = 0; index < ARRAYSIZE(forbidden); ++index)
		if (lstrcmpiW(name, forbidden[index]) == 0)
			return TRUE;
	return FALSE;
}

static BOOL IsSafeTerminationTarget(HANDLE process, const PM_INCIDENT* incident, WCHAR* reason, DWORD reasonCount)
{
	if (!process || !incident || incident->pid == 0 || incident->pid == 4 || incident->pid == GetCurrentProcessId())
	{
		CopyText(reason, reasonCount, L"目标是 TrayS 或核心系统进程");
		return FALSE;
	}
	if (!incident->path || !incident->path[0])
	{
		CopyText(reason, reasonCount, L"目标路径缺失，无法可靠验证进程身份");
		return FALSE;
	}
	const WCHAR* name = BaseName(incident->path);
	if (IsForbiddenName(name))
	{
		CopyText(reason, reasonCount, L"目标位于关键系统进程保护名单");
		return FALSE;
	}
	if (!QueryProcessIdentity(process, incident->pid, incident->createTime, incident->path, NULL, 0))
	{
		CopyText(reason, reasonCount, L"进程身份已变化或无法可靠验证");
		return FALSE;
	}
	if (!g_isProcessCritical)
	{
		CopyText(reason, reasonCount, L"当前系统无法完成关键进程安全检查");
		return FALSE;
	}
	BOOL critical = FALSE;
	if (!g_isProcessCritical(process, &critical))
	{
		CopyText(reason, reasonCount, L"关键进程安全检查失败");
		return FALSE;
	}
	if (critical)
	{
		CopyText(reason, reasonCount, L"Windows 将目标标记为关键进程");
		return FALSE;
	}
	return TRUE;
}

typedef struct _PM_CLOSE_CONTEXT
{
	DWORD pid;
	DWORD count;
} PM_CLOSE_CONTEXT;

static BOOL CALLBACK CloseWindowForProcess(HWND window, LPARAM parameter)
{
	PM_CLOSE_CONTEXT* context = (PM_CLOSE_CONTEXT*)parameter;
	DWORD pid = 0;
	GetWindowThreadProcessId(window, &pid);
	if (pid == context->pid && GetWindow(window, GW_OWNER) == NULL)
	{
		PostMessageW(window, WM_CLOSE, 0, 0);
		++context->count;
	}
	return TRUE;
}

static void FormatIncidentMetrics(const PM_INCIDENT* incident, WCHAR* text, DWORD textCount)
{
	text[0] = 0;
	WCHAR part[128];
	if (incident->metricMask & PM_METRIC_CPU)
	{
		wsprintfW(part, L"CPU %u%%（阈值 %u%%）", incident->cpuValue, incident->cpuThreshold);
		AppendText(text, textCount, part);
	}
	if (incident->metricMask & PM_METRIC_MEMORY)
	{
		if (text[0]) AppendText(text, textCount, L"，");
		wsprintfW(part, L"内存 %u%%（阈值 %u%%）", incident->memoryValue, incident->memoryThreshold);
		AppendText(text, textCount, part);
	}
	if (incident->metricMask & PM_METRIC_GPU)
	{
		if (text[0]) AppendText(text, textCount, L"，");
		wsprintfW(part, L"GPU %u%%（阈值 %u%%）", incident->gpuValue, incident->gpuThreshold);
		AppendText(text, textCount, part);
	}
}

static void CompleteTermination(const PM_INCIDENT* incident, const WCHAR* outcome, BOOL success)
{
	WCHAR message[768];
	WCHAR metrics[384];
	FormatIncidentMetrics(incident, metrics, ARRAYSIZE(metrics));
	wsprintfW(message, L"%s\r\n%s（PID %u）\r\n触发指标：%s", outcome, BaseName(incident->path), incident->pid, metrics);
	QueueUiEvent(PM_UI_RESULT, incident, message);
	Audit(success ? L"terminate-success" : L"terminate-failed", incident, outcome);
}

static void ExecuteTermination(const PM_INCIDENT* incident)
{
	if (!RuleStillCurrent(incident))
	{
		CompleteTermination(incident, L"规则已改变或停用，本次终止已取消。", FALSE);
		return;
	}
	HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, incident->pid);
	if (!process)
	{
		DWORD error = GetLastError();
		WCHAR message[256];
		wsprintfW(message, L"无法打开目标进程，终止未执行（错误 %u）。", error);
		CompleteTermination(incident, message, FALSE);
		return;
	}
	WCHAR* reason = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 384 * sizeof(WCHAR));
	if (!reason)
	{
		CloseHandle(process);
		CompleteTermination(incident, L"内存不足，终止未执行。", FALSE);
		return;
	}
	if (!IsSafeTerminationTarget(process, incident, reason, 384))
	{
		CloseHandle(process);
		WCHAR* message = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 512 * sizeof(WCHAR));
		if (message)
		{
			wsprintfW(message, L"安全检查拒绝终止：%s。", reason);
			CompleteTermination(incident, message, FALSE);
			HeapFree(GetProcessHeap(), 0, message);
		}
		else
			CompleteTermination(incident, L"安全检查拒绝终止。", FALSE);
		HeapFree(GetProcessHeap(), 0, reason);
		return;
	}
	PM_CLOSE_CONTEXT closeContext = { incident->pid, 0 };
	EnumWindows(CloseWindowForProcess, (LPARAM)&closeContext);
	HANDLE waitHandles[2] = { g_stopEvent, process };
	DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, 5000);
	if (waitResult == WAIT_OBJECT_0)
	{
		CloseHandle(process);
		HeapFree(GetProcessHeap(), 0, reason);
		return;
	}
	if (waitResult == WAIT_OBJECT_0 + 1)
	{
		CloseHandle(process);
		CompleteTermination(incident, closeContext.count ? L"进程已响应正常关闭请求并退出。" : L"进程已在宽限期内自行退出。", TRUE);
		HeapFree(GetProcessHeap(), 0, reason);
		return;
	}
	CloseHandle(process);
	process = NULL;
	if (WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0)
	{
		HeapFree(GetProcessHeap(), 0, reason);
		return;
	}
	process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, incident->pid);
	if (!process || !RuleStillCurrent(incident) || !IsSafeTerminationTarget(process, incident, reason, 384))
	{
		if (process)
			CloseHandle(process);
		CompleteTermination(incident, L"强制终止前身份或规则校验失败，本次操作已取消。", FALSE);
		HeapFree(GetProcessHeap(), 0, reason);
		return;
	}
	if (TerminateProcess(process, 1))
	{
		WaitForSingleObject(process, 1000);
		CloseHandle(process);
		CompleteTermination(incident, closeContext.count ? L"进程未响应正常关闭，已执行强制终止。" : L"进程没有可关闭窗口，已执行强制终止。", TRUE);
	}
	else
	{
		DWORD error = GetLastError();
		CloseHandle(process);
		WCHAR message[256];
		wsprintfW(message, L"强制终止失败（错误 %u）。", error);
		CompleteTermination(incident, message, FALSE);
	}
	HeapFree(GetProcessHeap(), 0, reason);
}

static DWORD WINAPI ActionThreadProc(LPVOID)
{
	HANDLE events[2] = { g_stopEvent, g_actionEvent };
	PM_INCIDENT incident;
	ZeroMemory(&incident, sizeof(incident));
	for (;;)
	{
		DWORD waitResult = WaitForMultipleObjects(2, events, FALSE, INFINITE);
		if (waitResult == WAIT_OBJECT_0)
			break;
		if (waitResult == WAIT_OBJECT_0 + 1)
		{
			while (PopAction(&incident))
			{
				if (WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0)
					break;
				ExecuteTermination(&incident);
				FreeIncident(&incident);
			}
		}
	}
	FreeIncident(&incident);
	return 0;
}

static BOOL EnsureMemoryApi()
{
	if (g_getProcessMemoryInfo)
		return TRUE;
	HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
	if (kernel)
		g_getProcessMemoryInfo = (PM_GET_PROCESS_MEMORY_INFO)GetProcAddress(kernel, "K32GetProcessMemoryInfo");
	if (!g_psapiModule)
		g_psapiModule = LoadLibraryW(L"psapi.dll");
	if (!g_getProcessMemoryInfo && g_psapiModule)
	{
		g_getProcessMemoryInfo = (PM_GET_PROCESS_MEMORY_INFO)GetProcAddress(g_psapiModule, "GetProcessMemoryInfo");
	}
	return g_getProcessMemoryInfo != NULL;
}

static void CloseGpuQuery()
{
	if (g_gpuQuery && g_pdhCloseQuery)
		g_pdhCloseQuery(g_gpuQuery);
	g_gpuQuery = NULL;
	g_gpuCounter = NULL;
	g_gpuHasBaseline = FALSE;
	g_gpuAvailable = FALSE;
	g_gpuUsageCount = 0;
}

static BOOL RecordGpuFailure()
{
	if (g_gpuFailureCount < 60)
		++g_gpuFailureCount;
	if (g_gpuFailureCount >= 5)
		CloseGpuQuery();
	return FALSE;
}

static BOOL LoadPdhApi()
{
	if (g_pdhModule)
		return g_pdhOpenQuery && g_pdhAddEnglishCounter && g_pdhCollectQueryData &&
			g_pdhGetFormattedCounterArray && g_pdhCloseQuery;
	if (g_pdhLoadAttempted)
		return FALSE;
	g_pdhLoadAttempted = TRUE;
	g_pdhModule = LoadLibraryW(L"pdh.dll");
	if (!g_pdhModule)
		return FALSE;
	g_pdhOpenQuery = (PM_PDH_OPEN_QUERY)GetProcAddress(g_pdhModule, "PdhOpenQueryW");
	g_pdhAddEnglishCounter = (PM_PDH_ADD_ENGLISH_COUNTER)GetProcAddress(g_pdhModule, "PdhAddEnglishCounterW");
	g_pdhCollectQueryData = (PM_PDH_COLLECT_QUERY_DATA)GetProcAddress(g_pdhModule, "PdhCollectQueryData");
	g_pdhGetFormattedCounterArray = (PM_PDH_GET_FORMATTED_COUNTER_ARRAY)GetProcAddress(g_pdhModule, "PdhGetFormattedCounterArrayW");
	g_pdhCloseQuery = (PM_PDH_CLOSE_QUERY)GetProcAddress(g_pdhModule, "PdhCloseQuery");
	return g_pdhOpenQuery && g_pdhAddEnglishCounter && g_pdhCollectQueryData &&
		g_pdhGetFormattedCounterArray && g_pdhCloseQuery;
}

static BOOL EnsureGpuQuery()
{
	if (g_gpuQuery && g_gpuCounter)
		return TRUE;
	CloseGpuQuery();
	if (!LoadPdhApi())
		return FALSE;
	if (g_pdhOpenQuery(NULL, 0, &g_gpuQuery) != ERROR_SUCCESS)
	{
		CloseGpuQuery();
		return FALSE;
	}
	if (g_pdhAddEnglishCounter(g_gpuQuery, L"\\GPU Engine(*)\\Utilization Percentage", 0, &g_gpuCounter) != ERROR_SUCCESS)
	{
		CloseGpuQuery();
		return FALSE;
	}
	return TRUE;
}

static BOOL ParseGpuPid(const WCHAR* name, DWORD* pid)
{
	if (!name || lstrlenW(name) < 6 || !(name[0] == L'p' || name[0] == L'P') ||
		!(name[1] == L'i' || name[1] == L'I') || !(name[2] == L'd' || name[2] == L'D') || name[3] != L'_')
		return FALSE;
	ULONGLONG value = 0;
	DWORD index = 4;
	BOOL any = FALSE;
	while (name[index] >= L'0' && name[index] <= L'9')
	{
		value = value * 10 + (name[index] - L'0');
		if (value > 0xffffffff)
			return FALSE;
		++index;
		any = TRUE;
	}
	if (!any || name[index] != L'_')
		return FALSE;
	*pid = (DWORD)value;
	return *pid != 0;
}

static void AddGpuUsage(DWORD pid, double value)
{
	if (value != value || value < 0.0)
		return;
	if (value > 100.0)
		value = 100.0;
	for (DWORD index = 0; index < g_gpuUsageCount; ++index)
	{
		if (g_gpuUsage[index].pid == pid)
		{
			if (value > g_gpuUsage[index].value)
				g_gpuUsage[index].value = value;
			return;
		}
	}
	if (g_gpuUsageCount < PM_MAX_GPU_PIDS)
	{
		g_gpuUsage[g_gpuUsageCount].pid = pid;
		g_gpuUsage[g_gpuUsageCount].value = value;
		++g_gpuUsageCount;
	}
}

static BOOL CollectGpuUsage(BOOL needed)
{
	g_gpuUsageCount = 0;
	g_gpuAvailable = FALSE;
	if (!needed)
	{
		CloseGpuQuery();
		g_gpuFailureCount = 0;
		return FALSE;
	}
	if (!EnsureGpuQuery())
		return RecordGpuFailure();
	PDH_STATUS status = g_pdhCollectQueryData(g_gpuQuery);
	if (status != ERROR_SUCCESS)
		return RecordGpuFailure();
	if (!g_gpuHasBaseline)
	{
		g_gpuHasBaseline = TRUE;
		return FALSE;
	}
	DWORD bufferSize = 0;
	DWORD itemCount = 0;
	status = g_pdhGetFormattedCounterArray(g_gpuCounter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, NULL);
	if (status == PDH_CSTATUS_NO_INSTANCE || status == PDH_NO_DATA)
	{
		g_gpuAvailable = TRUE;
		g_gpuFailureCount = 0;
		return TRUE;
	}
	if (status != PDH_MORE_DATA || bufferSize == 0 || bufferSize > 16 * 1024 * 1024)
		return RecordGpuFailure();
	PPDH_FMT_COUNTERVALUE_ITEM_W items = (PPDH_FMT_COUNTERVALUE_ITEM_W)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bufferSize);
	if (!items)
		return RecordGpuFailure();
	status = g_pdhGetFormattedCounterArray(g_gpuCounter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, items);
	if (status == ERROR_SUCCESS)
	{
		for (DWORD index = 0; index < itemCount; ++index)
		{
			if ((items[index].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || items[index].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA))
			{
				DWORD pid = 0;
				if (ParseGpuPid(items[index].szName, &pid))
					AddGpuUsage(pid, items[index].FmtValue.doubleValue);
			}
		}
		g_gpuAvailable = TRUE;
		g_gpuFailureCount = 0;
	}
	else
		RecordGpuFailure();
	HeapFree(GetProcessHeap(), 0, items);
	return g_gpuAvailable;
}

static BOOL GetGpuValue(DWORD pid, DWORD* value)
{
	if (!g_gpuAvailable)
		return FALSE;
	for (DWORD index = 0; index < g_gpuUsageCount; ++index)
	{
		if (g_gpuUsage[index].pid == pid)
		{
			double rounded = g_gpuUsage[index].value + 0.5;
			*value = rounded > 100.0 ? 100 : (DWORD)rounded;
			return TRUE;
		}
	}
	*value = 0;
	return TRUE;
}

static PM_INSTANCE_STATE* FindState(DWORD ruleIndex, DWORD pid, ULONGLONG createTime)
{
	PM_INSTANCE_STATE* freeState = NULL;
	for (DWORD index = 0; index < PM_MAX_INSTANCE_STATES; ++index)
	{
		PM_INSTANCE_STATE* state = &g_states[index];
		if (state->used && state->ruleIndex == ruleIndex && state->pid == pid && state->createTime == createTime)
			return state;
		if (!state->used && !freeState)
			freeState = state;
	}
	if (!freeState)
	{
		DWORD oldestCycle = 0xffffffff;
		for (DWORD index = 0; index < PM_MAX_INSTANCE_STATES; ++index)
		{
			if (g_states[index].seenCycle < oldestCycle)
			{
				oldestCycle = g_states[index].seenCycle;
				freeState = &g_states[index];
			}
		}
	}
	if (freeState)
	{
		if (freeState->used && freeState->latched && g_workerConfig && freeState->ruleIndex < g_workerConfig->count)
		{
			PM_INCIDENT incident;
			ZeroMemory(&incident, sizeof(incident));
			incident.ruleId = g_workerConfig->rules[freeState->ruleIndex].id;
			incident.pid = freeState->pid;
			incident.createTime = freeState->createTime;
			QueueUiEvent(PM_UI_CLEAR, &incident, NULL);
		}
		ZeroMemory(freeState, sizeof(*freeState));
		freeState->used = TRUE;
		freeState->ruleIndex = ruleIndex;
		freeState->pid = pid;
		freeState->createTime = createTime;
	}
	return freeState;
}

static BOOL QueryPrivateWorkingSet(HANDLE process, ULONGLONG totalPhysical, DWORD* value)
{
	if (!EnsureMemoryApi() || totalPhysical == 0)
		return FALSE;
	// EX2 is only defined by newer Windows SDK contracts; query by size and fall back for older systems.
#if defined(NTDDI_WIN10_VB) && NTDDI_VERSION >= NTDDI_WIN10_VB
	PROCESS_MEMORY_COUNTERS_EX2 counters;
	ZeroMemory(&counters, sizeof(counters));
	counters.cb = sizeof(counters);
	ULONGLONG privateBytes = 0;
	if (!g_getProcessMemoryInfo(process, (PPROCESS_MEMORY_COUNTERS)&counters, sizeof(counters)))
	{
		PROCESS_MEMORY_COUNTERS_EX legacy;
		ZeroMemory(&legacy, sizeof(legacy));
		legacy.cb = sizeof(legacy);
		if (!g_getProcessMemoryInfo(process, (PPROCESS_MEMORY_COUNTERS)&legacy, sizeof(legacy)))
			return FALSE;
		// Older Windows versions do not expose PrivateWorkingSetSize; PrivateUsage is the safest available private-memory fallback.
		privateBytes = legacy.PrivateUsage;
	}
	else
		privateBytes = counters.PrivateWorkingSetSize;
#else
	PROCESS_MEMORY_COUNTERS_EX legacy;
	ZeroMemory(&legacy, sizeof(legacy));
	legacy.cb = sizeof(legacy);
	if (!g_getProcessMemoryInfo(process, (PPROCESS_MEMORY_COUNTERS)&legacy, sizeof(legacy)))
		return FALSE;
	ULONGLONG privateBytes = legacy.PrivateUsage;
#endif
	ULONGLONG percentage = privateBytes > MAXULONGLONG / 100 ? 100 :
		(privateBytes * 100 + totalPhysical / 2) / totalPhysical;
	*value = percentage > 100 ? 100 : (DWORD)percentage;
	return TRUE;
}

static BOOL QueryCpuUsage(HANDLE process, PM_INSTANCE_STATE* state, DWORD logicalProcessors,
	ULONGLONG tick, BOOL sampleGap, DWORD* value)
{
	FILETIME createTime, exitTime, kernelTime, userTime;
	if (!GetProcessTimes(process, &createTime, &exitTime, &kernelTime, &userTime))
		return FALSE;
	ULONGLONG cpuTime = FileTimeToUInt64(&kernelTime) + FileTimeToUInt64(&userTime);
	if (sampleGap || !state->cpuBaseline || tick <= state->previousTick || cpuTime < state->previousCpuTime)
	{
		state->previousCpuTime = cpuTime;
		state->previousTick = tick;
		state->cpuBaseline = TRUE;
		return FALSE;
	}
	ULONGLONG elapsed100ns = (tick - state->previousTick) * 10000;
	ULONGLONG denominator = elapsed100ns * (logicalProcessors ? logicalProcessors : 1);
	ULONGLONG percentage = denominator ? ((cpuTime - state->previousCpuTime) * 100 + denominator / 2) / denominator : 0;
	state->previousCpuTime = cpuTime;
	state->previousTick = tick;
	*value = percentage > 100 ? 100 : (DWORD)percentage;
	return TRUE;
}

static DWORD MetricIndex(DWORD metric)
{
	return metric == PM_METRIC_CPU ? 0 : metric == PM_METRIC_MEMORY ? 1 : 2;
}

static DWORD MetricThreshold(const PM_RULE* rule, DWORD metric)
{
	return metric == PM_METRIC_CPU ? rule->cpuThreshold : metric == PM_METRIC_MEMORY ? rule->memoryThreshold : rule->gpuThreshold;
}

static DWORD MetricValue(DWORD metric, DWORD cpu, DWORD memory, DWORD gpu)
{
	return metric == PM_METRIC_CPU ? cpu : metric == PM_METRIC_MEMORY ? memory : gpu;
}

static void CreateIncident(const PM_RULE* rule, PM_INSTANCE_STATE* state, DWORD triggeredMask,
	DWORD cpu, DWORD memory, DWORD gpu)
{
	PM_INCIDENT* incident = (PM_INCIDENT*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PM_INCIDENT));
	if (!incident)
		return;
	incident->ruleId = rule->id;
	incident->generation = g_workerConfig->generation;
	incident->pid = state->pid;
	incident->createTime = state->createTime;
	incident->metricMask = triggeredMask;
	incident->cpuValue = cpu;
	incident->memoryValue = memory;
	incident->gpuValue = gpu;
	incident->cpuThreshold = rule->cpuThreshold;
	incident->memoryThreshold = rule->memoryThreshold;
	incident->gpuThreshold = rule->gpuThreshold;
	incident->holdSeconds = rule->holdSeconds;
	incident->action = rule->action;
	incident->path = DuplicateText(rule->path);
	if (!incident->path)
	{
		HeapFree(GetProcessHeap(), 0, incident);
		return;
	}
	WCHAR metrics[384];
	FormatIncidentMetrics(incident, metrics, ARRAYSIZE(metrics));
	Audit(L"threshold-triggered", incident, metrics);
	if (rule->action == PM_ACTION_AUTO_TERMINATE)
		QueueAction(incident);
	else
		QueueUiEvent(PM_UI_ALERT, incident, NULL);
	FreeIncident(incident);
	HeapFree(GetProcessHeap(), 0, incident);
}

static void UpdateState(const PM_RULE* rule, PM_INSTANCE_STATE* state,
	ULONGLONG tick, BOOL cpuValid, DWORD cpu, BOOL memoryValid, DWORD memory, BOOL gpuValid, DWORD gpu)
{
	DWORD metrics[] = { PM_METRIC_CPU, PM_METRIC_MEMORY, PM_METRIC_GPU };
	BOOL valid[] = { cpuValid, memoryValid, gpuValid };
	DWORD triggeredMask = 0;
	for (DWORD item = 0; item < ARRAYSIZE(metrics); ++item)
	{
		DWORD metric = metrics[item];
		DWORD index = MetricIndex(metric);
		if ((rule->metricMask & metric) == 0 || !valid[item])
		{
			state->overSince[index] = 0;
			continue;
		}
		DWORD value = MetricValue(metric, cpu, memory, gpu);
		if (value >= MetricThreshold(rule, metric))
		{
			if (!state->overSince[index])
				state->overSince[index] = tick + 1;
			ULONGLONG startTick = state->overSince[index] - 1;
			if (tick >= startTick && tick - startTick >= (ULONGLONG)rule->holdSeconds * 1000)
				triggeredMask |= metric;
		}
		else
			state->overSince[index] = 0;
	}
	if (!state->latched && triggeredMask)
	{
		DWORD overLimitMask = 0;
		for (DWORD item = 0; item < ARRAYSIZE(metrics); ++item)
		{
			DWORD metric = metrics[item];
			if ((rule->metricMask & metric) && valid[item] &&
				MetricValue(metric, cpu, memory, gpu) >= MetricThreshold(rule, metric))
				overLimitMask |= metric;
		}
		state->latched = TRUE;
		state->triggeredMask = overLimitMask;
		state->recoveryCount = 0;
		CreateIncident(rule, state, overLimitMask, cpu, memory, gpu);
	}
	if (state->latched)
	{
		state->triggeredMask |= triggeredMask;
		BOOL recovered = TRUE;
		for (DWORD item = 0; item < ARRAYSIZE(metrics); ++item)
		{
			DWORD metric = metrics[item];
			if ((state->triggeredMask & metric) == 0)
				continue;
			if (!valid[item])
			{
				recovered = FALSE;
				break;
			}
			DWORD threshold = MetricThreshold(rule, metric);
			DWORD recoveryThreshold = threshold > 5 ? threshold - 5 : 0;
			DWORD currentValue = MetricValue(metric, cpu, memory, gpu);
			if (currentValue > recoveryThreshold)
			{
				recovered = FALSE;
				break;
			}
		}
		if (recovered)
			++state->recoveryCount;
		else
			state->recoveryCount = 0;
		if (state->recoveryCount >= 3)
		{
			PM_INCIDENT incident;
			ZeroMemory(&incident, sizeof(incident));
			incident.ruleId = rule->id;
			incident.pid = state->pid;
			incident.createTime = state->createTime;
			QueueUiEvent(PM_UI_CLEAR, &incident, NULL);
			state->latched = FALSE;
			state->triggeredMask = 0;
			state->recoveryCount = 0;
			ZeroMemory(state->overSince, sizeof(state->overSince));
		}
	}
}

static BOOL WorkerNeedsGpu(const PM_CONFIG* config)
{
	if (!config->enabled)
		return FALSE;
	for (DWORD index = 0; index < config->count; ++index)
		if (config->rules[index].enabled && (config->rules[index].metricMask & PM_METRIC_GPU))
			return TRUE;
	return FALSE;
}

static DWORD CountLogicalProcessors()
{
	DWORD length = 0;
	GetLogicalProcessorInformationEx(RelationGroup, NULL, &length);
	if (!length)
	{
		SYSTEM_INFO info;
		GetSystemInfo(&info);
		return info.dwNumberOfProcessors ? info.dwNumberOfProcessors : 1;
	}
	BYTE* buffer = (BYTE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, length);
	if (!buffer)
		return 1;
	DWORD count = 0;
	if (GetLogicalProcessorInformationEx(RelationGroup, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buffer, &length))
	{
		DWORD offset = 0;
		while (offset < length)
		{
			PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX entry = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buffer + offset);
			if (entry->Relationship == RelationGroup)
			{
				for (WORD group = 0; group < entry->Group.ActiveGroupCount; ++group)
					count += entry->Group.GroupInfo[group].ActiveProcessorCount;
			}
			if (!entry->Size)
				break;
			offset += entry->Size;
		}
	}
	HeapFree(GetProcessHeap(), 0, buffer);
	return count ? count : 1;
}

static void PublishRuntime(const PM_CONFIG* config)
{
	EnterCriticalSection(&g_runtimeLock);
	g_runtimeCount = config->count;
	for (DWORD index = 0; index < config->count; ++index)
	{
		g_runtime[index].ruleId = config->rules[index].id;
		g_runtime[index].runningInstances = 0;
		g_runtime[index].gpuRequested = (config->rules[index].metricMask & PM_METRIC_GPU) != 0;
		g_runtime[index].gpuAvailable = !g_runtime[index].gpuRequested || g_gpuAvailable;
	}
	for (DWORD index = 0; index < PM_MAX_INSTANCE_STATES; ++index)
	{
		PM_INSTANCE_STATE* state = &g_states[index];
		if (state->used && state->seenCycle == g_sampleCycle && state->ruleIndex < g_runtimeCount)
			++g_runtime[state->ruleIndex].runningInstances;
	}
	LeaveCriticalSection(&g_runtimeLock);
	if (g_rulesWindow)
		PostMessageW(g_rulesWindow, WM_PROCESS_MONITOR_UI, 0, 0);
}

static void SampleProcesses(const PM_CONFIG* config, DWORD logicalProcessors, ULONGLONG totalPhysical,
	ULONGLONG tick, BOOL sampleGap)
{
	++g_sampleCycle;
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE)
	{
		PublishRuntime(config);
		return;
	}
	PROCESSENTRY32W entry;
	ZeroMemory(&entry, sizeof(entry));
	entry.dwSize = sizeof(entry);
	BOOL hasEntry = Process32FirstW(snapshot, &entry);
	WCHAR* processPath = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, PM_MAX_PATH_CHARS * sizeof(WCHAR));
	if (!hasEntry || !processPath)
	{
		HeapFree(GetProcessHeap(), 0, processPath);
		CloseHandle(snapshot);
		if (!hasEntry)
			PublishRuntime(config);
		return;
	}
	while (hasEntry)
	{
		for (DWORD ruleIndex = 0; ruleIndex < config->count; ++ruleIndex)
		{
			const PM_RULE* rule = &config->rules[ruleIndex];
			if (!config->enabled || !rule->enabled || lstrcmpiW(entry.szExeFile, rule->exeName) != 0)
				continue;
			HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
			if (!process)
				continue;
			FILETIME createTime, exitTime, kernelTime, userTime;
			BOOL identityOk = processPath && QueryProcessPath(process, processPath, PM_MAX_PATH_CHARS) &&
				lstrcmpiW(processPath, rule->path) == 0 &&
				GetProcessTimes(process, &createTime, &exitTime, &kernelTime, &userTime);
			if (identityOk)
			{
				ULONGLONG creation = FileTimeToUInt64(&createTime);
				PM_INSTANCE_STATE* state = FindState(ruleIndex, entry.th32ProcessID, creation);
				if (state)
				{
					state->seenCycle = g_sampleCycle;
					if (sampleGap)
					{
						ZeroMemory(state->overSince, sizeof(state->overSince));
						state->recoveryCount = 0;
					}
					DWORD cpu = 0, memory = 0, gpu = 0;
					BOOL cpuValid = (rule->metricMask & PM_METRIC_CPU) && QueryCpuUsage(process, state, logicalProcessors, tick, sampleGap, &cpu);
					BOOL memoryValid = FALSE;
					if (rule->metricMask & PM_METRIC_MEMORY)
						memoryValid = QueryPrivateWorkingSet(process, totalPhysical, &memory);
					BOOL gpuValid = (rule->metricMask & PM_METRIC_GPU) && GetGpuValue(entry.th32ProcessID, &gpu);
					UpdateState(rule, state, tick, cpuValid, cpu, memoryValid, memory, gpuValid, gpu);
				}
			}
			CloseHandle(process);
		}
		hasEntry = Process32NextW(snapshot, &entry);
	}
	HeapFree(GetProcessHeap(), 0, processPath);
	CloseHandle(snapshot);
	for (DWORD index = 0; index < PM_MAX_INSTANCE_STATES; ++index)
	{
		if (g_states[index].used && g_states[index].seenCycle != g_sampleCycle)
		{
			if (g_states[index].latched)
			{
				PM_INCIDENT incident;
				ZeroMemory(&incident, sizeof(incident));
				incident.ruleId = config->rules[g_states[index].ruleIndex].id;
				incident.pid = g_states[index].pid;
				incident.createTime = g_states[index].createTime;
				QueueUiEvent(PM_UI_CLEAR, &incident, NULL);
			}
			ZeroMemory(&g_states[index], sizeof(g_states[index]));
		}
	}
	PublishRuntime(config);
}

static DWORD WINAPI MonitorThreadProc(LPVOID)
{
	if (!g_workerConfig)
		return 0;
	DWORD logicalProcessors = CountLogicalProcessors();
	MEMORYSTATUSEX memoryStatus;
	ZeroMemory(&memoryStatus, sizeof(memoryStatus));
	memoryStatus.dwLength = sizeof(memoryStatus);
	GlobalMemoryStatusEx(&memoryStatus);
	ULONGLONG previousTick = 0;
	HANDLE events[2] = { g_stopEvent, g_configChangedEvent };
	for (;;)
	{
		if (!ConfigurationHasWork(g_workerConfig))
		{
			CloseGpuQuery();
			PublishRuntime(g_workerConfig);
			DWORD waitResult = WaitForMultipleObjects(2, events, FALSE, INFINITE);
			if (waitResult == WAIT_OBJECT_0)
				break;
			ResetEvent(g_configChangedEvent);
			if (!CopyWorkerConfiguration())
			{
				if (WaitForSingleObject(g_stopEvent, 1000) == WAIT_OBJECT_0)
					break;
				SetEvent(g_configChangedEvent);
			}
			previousTick = 0;
			continue;
		}
		ULONGLONG tick = GetTickCount64();
		BOOL sampleGap = previousTick == 0 || tick <= previousTick || tick - previousTick > 2500;
		BOOL needsGpu = WorkerNeedsGpu(g_workerConfig);
		CollectGpuUsage(needsGpu);
		SampleProcesses(g_workerConfig, logicalProcessors, memoryStatus.ullTotalPhys, tick, sampleGap);
		previousTick = tick;
		DWORD elapsed = (DWORD)(GetTickCount64() - tick);
		DWORD timeout = elapsed < 1000 ? 1000 - elapsed : 0;
		DWORD waitResult = WaitForMultipleObjects(2, events, FALSE, timeout);
		if (waitResult == WAIT_OBJECT_0)
			break;
		if (waitResult == WAIT_OBJECT_0 + 1)
		{
			ResetEvent(g_configChangedEvent);
			if (!CopyWorkerConfiguration())
			{
				if (WaitForSingleObject(g_stopEvent, 1000) == WAIT_OBJECT_0)
					break;
				SetEvent(g_configChangedEvent);
			}
			previousTick = 0;
		}
	}
	CloseGpuQuery();
	return 0;
}

static int SelectedListIndex(HWND list)
{
	return ListView_GetNextItem(list, -1, LVNI_SELECTED);
}

static void ConfigureListColumn(HWND list, int column, int width, const WCHAR* title)
{
	LVCOLUMNW item;
	ZeroMemory(&item, sizeof(item));
	item.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
	item.pszText = (LPWSTR)title;
	item.cx = width;
	item.iSubItem = column;
	ListView_InsertColumn(list, column, &item);
}

static void SetListText(HWND list, int row, int column, const WCHAR* text)
{
	ListView_SetItemText(list, row, column, (LPWSTR)text);
}

static void GetRuntimeStatus(DWORD ruleIndex, WCHAR* status, DWORD statusCount)
{
	status[0] = 0;
	EnterCriticalSection(&g_runtimeLock);
	if (ruleIndex < g_runtimeCount && GuidEqual(&g_runtime[ruleIndex].ruleId, &g_config.rules[ruleIndex].id))
	{
		PM_RULE_RUNTIME runtime = g_runtime[ruleIndex];
		if (runtime.gpuRequested && !runtime.gpuAvailable)
			CopyText(status, statusCount, runtime.runningInstances ? L"运行中 / GPU 不可用" : L"未运行 / GPU 不可用");
		else if (runtime.runningInstances)
			wsprintfW(status, L"运行中（%u 个实例）", runtime.runningInstances);
		else
			CopyText(status, statusCount, L"未运行");
	}
	LeaveCriticalSection(&g_runtimeLock);
	if (!status[0])
		CopyText(status, statusCount, L"等待采样");
}

static void RefreshRulesList(HWND dialog)
{
	HWND list = GetDlgItem(dialog, IDC_PM_RULE_LIST);
	int selected = SelectedListIndex(list);
	ListView_DeleteAllItems(list);
	EnterCriticalSection(&g_configLock);
	CheckDlgButton(dialog, IDC_PM_GLOBAL_ENABLE, g_config.enabled ? BST_CHECKED : BST_UNCHECKED);
	for (DWORD index = 0; index < g_config.count; ++index)
	{
		PM_RULE* rule = &g_config.rules[index];
		LVITEMW item;
		ZeroMemory(&item, sizeof(item));
		item.mask = LVIF_TEXT | LVIF_PARAM;
		item.iItem = index;
		item.pszText = rule->enabled ? (LPWSTR)L"是" : (LPWSTR)L"否";
		item.lParam = index;
		int row = ListView_InsertItem(list, &item);
		SetListText(list, row, 1, BaseName(rule->path));
		WCHAR value[64];
		if (rule->metricMask & PM_METRIC_CPU) wsprintfW(value, L"%u%%", rule->cpuThreshold); else CopyText(value, ARRAYSIZE(value), L"—");
		SetListText(list, row, 2, value);
		if (rule->metricMask & PM_METRIC_MEMORY) wsprintfW(value, L"%u%%", rule->memoryThreshold); else CopyText(value, ARRAYSIZE(value), L"—");
		SetListText(list, row, 3, value);
		if (rule->metricMask & PM_METRIC_GPU) wsprintfW(value, L"%u%%", rule->gpuThreshold); else CopyText(value, ARRAYSIZE(value), L"—");
		SetListText(list, row, 4, value);
		wsprintfW(value, L"%u 秒", rule->holdSeconds); SetListText(list, row, 5, value);
		SetListText(list, row, 6, rule->action == PM_ACTION_AUTO_TERMINATE ? L"自动终止" : L"提示选择");
		GetRuntimeStatus(index, value, ARRAYSIZE(value)); SetListText(list, row, 7, value);
	}
	WCHAR status[256];
	CopyText(status, ARRAYSIZE(status), g_configStatus);
	if (!status[0])
		wsprintfW(status, L"共 %u 条规则；每秒采样，达到阈值并持续指定时间后触发。", g_config.count);
	LeaveCriticalSection(&g_configLock);
	SetDlgItemTextW(dialog, IDC_PM_STATUS, status);
	if (selected >= 0 && selected < ListView_GetItemCount(list))
		ListView_SetItemState(list, selected, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

static BOOL IsDuplicateRulePath(const WCHAR* path, int excludingIndex)
{
	BOOL duplicate = FALSE;
	EnterCriticalSection(&g_configLock);
	for (DWORD index = 0; index < g_config.count; ++index)
	{
		if ((int)index != excludingIndex && lstrcmpiW(g_config.rules[index].path, path) == 0)
		{
			duplicate = TRUE;
			break;
		}
	}
	LeaveCriticalSection(&g_configLock);
	return duplicate;
}

static BOOL InitializeRuleDefaults(PM_RULE* rule)
{
	ZeroMemory(rule, sizeof(*rule));
	if (FAILED(CoCreateGuid(&rule->id)))
		return FALSE;
	rule->enabled = TRUE;
	rule->metricMask = PM_METRIC_CPU | PM_METRIC_MEMORY | PM_METRIC_GPU;
	rule->cpuThreshold = 70;
	rule->memoryThreshold = 70;
	rule->gpuThreshold = 70;
	rule->holdSeconds = 5;
	rule->action = PM_ACTION_PROMPT;
	return TRUE;
}

static BOOL BrowseForExecutable(HWND owner, WCHAR* path, DWORD pathCount)
{
	OPENFILENAMEW file;
	ZeroMemory(&file, sizeof(file));
	file.lStructSize = sizeof(file);
	file.hwndOwner = owner;
	file.lpstrFilter = L"可执行文件 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0\0";
	file.lpstrFile = path;
	file.nMaxFile = pathCount;
	file.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
	return GetOpenFileNameW(&file);
}

static void EnableThresholdControls(HWND dialog)
{
	EnableWindow(GetDlgItem(dialog, IDC_PM_CPU_THRESHOLD), IsDlgButtonChecked(dialog, IDC_PM_CPU_ENABLE) == BST_CHECKED);
	EnableWindow(GetDlgItem(dialog, IDC_PM_MEMORY_THRESHOLD), IsDlgButtonChecked(dialog, IDC_PM_MEMORY_ENABLE) == BST_CHECKED);
	EnableWindow(GetDlgItem(dialog, IDC_PM_GPU_THRESHOLD), IsDlgButtonChecked(dialog, IDC_PM_GPU_ENABLE) == BST_CHECKED);
}

static BOOL ConfirmAutoTermination(HWND dialog, const WCHAR* path, DWORD cpu, DWORD memory, DWORD gpu,
	DWORD metricMask, DWORD holdSeconds)
{
	WCHAR metrics[256] = L"";
	WCHAR part[64];
	if (metricMask & PM_METRIC_CPU) { wsprintfW(part, L"CPU %u%%", cpu); AppendText(metrics, ARRAYSIZE(metrics), part); }
	if (metricMask & PM_METRIC_MEMORY) { if (metrics[0]) AppendText(metrics, ARRAYSIZE(metrics), L"、"); wsprintfW(part, L"内存 %u%%", memory); AppendText(metrics, ARRAYSIZE(metrics), part); }
	if (metricMask & PM_METRIC_GPU) { if (metrics[0]) AppendText(metrics, ARRAYSIZE(metrics), L"、"); wsprintfW(part, L"GPU %u%%", gpu); AppendText(metrics, ARRAYSIZE(metrics), part); }
	WCHAR holdText[16];
	wsprintfW(holdText, L"%u", holdSeconds);
	SIZE_T textCount = (SIZE_T)lstrlenW(path) + lstrlenW(metrics) + 512;
	WCHAR* text = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, textCount * sizeof(WCHAR));
	if (!text)
		return FALSE;
	AppendText(text, (DWORD)textCount, L"开启后，以下程序任一指标持续超限 ");
	AppendText(text, (DWORD)textCount, holdText);
	AppendText(text, (DWORD)textCount, L" 秒时，TrayS 将不再事前询问：\r\n\r\n");
	AppendText(text, (DWORD)textCount, path);
	AppendText(text, (DWORD)textCount, L"\r\n\r\n");
	AppendText(text, (DWORD)textCount, metrics);
	AppendText(text, (DWORD)textCount, L"\r\n\r\n先请求正常退出；5 秒后仍未退出才强制终止。未保存数据可能丢失。确定开启吗？");
	BOOL confirmed = MessageBoxW(dialog, text, L"确认自动终止", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES;
	HeapFree(GetProcessHeap(), 0, text);
	return confirmed;
}

static BOOL ReadRuleFromDialog(HWND dialog, PM_EDIT_CONTEXT* context)
{
	if (!context || !context->candidate)
		return FALSE;
	WCHAR* rawPath = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, PM_MAX_PATH_CHARS * sizeof(WCHAR));
	WCHAR* normalized = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, PM_MAX_PATH_CHARS * sizeof(WCHAR));
	if (!rawPath || !normalized)
	{
		HeapFree(GetProcessHeap(), 0, rawPath);
		HeapFree(GetProcessHeap(), 0, normalized);
		MessageBoxW(dialog, L"内存不足，无法保存规则。", L"进程监控规则", MB_ICONERROR | MB_OK);
		return FALSE;
	}
	GetDlgItemTextW(dialog, IDC_PM_PATH, rawPath, PM_MAX_PATH_CHARS);
	DWORD attributes = INVALID_FILE_ATTRIBUTES;
	if (!NormalizePath(rawPath, normalized, PM_MAX_PATH_CHARS) ||
		(attributes = GetFileAttributesW(normalized)) == INVALID_FILE_ATTRIBUTES ||
		(attributes & FILE_ATTRIBUTE_DIRECTORY) || !HasExeExtension(normalized))
	{
		HeapFree(GetProcessHeap(), 0, rawPath);
		HeapFree(GetProcessHeap(), 0, normalized);
		MessageBoxW(dialog, L"请选择一个现存的 .exe 可执行文件。", L"进程监控规则", MB_ICONWARNING | MB_OK);
		return FALSE;
	}
	if (lstrcmpiW(normalized, g_selfPath) == 0)
	{
		HeapFree(GetProcessHeap(), 0, rawPath);
		HeapFree(GetProcessHeap(), 0, normalized);
		MessageBoxW(dialog, L"不能为 TrayS 自身创建终止规则。", L"进程监控规则", MB_ICONWARNING | MB_OK);
		return FALSE;
	}
	if (IsDuplicateRulePath(normalized, context->editing ? (int)context->index : -1))
	{
		HeapFree(GetProcessHeap(), 0, rawPath);
		HeapFree(GetProcessHeap(), 0, normalized);
		MessageBoxW(dialog, L"同一可执行文件路径只能创建一条规则。", L"进程监控规则", MB_ICONWARNING | MB_OK);
		return FALSE;
	}
	DWORD metricMask = 0;
	if (IsDlgButtonChecked(dialog, IDC_PM_CPU_ENABLE) == BST_CHECKED) metricMask |= PM_METRIC_CPU;
	if (IsDlgButtonChecked(dialog, IDC_PM_MEMORY_ENABLE) == BST_CHECKED) metricMask |= PM_METRIC_MEMORY;
	if (IsDlgButtonChecked(dialog, IDC_PM_GPU_ENABLE) == BST_CHECKED) metricMask |= PM_METRIC_GPU;
	if (!metricMask)
	{
		HeapFree(GetProcessHeap(), 0, rawPath);
		HeapFree(GetProcessHeap(), 0, normalized);
		MessageBoxW(dialog, L"至少启用一个资源指标。", L"进程监控规则", MB_ICONWARNING | MB_OK);
		return FALSE;
	}
	BOOL translated = FALSE;
	DWORD cpuThreshold = GetDlgItemInt(dialog, IDC_PM_CPU_THRESHOLD, &translated, FALSE);
	if (!translated || cpuThreshold < 1 || cpuThreshold > 100) { HeapFree(GetProcessHeap(), 0, rawPath); HeapFree(GetProcessHeap(), 0, normalized); MessageBoxW(dialog, L"CPU 阈值必须为 1–100。", L"进程监控规则", MB_ICONWARNING | MB_OK); return FALSE; }
	DWORD memoryThreshold = GetDlgItemInt(dialog, IDC_PM_MEMORY_THRESHOLD, &translated, FALSE);
	if (!translated || memoryThreshold < 1 || memoryThreshold > 100) { HeapFree(GetProcessHeap(), 0, rawPath); HeapFree(GetProcessHeap(), 0, normalized); MessageBoxW(dialog, L"内存阈值必须为 1–100。", L"进程监控规则", MB_ICONWARNING | MB_OK); return FALSE; }
	DWORD gpuThreshold = GetDlgItemInt(dialog, IDC_PM_GPU_THRESHOLD, &translated, FALSE);
	if (!translated || gpuThreshold < 1 || gpuThreshold > 100) { HeapFree(GetProcessHeap(), 0, rawPath); HeapFree(GetProcessHeap(), 0, normalized); MessageBoxW(dialog, L"GPU 阈值必须为 1–100。", L"进程监控规则", MB_ICONWARNING | MB_OK); return FALSE; }
	DWORD holdSeconds = GetDlgItemInt(dialog, IDC_PM_HOLD_SECONDS, &translated, FALSE);
	if (!translated || holdSeconds < 1 || holdSeconds > 300) { HeapFree(GetProcessHeap(), 0, rawPath); HeapFree(GetProcessHeap(), 0, normalized); MessageBoxW(dialog, L"连续超限时间必须为 1–300 秒。", L"进程监控规则", MB_ICONWARNING | MB_OK); return FALSE; }
	DWORD action = IsDlgButtonChecked(dialog, IDC_PM_ACTION_AUTO) == BST_CHECKED ? PM_ACTION_AUTO_TERMINATE : PM_ACTION_PROMPT;
	BOOL targetChanged = !context->candidate->path || lstrcmpiW(context->candidate->path, normalized) != 0;
	if (action == PM_ACTION_AUTO_TERMINATE &&
		(context->candidate->action != PM_ACTION_AUTO_TERMINATE || targetChanged) &&
		!ConfirmAutoTermination(dialog, normalized, cpuThreshold, memoryThreshold,
			gpuThreshold, metricMask, holdSeconds))
	{
		HeapFree(GetProcessHeap(), 0, rawPath);
		HeapFree(GetProcessHeap(), 0, normalized);
		return FALSE;
	}
	WCHAR* savedPath = DuplicateText(normalized);
	HeapFree(GetProcessHeap(), 0, rawPath);
	HeapFree(GetProcessHeap(), 0, normalized);
	if (!savedPath)
	{
		MessageBoxW(dialog, L"内存不足，无法保存规则。", L"进程监控规则", MB_ICONERROR | MB_OK);
		return FALSE;
	}
	if (context->candidate->path)
		HeapFree(GetProcessHeap(), 0, context->candidate->path);
	context->candidate->path = savedPath;
	context->candidate->metricMask = metricMask;
	context->candidate->cpuThreshold = cpuThreshold;
	context->candidate->memoryThreshold = memoryThreshold;
	context->candidate->gpuThreshold = gpuThreshold;
	context->candidate->holdSeconds = holdSeconds;
	context->candidate->action = action;
	CopyText(context->candidate->exeName, ARRAYSIZE(context->candidate->exeName), BaseName(savedPath));
	return TRUE;
}

static INT_PTR CALLBACK RuleEditDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
	PM_EDIT_CONTEXT* context = (PM_EDIT_CONTEXT*)GetWindowLongPtrW(dialog, DWLP_USER);
	switch (message)
	{
	case WM_INITDIALOG:
		context = (PM_EDIT_CONTEXT*)lParam;
		SetWindowLongPtrW(dialog, DWLP_USER, (LONG_PTR)context);
		SendMessageW(dialog, WM_SETICON, ICON_SMALL, (LPARAM)g_appIcon);
		SetDlgItemTextW(dialog, IDC_PM_PATH, context->candidate->path);
		CheckDlgButton(dialog, IDC_PM_CPU_ENABLE, (context->candidate->metricMask & PM_METRIC_CPU) ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(dialog, IDC_PM_MEMORY_ENABLE, (context->candidate->metricMask & PM_METRIC_MEMORY) ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(dialog, IDC_PM_GPU_ENABLE, (context->candidate->metricMask & PM_METRIC_GPU) ? BST_CHECKED : BST_UNCHECKED);
		SetDlgItemInt(dialog, IDC_PM_CPU_THRESHOLD, context->candidate->cpuThreshold, FALSE);
		SetDlgItemInt(dialog, IDC_PM_MEMORY_THRESHOLD, context->candidate->memoryThreshold, FALSE);
		SetDlgItemInt(dialog, IDC_PM_GPU_THRESHOLD, context->candidate->gpuThreshold, FALSE);
		SetDlgItemInt(dialog, IDC_PM_HOLD_SECONDS, context->candidate->holdSeconds, FALSE);
		CheckRadioButton(dialog, IDC_PM_ACTION_PROMPT, IDC_PM_ACTION_AUTO,
			context->candidate->action == PM_ACTION_AUTO_TERMINATE ? IDC_PM_ACTION_AUTO : IDC_PM_ACTION_PROMPT);
		EnableThresholdControls(dialog);
		return TRUE;
	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDC_PM_CPU_ENABLE:
		case IDC_PM_MEMORY_ENABLE:
		case IDC_PM_GPU_ENABLE:
			EnableThresholdControls(dialog);
			return TRUE;
		case IDC_PM_BROWSE:
		{
			WCHAR* path = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, PM_MAX_PATH_CHARS * sizeof(WCHAR));
			if (path)
			{
				GetDlgItemTextW(dialog, IDC_PM_PATH, path, PM_MAX_PATH_CHARS);
				if (BrowseForExecutable(dialog, path, PM_MAX_PATH_CHARS))
					SetDlgItemTextW(dialog, IDC_PM_PATH, path);
				HeapFree(GetProcessHeap(), 0, path);
			}
			return TRUE;
		}
		case IDC_PM_PICK_PROCESS:
		{
			WCHAR* path = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, PM_MAX_PATH_CHARS * sizeof(WCHAR));
			if (path)
			{
				if (DialogBoxParamW(g_instance, MAKEINTRESOURCEW(IDD_PROCESS_PICKER), dialog, ProcessPickerDialogProc, (LPARAM)path) == IDOK)
					SetDlgItemTextW(dialog, IDC_PM_PATH, path);
				HeapFree(GetProcessHeap(), 0, path);
			}
			return TRUE;
		}
		case IDC_PM_SAVE:
			if (context && ReadRuleFromDialog(dialog, context))
			{
				context->accepted = TRUE;
				EndDialog(dialog, IDOK);
			}
			return TRUE;
		case IDC_PM_CANCEL:
		case IDCANCEL:
			EndDialog(dialog, IDCANCEL);
			return TRUE;
		}
		break;
	}
	return FALSE;
}

static void FreePickerEntries(HWND list)
{
	for (DWORD index = 0; index < g_pickerEntryCount; ++index)
	{
		PM_PICK_ENTRY* pickerEntry = g_pickerEntries[index];
		if (!pickerEntry)
			continue;
#pragma warning(suppress: 6001)
		WCHAR* pickerPath = pickerEntry->path;
		if (pickerPath)
			HeapFree(GetProcessHeap(), 0, pickerPath);
		HeapFree(GetProcessHeap(), 0, pickerEntry);
	}
	ZeroMemory(g_pickerEntries, sizeof(g_pickerEntries));
	g_pickerEntryCount = 0;
	ListView_DeleteAllItems(list);
}

static void RefreshProcessPicker(HWND dialog)
{
	HWND list = GetDlgItem(dialog, IDC_PM_PROCESS_LIST);
	FreePickerEntries(list);
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE)
		return;
	PROCESSENTRY32W processEntry;
	ZeroMemory(&processEntry, sizeof(processEntry));
	processEntry.dwSize = sizeof(processEntry);
	BOOL hasEntry = Process32FirstW(snapshot, &processEntry);
	while (hasEntry)
	{
		if (processEntry.th32ProcessID != GetCurrentProcessId())
		{
			if (g_pickerEntryCount >= ARRAYSIZE(g_pickerEntries))
				break;
			HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processEntry.th32ProcessID);
			if (process)
			{
				DWORD exitCode = 0;
				WCHAR* queryPath = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, PM_MAX_PATH_CHARS * sizeof(WCHAR));
				if (queryPath && GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE &&
					QueryProcessPath(process, queryPath, PM_MAX_PATH_CHARS))
				{
					PM_PICK_ENTRY* itemData = (PM_PICK_ENTRY*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PM_PICK_ENTRY));
					if (itemData)
					{
						itemData->path = DuplicateText(queryPath);
						if (!itemData->path)
						{
							HeapFree(GetProcessHeap(), 0, itemData);
							itemData = NULL;
						}
					}
					if (itemData)
					{
						g_pickerEntries[g_pickerEntryCount++] = itemData;
						itemData->pid = processEntry.th32ProcessID;
						LVITEMW item;
						ZeroMemory(&item, sizeof(item));
						item.mask = LVIF_TEXT | LVIF_PARAM;
						item.iItem = ListView_GetItemCount(list);
						item.pszText = processEntry.szExeFile;
						item.lParam = (LPARAM)itemData;
						int row = ListView_InsertItem(list, &item);
						if (row >= 0)
						{
							WCHAR pid[32]; wsprintfW(pid, L"%u", itemData->pid);
							SetListText(list, row, 1, pid);
							SetListText(list, row, 2, itemData->path);
						}
						else
						{
							g_pickerEntries[--g_pickerEntryCount] = NULL;
							HeapFree(GetProcessHeap(), 0, itemData->path);
							HeapFree(GetProcessHeap(), 0, itemData);
						}
					}
				}
				HeapFree(GetProcessHeap(), 0, queryPath);
				CloseHandle(process);
			}
		}
		hasEntry = Process32NextW(snapshot, &processEntry);
	}
	CloseHandle(snapshot);
}

static INT_PTR CALLBACK ProcessPickerDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
	WCHAR* output = (WCHAR*)GetWindowLongPtrW(dialog, DWLP_USER);
	switch (message)
	{
	case WM_INITDIALOG:
		SetWindowLongPtrW(dialog, DWLP_USER, lParam);
		SendMessageW(dialog, WM_SETICON, ICON_SMALL, (LPARAM)g_appIcon);
		{
			HWND list = GetDlgItem(dialog, IDC_PM_PROCESS_LIST);
			ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
			ConfigureListColumn(list, 0, 120, L"进程");
			ConfigureListColumn(list, 1, 60, L"PID");
			ConfigureListColumn(list, 2, 300, L"完整路径");
			RefreshProcessPicker(dialog);
		}
		return TRUE;
	case WM_NOTIFY:
		if (((LPNMHDR)lParam)->idFrom == IDC_PM_PROCESS_LIST && ((LPNMHDR)lParam)->code == NM_DBLCLK)
			PostMessageW(dialog, WM_COMMAND, IDC_PM_PICK_SELECT, 0);
		break;
	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDC_PM_PICK_REFRESH:
			RefreshProcessPicker(dialog);
			return TRUE;
		case IDC_PM_PICK_SELECT:
		{
			HWND list = GetDlgItem(dialog, IDC_PM_PROCESS_LIST);
			int selected = SelectedListIndex(list);
			if (selected < 0)
			{
				MessageBoxW(dialog, L"请先选择一个进程。", L"从当前进程选择", MB_ICONINFORMATION | MB_OK);
				return TRUE;
			}
			LVITEMW item;
			ZeroMemory(&item, sizeof(item));
			item.mask = LVIF_PARAM;
			item.iItem = selected;
			if (ListView_GetItem(list, &item))
			{
				PM_PICK_ENTRY* entry = (PM_PICK_ENTRY*)item.lParam;
				if (entry && output)
					CopyText(output, PM_MAX_PATH_CHARS, entry->path);
			}
			EndDialog(dialog, IDOK);
			return TRUE;
		}
		case IDC_PM_PICK_CANCEL:
		case IDCANCEL:
			EndDialog(dialog, IDCANCEL);
			return TRUE;
		}
		break;
	case WM_DESTROY:
		FreePickerEntries(GetDlgItem(dialog, IDC_PM_PROCESS_LIST));
		break;
	}
	return FALSE;
}

static BOOL EditRule(HWND owner, BOOL editing, DWORD index)
{
	PM_RULE* candidate = (PM_RULE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PM_RULE));
	if (!candidate)
		return FALSE;
	if (editing)
	{
		EnterCriticalSection(&g_configLock);
		if (index >= g_config.count)
		{
			LeaveCriticalSection(&g_configLock);
			HeapFree(GetProcessHeap(), 0, candidate);
			return FALSE;
		}
		BOOL copied = CopyRule(candidate, &g_config.rules[index]);
		LeaveCriticalSection(&g_configLock);
		if (!copied)
		{
			HeapFree(GetProcessHeap(), 0, candidate);
			return FALSE;
		}
	}
	else
	{
		if (!InitializeRuleDefaults(candidate))
		{
			HeapFree(GetProcessHeap(), 0, candidate);
			MessageBoxW(owner, L"无法创建规则标识，规则未新增。", L"进程资源监控", MB_ICONERROR | MB_OK);
			return FALSE;
		}
	}
	PM_EDIT_CONTEXT context = { editing, index, candidate, FALSE };
	if (DialogBoxParamW(g_instance, MAKEINTRESOURCEW(IDD_PROCESS_RULE_EDIT), owner, RuleEditDialogProc, (LPARAM)&context) != IDOK || !context.accepted)
	{
		FreeRule(candidate);
		HeapFree(GetProcessHeap(), 0, candidate);
		return FALSE;
	}
	EnterCriticalSection(&g_configLock);
	PM_CONFIG* updated = CloneConfiguration(&g_config);
	LeaveCriticalSection(&g_configLock);
	if (!updated)
	{
		FreeRule(candidate);
		HeapFree(GetProcessHeap(), 0, candidate);
		MessageBoxW(owner, L"内存不足，规则未保存。", L"进程资源监控", MB_ICONERROR | MB_OK);
		return FALSE;
	}
	BOOL changed = FALSE;
	if (editing && index < updated->count)
		changed = CopyRule(&updated->rules[index], candidate);
	else if (!editing && updated->count < PM_MAX_RULES)
	{
		changed = CopyRule(&updated->rules[updated->count], candidate);
		if (changed)
			++updated->count;
	}
	FreeRule(candidate);
	HeapFree(GetProcessHeap(), 0, candidate);
	if (!changed)
	{
		FreeConfiguration(updated);
		HeapFree(GetProcessHeap(), 0, updated);
		MessageBoxW(owner, editing ? L"内存不足，规则未保存。" : L"最多只能创建 64 条进程监控规则。",
			L"进程资源监控", MB_ICONWARNING | MB_OK);
		return FALSE;
	}
	if (!CommitConfiguration(updated))
	{
		MessageBoxW(owner, L"保存规则文件失败，已保留上一次有效配置。", L"进程资源监控", MB_ICONERROR | MB_OK);
		return FALSE;
	}
	return TRUE;
}

static void QueueTestAlert(const PM_RULE* rule)
{
	PM_INCIDENT* incident = (PM_INCIDENT*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PM_INCIDENT));
	if (!incident)
		return;
	incident->ruleId = rule->id;
	incident->generation = g_config.generation;
	incident->pid = 1234;
	incident->metricMask = rule->metricMask;
	incident->cpuThreshold = rule->cpuThreshold;
	incident->memoryThreshold = rule->memoryThreshold;
	incident->gpuThreshold = rule->gpuThreshold;
	incident->cpuValue = (rule->cpuThreshold + 3 > 100) ? 100 : rule->cpuThreshold + 3;
	incident->memoryValue = (rule->memoryThreshold + 3 > 100) ? 100 : rule->memoryThreshold + 3;
	incident->gpuValue = (rule->gpuThreshold + 3 > 100) ? 100 : rule->gpuThreshold + 3;
	incident->holdSeconds = rule->holdSeconds;
	incident->action = PM_ACTION_PROMPT;
	incident->path = DuplicateText(rule->path);
	if (incident->path)
		QueueUiEvent(PM_UI_ALERT, incident, L"TEST");
	FreeIncident(incident);
	HeapFree(GetProcessHeap(), 0, incident);
}

static INT_PTR CALLBACK RulesDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_INITDIALOG:
		g_rulesWindow = dialog;
		SendMessageW(dialog, WM_SETICON, ICON_BIG, (LPARAM)g_appIcon);
		SendMessageW(dialog, WM_SETICON, ICON_SMALL, (LPARAM)g_appIcon);
		{
			HWND list = GetDlgItem(dialog, IDC_PM_RULE_LIST);
			ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
			ConfigureListColumn(list, 0, 44, L"启用");
			ConfigureListColumn(list, 1, 112, L"程序");
			ConfigureListColumn(list, 2, 48, L"CPU");
			ConfigureListColumn(list, 3, 52, L"内存");
			ConfigureListColumn(list, 4, 48, L"GPU");
			ConfigureListColumn(list, 5, 58, L"持续");
			ConfigureListColumn(list, 6, 72, L"处理");
			ConfigureListColumn(list, 7, 120, L"状态");
			RefreshRulesList(dialog);
		}
		return TRUE;
	case WM_PROCESS_MONITOR_UI:
		RefreshRulesList(dialog);
		return TRUE;
	case WM_NOTIFY:
		if (((LPNMHDR)lParam)->idFrom == IDC_PM_RULE_LIST && ((LPNMHDR)lParam)->code == NM_DBLCLK)
			PostMessageW(dialog, WM_COMMAND, IDC_PM_EDIT, 0);
		break;
	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDC_PM_GLOBAL_ENABLE:
		{
			BOOL enabled = IsDlgButtonChecked(dialog, IDC_PM_GLOBAL_ENABLE) == BST_CHECKED;
			EnterCriticalSection(&g_configLock);
			PM_CONFIG* updated = CloneConfiguration(&g_config);
			LeaveCriticalSection(&g_configLock);
			if (updated)
				updated->enabled = enabled;
			if (!updated)
				MessageBoxW(dialog, L"保存总开关失败。", L"进程资源监控", MB_ICONERROR | MB_OK);
			else if (!CommitConfiguration(updated))
				MessageBoxW(dialog, L"保存总开关失败。", L"进程资源监控", MB_ICONERROR | MB_OK);
			RefreshRulesList(dialog);
			return TRUE;
		}
		case IDC_PM_ADD:
			if (EditRule(dialog, FALSE, 0)) RefreshRulesList(dialog);
			return TRUE;
		case IDC_PM_EDIT:
		{
			int selected = SelectedListIndex(GetDlgItem(dialog, IDC_PM_RULE_LIST));
			if (selected >= 0 && EditRule(dialog, TRUE, selected)) RefreshRulesList(dialog);
			return TRUE;
		}
		case IDC_PM_TOGGLE:
		{
			int selected = SelectedListIndex(GetDlgItem(dialog, IDC_PM_RULE_LIST));
			if (selected >= 0)
			{
				EnterCriticalSection(&g_configLock);
				PM_CONFIG* updated = CloneConfiguration(&g_config);
				LeaveCriticalSection(&g_configLock);
				if (updated && (DWORD)selected < updated->count)
					updated->rules[selected].enabled = !updated->rules[selected].enabled;
				if (!updated)
				{
					MessageBoxW(dialog, L"保存规则状态失败，已保留上一次有效配置。", L"进程资源监控", MB_ICONERROR | MB_OK);
				}
				else if ((DWORD)selected >= updated->count)
				{
					FreeConfiguration(updated);
					HeapFree(GetProcessHeap(), 0, updated);
					MessageBoxW(dialog, L"保存规则状态失败，已保留上一次有效配置。", L"进程资源监控", MB_ICONERROR | MB_OK);
				}
				else if (!CommitConfiguration(updated))
				{
					MessageBoxW(dialog, L"保存规则状态失败，已保留上一次有效配置。", L"进程资源监控", MB_ICONERROR | MB_OK);
				}
				RefreshRulesList(dialog);
			}
			return TRUE;
		}
		case IDC_PM_DELETE:
		{
			int selected = SelectedListIndex(GetDlgItem(dialog, IDC_PM_RULE_LIST));
			if (selected >= 0 && MessageBoxW(dialog, L"确定删除选中的规则吗？", L"进程资源监控", MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) == IDYES)
			{
				EnterCriticalSection(&g_configLock);
				PM_CONFIG* updated = CloneConfiguration(&g_config);
				LeaveCriticalSection(&g_configLock);
				BOOL validSelection = updated && (DWORD)selected < updated->count;
				if (validSelection)
				{
					FreeRule(&updated->rules[selected]);
					for (DWORD index = selected + 1; index < updated->count; ++index)
					{
						updated->rules[index - 1] = updated->rules[index];
						ZeroMemory(&updated->rules[index], sizeof(PM_RULE));
					}
					--updated->count;
				}
				if (!updated)
				{
					MessageBoxW(dialog, L"删除规则失败，已保留上一次有效配置。", L"进程资源监控", MB_ICONERROR | MB_OK);
				}
				else if (!validSelection)
				{
					FreeConfiguration(updated);
					HeapFree(GetProcessHeap(), 0, updated);
					MessageBoxW(dialog, L"删除规则失败，已保留上一次有效配置。", L"进程资源监控", MB_ICONERROR | MB_OK);
				}
				else if (!CommitConfiguration(updated))
				{
					MessageBoxW(dialog, L"删除规则失败，已保留上一次有效配置。", L"进程资源监控", MB_ICONERROR | MB_OK);
				}
				RefreshRulesList(dialog);
			}
			return TRUE;
		}
		case IDC_PM_TEST:
		{
			int selected = SelectedListIndex(GetDlgItem(dialog, IDC_PM_RULE_LIST));
			if (selected >= 0)
			{
				EnterCriticalSection(&g_configLock);
				if ((DWORD)selected < g_config.count) QueueTestAlert(&g_config.rules[selected]);
				LeaveCriticalSection(&g_configLock);
			}
			return TRUE;
		}
		case IDC_PM_CLOSE:
		case IDCANCEL:
			DestroyWindow(dialog);
			return TRUE;
		}
		break;
	case WM_DESTROY:
		if (g_rulesWindow == dialog) g_rulesWindow = NULL;
		return TRUE;
	}
	return FALSE;
}

static void RemoveAlertForIncident(const PM_INCIDENT* incident)
{
	if (!incident)
		return;
	DWORD remaining = g_alertCount;
	PM_INCIDENT* retained = (PM_INCIDENT*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
		PM_MAX_ALERTS * sizeof(PM_INCIDENT));
	if (!retained)
		return;
	DWORD retainedCount = 0;
	while (remaining-- > 0)
	{
		PM_INCIDENT current;
		ZeroMemory(&current, sizeof(current));
		MoveIncident(&current, &g_alerts[g_alertHead]);
		g_alertHead = (g_alertHead + 1) % PM_MAX_ALERTS;
		--g_alertCount;
		if (!(current.pid == incident->pid && current.createTime == incident->createTime && GuidEqual(&current.ruleId, &incident->ruleId)))
			MoveIncident(&retained[retainedCount++], &current);
		FreeIncident(&current);
	}
	g_alertHead = g_alertTail = g_alertCount = 0;
	for (DWORD index = 0; index < retainedCount; ++index)
	{
		MoveIncident(&g_alerts[g_alertTail], &retained[index]);
		g_alertTail = (g_alertTail + 1) % PM_MAX_ALERTS;
		++g_alertCount;
	}
	HeapFree(GetProcessHeap(), 0, retained);
	if (g_hasCurrentAlert && g_currentAlert.pid == incident->pid && g_currentAlert.createTime == incident->createTime &&
		GuidEqual(&g_currentAlert.ruleId, &incident->ruleId))
	{
		DismissAlertNotification();
		g_hasCurrentAlert = FALSE;
		FreeIncident(&g_currentAlert);
		if (g_alertWindow)
		{
			HWND window = g_alertWindow;
			g_alertWindow = NULL;
			DestroyWindow(window);
		}
	}
	ShowNextAlert();
}

static void ClearAllAlerts()
{
	DismissAlertNotification();
	while (g_alertCount > 0)
	{
		FreeIncident(&g_alerts[g_alertHead]);
		g_alertHead = (g_alertHead + 1) % PM_MAX_ALERTS;
		--g_alertCount;
	}
	g_alertHead = g_alertTail = 0;
	g_hasCurrentAlert = FALSE;
	FreeIncident(&g_currentAlert);
	if (g_alertWindow)
	{
		HWND window = g_alertWindow;
		g_alertWindow = NULL;
		DestroyWindow(window);
	}
}

static BOOL ShowAlertNotification()
{
	if (!g_mainWindow || !g_hasCurrentAlert)
		return FALSE;

	NOTIFYICONDATAW data;
	ZeroMemory(&data, sizeof(data));
	data.cbSize = sizeof(data);
	data.hWnd = g_mainWindow;
	data.uID = g_trayIconId;
	data.uFlags = NIF_INFO;
	CopyText(data.szInfoTitle, ARRAYSIZE(data.szInfoTitle), L"TrayS 进程资源告警");
	WCHAR metrics[384];
	FormatIncidentMetrics(&g_currentAlert, metrics, ARRAYSIZE(metrics));
	WCHAR body[256] = L"";
	AppendText(body, ARRAYSIZE(body), L"程序：");
	AppendText(body, ARRAYSIZE(body), BaseName(g_currentAlert.path));
	WCHAR pidText[32];
	wsprintfW(pidText, L"（PID %u）\r\n", g_currentAlert.pid);
	AppendText(body, ARRAYSIZE(body), pidText);
	AppendText(body, ARRAYSIZE(body), metrics);
	AppendText(body, ARRAYSIZE(body), L"\r\n单击通知选择处理方式；关闭则暂不终止。");
	CopyText(data.szInfo, ARRAYSIZE(data.szInfo), body);
	data.dwInfoFlags = NIIF_WARNING;
	data.uTimeout = PM_ALERT_TIMEOUT_MS;

	if (!g_trayIconAvailable)
	{
		// A disabled tray icon cannot receive balloon callbacks, so create a temporary one
		// only for the lifetime of this notification.
		data.uID = g_trayIconId + 1;
		data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
		data.hIcon = g_appIcon;
		data.uCallbackMessage = WM_PROCESS_MONITOR_NOTIFICATION;
		CopyText(data.szTip, ARRAYSIZE(data.szTip), L"TrayS 进程监控");
		if (!Shell_NotifyIconW(NIM_ADD, &data))
			return FALSE;
		g_alertTemporaryTrayIcon = TRUE;
		g_alertNotificationIconId = data.uID;
		data.uFlags = NIF_INFO;
	}
	else
		g_alertNotificationIconId = data.uID;

	if (!Shell_NotifyIconW(NIM_MODIFY, &data))
	{
		if (g_alertTemporaryTrayIcon)
		{
			NOTIFYICONDATAW removeData;
			ZeroMemory(&removeData, sizeof(removeData));
			removeData.cbSize = sizeof(removeData);
			removeData.hWnd = g_mainWindow;
			removeData.uID = g_alertNotificationIconId;
			Shell_NotifyIconW(NIM_DELETE, &removeData);
		}
		g_alertTemporaryTrayIcon = FALSE;
		g_alertNotificationIconId = 0;
		return FALSE;
	}

	g_alertNotificationActive = TRUE;
	g_alertTimerId = SetTimer(NULL, 0, PM_ALERT_TIMEOUT_MS, AlertNotificationTimerProc);
	return TRUE;
}

static void DismissAlertNotification()
{
	if (g_alertTimerId)
	{
		KillTimer(NULL, g_alertTimerId);
		g_alertTimerId = 0;
	}
	BOOL wasActive = g_alertNotificationActive;
	BOOL temporary = g_alertTemporaryTrayIcon;
	UINT iconId = g_alertNotificationIconId;
	g_alertNotificationActive = FALSE;
	g_alertTemporaryTrayIcon = FALSE;
	g_alertNotificationIconId = 0;
	if (!g_mainWindow || !iconId)
		return;

	NOTIFYICONDATAW data;
	ZeroMemory(&data, sizeof(data));
	data.cbSize = sizeof(data);
	data.hWnd = g_mainWindow;
	data.uID = iconId;
	if (temporary)
	{
		Shell_NotifyIconW(NIM_DELETE, &data);
	}
	else if (wasActive)
	{
		data.uFlags = NIF_INFO;
		data.szInfo[0] = 0;
		Shell_NotifyIconW(NIM_MODIFY, &data);
	}
}

static VOID CALLBACK AlertNotificationTimerProc(HWND window, UINT message, UINT_PTR timerId, DWORD time)
{
	UNREFERENCED_PARAMETER(window);
	UNREFERENCED_PARAMETER(message);
	UNREFERENCED_PARAMETER(timerId);
	UNREFERENCED_PARAMETER(time);
	if (g_alertNotificationActive)
		FinishCurrentAlert(FALSE);
}

static HRESULT CALLBACK AlertTaskDialogCallback(HWND dialog, UINT notification, WPARAM wParam, LPARAM lParam, LONG_PTR referenceData)
{
	UNREFERENCED_PARAMETER(wParam);
	UNREFERENCED_PARAMETER(lParam);
	UNREFERENCED_PARAMETER(referenceData);
	if (notification == TDN_CREATED)
	{
		g_alertWindow = dialog;
		SetWindowPos(dialog, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		if (g_hasCurrentAlert && g_currentAlert.pid == 1234 && g_currentAlert.createTime == 0)
			SendMessageW(dialog, TDM_ENABLE_BUTTON, PM_TASK_ALERT_TERMINATE, FALSE);
	}
	else if (notification == TDN_DESTROYED && g_alertWindow == dialog)
		g_alertWindow = NULL;
	return S_OK;
}

static BOOL ShowSystemAlert(int* selectedButton)
{
	if (!selectedButton || !g_hasCurrentAlert)
		return FALSE;
	HMODULE commonControls = LoadLibraryW(L"comctl32.dll");
	if (!commonControls)
		return FALSE;
	PM_TASK_DIALOG_INDIRECT taskDialog = (PM_TASK_DIALOG_INDIRECT)GetProcAddress(commonControls, "TaskDialogIndirect");
	if (!taskDialog)
	{
		FreeLibrary(commonControls);
		return FALSE;
	}

	WCHAR metrics[384];
	FormatIncidentMetrics(&g_currentAlert, metrics, ARRAYSIZE(metrics));
	const WCHAR* alertPath = g_currentAlert.path ? g_currentAlert.path : L"";
	WCHAR pidText[32];
	WCHAR holdText[32];
	wsprintfW(pidText, L"%u", g_currentAlert.pid);
	wsprintfW(holdText, L"%u", g_currentAlert.holdSeconds);
	WCHAR content[768] = L"";
	AppendText(content, ARRAYSIZE(content), L"程序：");
	AppendText(content, ARRAYSIZE(content), BaseName(alertPath));
	AppendText(content, ARRAYSIZE(content), L"\r\nPID：");
	AppendText(content, ARRAYSIZE(content), pidText);
	AppendText(content, ARRAYSIZE(content), L"\r\n连续超限：");
	AppendText(content, ARRAYSIZE(content), holdText);
	AppendText(content, ARRAYSIZE(content), L" 秒\r\n");
	AppendText(content, ARRAYSIZE(content), metrics);

	SIZE_T detailsCount = (SIZE_T)lstrlenW(alertPath) + lstrlenW(metrics) + 128;
	WCHAR* details = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, detailsCount * sizeof(WCHAR));
	if (details)
	{
		AppendText(details, (DWORD)detailsCount, L"完整路径：");
		AppendText(details, (DWORD)detailsCount, alertPath);
		AppendText(details, (DWORD)detailsCount, L"\r\n\r\n");
		AppendText(details, (DWORD)detailsCount, metrics);
	}

	TASKDIALOG_BUTTON buttons[] =
	{
		{ PM_TASK_ALERT_TERMINATE, L"终止进程\n先请求正常关闭，5 秒后仍在运行才强制终止" },
		{ PM_TASK_ALERT_IGNORE, L"暂不终止\n忽略本次告警，资源恢复后才会重新提示" }
	};
	TASKDIALOGCONFIG config;
	ZeroMemory(&config, sizeof(config));
	config.cbSize = sizeof(config);
	config.hwndParent = g_mainWindow;
	config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_USE_COMMAND_LINKS | TDF_SIZE_TO_CONTENT;
	config.pszWindowTitle = L"TrayS 进程资源告警";
	config.pszMainIcon = TD_WARNING_ICON;
	config.pszMainInstruction = (g_currentAlert.pid == 1234 && g_currentAlert.createTime == 0) ?
		L"测试提示（不会执行真实终止操作）" : L"检测到进程持续超出资源阈值";
	config.pszContent = content;
	config.cButtons = ARRAYSIZE(buttons);
	config.pButtons = buttons;
	config.nDefaultButton = PM_TASK_ALERT_IGNORE;
	config.pszExpandedInformation = details;
	config.pszExpandedControlText = L"查看详细信息";
	config.pszCollapsedControlText = L"隐藏详细信息";
	config.pszFooter = L"关闭窗口或按 Esc 等同于“暂不终止”。";
	config.pfCallback = AlertTaskDialogCallback;

	int button = 0;
	HRESULT result = taskDialog(&config, &button, NULL, NULL);
	if (details)
		HeapFree(GetProcessHeap(), 0, details);
	FreeLibrary(commonControls);
	if (FAILED(result))
		return FALSE;
	*selectedButton = button;
	return TRUE;
}

static void ShowNextAlert()
{
	if (g_taskDialogRunning || g_alertNotificationActive || g_alertWindow || g_hasCurrentAlert || g_alertCount == 0)
		return;
	MoveIncident(&g_currentAlert, &g_alerts[g_alertHead]);
	g_alertHead = (g_alertHead + 1) % PM_MAX_ALERTS;
	--g_alertCount;
	g_hasCurrentAlert = TRUE;
	if (ShowAlertNotification())
		return;

	// If the Windows notification service is unavailable, retain the native task dialog
	// so a resource alert is never silently discarded.
	int selectedButton = 0;
	g_taskDialogRunning = TRUE;
	BOOL shownBySystem = ShowSystemAlert(&selectedButton);
	if (shownBySystem)
	{
		if (g_hasCurrentAlert)
			FinishCurrentAlert(selectedButton == PM_TASK_ALERT_TERMINATE);
		g_taskDialogRunning = FALSE;
		ShowNextAlert();
		return;
	}
	g_taskDialogRunning = FALSE;
	// Compatibility fallback for systems where the native task dialog API cannot be loaded.
	g_alertWindow = CreateDialogParamW(g_instance, MAKEINTRESOURCEW(IDD_PROCESS_ALERT), NULL, AlertDialogProc, 0);
	if (!g_alertWindow)
	{
		g_hasCurrentAlert = FALSE;
		FreeIncident(&g_currentAlert);
		return;
	}
	ShowWindow(g_alertWindow, SW_SHOWNORMAL);
	SetForegroundWindow(g_alertWindow);
}

static void EnqueueAlert(const PM_INCIDENT* incident)
{
	if (g_alertCount == PM_MAX_ALERTS)
	{
		Audit(L"alert-queue-full", incident, L"alert queue full");
		QueueUiEvent(PM_UI_RESULT, incident, L"进程资源告警队列已满，新的提示未能显示；请检查规则数量。");
		return;
	}
	if (!CopyIncident(&g_alerts[g_alertTail], incident))
	{
		Audit(L"alert-allocation-failed", incident, L"alert allocation failed");
		QueueUiEvent(PM_UI_RESULT, incident, L"内存不足，进程资源告警未能显示。");
		return;
	}
	g_alertTail = (g_alertTail + 1) % PM_MAX_ALERTS;
	++g_alertCount;
	ShowNextAlert();
}

static void FinishCurrentAlert(BOOL terminate)
{
	if (!g_hasCurrentAlert)
		return;
	DismissAlertNotification();
	PM_INCIDENT incident;
	ZeroMemory(&incident, sizeof(incident));
	MoveIncident(&incident, &g_currentAlert);
	g_hasCurrentAlert = FALSE;
	if (g_alertWindow)
	{
		HWND window = g_alertWindow;
		g_alertWindow = NULL;
		DestroyWindow(window);
	}
	if (terminate)
	{
		if (incident.pid == 1234 && incident.createTime == 0)
			Audit(L"test-alert", &incident, L"test alert cannot terminate a real process");
		else
		{
			Audit(L"user-terminate", &incident, L"user requested termination");
			QueueAction(&incident);
		}
	}
	else
		Audit(L"user-ignore", &incident, L"user chose not to terminate");
	FreeIncident(&incident);
	ShowNextAlert();
}

static INT_PTR CALLBACK AlertDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_INITDIALOG:
		SendMessageW(dialog, WM_SETICON, ICON_BIG, (LPARAM)g_appIcon);
		SendMessageW(dialog, WM_SETICON, ICON_SMALL, (LPARAM)g_appIcon);
		if (g_hasCurrentAlert)
		{
			WCHAR metrics[384];
			FormatIncidentMetrics(&g_currentAlert, metrics, ARRAYSIZE(metrics));
			if (g_currentAlert.pid == 1234 && g_currentAlert.createTime == 0)
			{
				SetDlgItemTextW(dialog, IDC_PM_ALERT_TITLE, L"测试提示（不会执行真实终止操作）");
				EnableWindow(GetDlgItem(dialog, IDC_PM_ALERT_TERMINATE), FALSE);
			}
			const WCHAR* alertPath = g_currentAlert.path ? g_currentAlert.path : L"";
			WCHAR pidText[32];
			WCHAR holdText[32];
			wsprintfW(pidText, L"%u", g_currentAlert.pid);
			wsprintfW(holdText, L"%u", g_currentAlert.holdSeconds);
			SIZE_T detailsCount = (SIZE_T)lstrlenW(alertPath) * 2 + lstrlenW(metrics) + 256;
			WCHAR* details = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, detailsCount * sizeof(WCHAR));
			if (details)
			{
				AppendText(details, (DWORD)detailsCount, L"程序：");
				AppendText(details, (DWORD)detailsCount, BaseName(alertPath));
				AppendText(details, (DWORD)detailsCount, L"\r\n路径：");
				AppendText(details, (DWORD)detailsCount, alertPath);
				AppendText(details, (DWORD)detailsCount, L"\r\nPID：");
				AppendText(details, (DWORD)detailsCount, pidText);
				AppendText(details, (DWORD)detailsCount, L"\r\n连续超限：");
				AppendText(details, (DWORD)detailsCount, holdText);
				AppendText(details, (DWORD)detailsCount, L" 秒\r\n\r\n");
				AppendText(details, (DWORD)detailsCount, metrics);
				SetDlgItemTextW(dialog, IDC_PM_ALERT_DETAILS, details);
				HeapFree(GetProcessHeap(), 0, details);
			}
			SetFocus(GetDlgItem(dialog, IDC_PM_ALERT_IGNORE));
			return FALSE;
		}
		return TRUE;
	case WM_COMMAND:
		if (LOWORD(wParam) == IDC_PM_ALERT_TERMINATE)
		{
			FinishCurrentAlert(TRUE);
			return TRUE;
		}
		if (LOWORD(wParam) == IDC_PM_ALERT_IGNORE || LOWORD(wParam) == IDCANCEL)
		{
			FinishCurrentAlert(FALSE);
			return TRUE;
		}
		break;
	case WM_CLOSE:
		FinishCurrentAlert(FALSE);
		return TRUE;
	case WM_DESTROY:
		if (g_alertWindow == dialog) g_alertWindow = NULL;
		return TRUE;
	}
	return FALSE;
}

static INT_PTR CALLBACK ResultDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_INITDIALOG:
		g_resultWindow = dialog;
		SetDlgItemTextW(dialog, IDC_PM_RESULT_TEXT, g_resultMessage);
		SetTimer(dialog, 1, 8000, NULL);
		return TRUE;
	case WM_TIMER:
		if (wParam != 1)
			break;
		DestroyWindow(dialog);
		return TRUE;
	case WM_CLOSE:
		DestroyWindow(dialog);
		return TRUE;
	case WM_COMMAND:
		if (LOWORD(wParam) == IDC_PM_RESULT_CLOSE || LOWORD(wParam) == IDCANCEL)
		{
			DestroyWindow(dialog);
			return TRUE;
		}
		break;
	case WM_DESTROY:
		KillTimer(dialog, 1);
		if (g_resultWindow == dialog) g_resultWindow = NULL;
		return TRUE;
	}
	return FALSE;
}

static void ShowResultNotification(const WCHAR* message, BOOL trayIconAvailable)
{
	if (trayIconAvailable && g_mainWindow && !g_alertNotificationActive)
	{
		NOTIFYICONDATAW data;
		ZeroMemory(&data, sizeof(data));
		data.cbSize = sizeof(data);
		data.hWnd = g_mainWindow;
		data.uID = g_trayIconId;
		data.uFlags = NIF_INFO;
		CopyText(data.szInfoTitle, ARRAYSIZE(data.szInfoTitle), L"TrayS 进程监控");
		CopyText(data.szInfo, ARRAYSIZE(data.szInfo), message);
		data.dwInfoFlags = NIIF_INFO;
		Shell_NotifyIconW(NIM_MODIFY, &data);
	}
	else
	{
		CopyText(g_resultMessage, ARRAYSIZE(g_resultMessage), message);
		if (g_resultWindow)
		{
			SetDlgItemTextW(g_resultWindow, IDC_PM_RESULT_TEXT, g_resultMessage);
			KillTimer(g_resultWindow, 1);
			SetTimer(g_resultWindow, 1, 8000, NULL);
		}
		else
		{
			g_resultWindow = CreateDialogParamW(g_instance, MAKEINTRESOURCEW(IDD_PROCESS_RESULT), NULL, ResultDialogProc, 0);
			if (g_resultWindow)
				ShowWindow(g_resultWindow, SW_SHOWNOACTIVATE);
		}
	}
}

BOOL ProcessMonitorInitialize(HWND mainWindow, HINSTANCE instance, HICON appIcon, UINT trayIconId)
{
	if (g_locksInitialized)
		return TRUE;
	INITCOMMONCONTROLSEX commonControls = { sizeof(commonControls), ICC_LISTVIEW_CLASSES };
	if (!InitCommonControlsEx(&commonControls))
		return FALSE;
	g_mainWindow = mainWindow;
	g_instance = instance;
	g_appIcon = appIcon;
	g_trayIconId = trayIconId;
	g_taskDialogRunning = FALSE;
	g_alertNotificationActive = FALSE;
	g_alertTemporaryTrayIcon = FALSE;
	g_trayIconAvailable = FALSE;
	g_alertNotificationIconId = 0;
	g_alertTimerId = 0;
	InitializeCriticalSection(&g_configLock);
	InitializeCriticalSection(&g_uiLock);
	InitializeCriticalSection(&g_actionLock);
	InitializeCriticalSection(&g_logLock);
	InitializeCriticalSection(&g_runtimeLock);
	g_locksInitialized = TRUE;
	WCHAR* modulePath = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, PM_MAX_PATH_CHARS * sizeof(WCHAR));
	if (!modulePath)
	{
		ProcessMonitorShutdown();
		return FALSE;
	}
	DWORD moduleLength = GetModuleFileNameW(NULL, modulePath, PM_MAX_PATH_CHARS);
	if (!moduleLength || moduleLength >= PM_MAX_PATH_CHARS)
	{
		HeapFree(GetProcessHeap(), 0, modulePath);
		ProcessMonitorShutdown();
		return FALSE;
	}
	g_selfPath = DuplicateText(modulePath);
	g_moduleDirectory = DuplicateText(modulePath);
	HeapFree(GetProcessHeap(), 0, modulePath);
	if (!g_selfPath || !g_moduleDirectory)
	{
		ProcessMonitorShutdown();
		return FALSE;
	}
	BOOL directoryFound = FALSE;
	for (DWORD index = lstrlenW(g_moduleDirectory); index > 0; --index)
	{
		if (g_moduleDirectory[index - 1] == L'\\')
		{
			if (index == 3 && g_moduleDirectory[1] == L':')
				g_moduleDirectory[index] = 0;
			else
				g_moduleDirectory[index - 1] = 0;
			directoryFound = TRUE;
			break;
		}
	}
	if (!directoryFound)
	{
		ProcessMonitorShutdown();
		return FALSE;
	}
	if ((DWORD)lstrlenW(g_moduleDirectory) > PM_MAX_PATH_CHARS - 64)
	{
		ProcessMonitorShutdown();
		return FALSE;
	}
	g_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	g_configChangedEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	g_actionEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!g_stopEvent || !g_configChangedEvent || !g_actionEvent)
	{
		ProcessMonitorShutdown();
		return FALSE;
	}
	HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
	g_isProcessCritical = kernel ? (PM_IS_PROCESS_CRITICAL)GetProcAddress(kernel, "IsProcessCritical") : NULL;
	LoadConfiguration();
	g_workerConfig = (PM_CONFIG*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PM_CONFIG));
	if (!g_workerConfig || !CopyWorkerConfiguration())
	{
		ProcessMonitorShutdown();
		return FALSE;
	}
	g_actionThread = CreateThread(NULL, 0, ActionThreadProc, NULL, 0, NULL);
	g_monitorThread = CreateThread(NULL, 0, MonitorThreadProc, NULL, 0, NULL);
	if (!g_actionThread || !g_monitorThread)
	{
		ProcessMonitorShutdown();
		return FALSE;
	}
	return TRUE;
}

void ProcessMonitorShutdown()
{
	if (!g_locksInitialized)
		return;
	if (g_stopEvent)
		SetEvent(g_stopEvent);
	if (g_actionEvent)
		SetEvent(g_actionEvent);
	if (g_configChangedEvent)
		SetEvent(g_configChangedEvent);
	if (g_monitorThread)
	{
		WaitForSingleObject(g_monitorThread, INFINITE);
		CloseHandle(g_monitorThread);
		g_monitorThread = NULL;
	}
	if (g_actionThread)
	{
		WaitForSingleObject(g_actionThread, INFINITE);
		CloseHandle(g_actionThread);
		g_actionThread = NULL;
	}
	if (g_rulesWindow) DestroyWindow(g_rulesWindow);
	if (g_alertWindow) DestroyWindow(g_alertWindow);
	if (g_resultWindow) DestroyWindow(g_resultWindow);
	g_rulesWindow = g_alertWindow = g_resultWindow = NULL;
	ClearAllAlerts();
	FreeIncident(&g_dispatchEvent.incident);
	EnterCriticalSection(&g_uiLock);
	for (DWORD index = 0; index < PM_MAX_UI_EVENTS; ++index)
		FreeIncident(&g_uiEvents[index].incident);
	g_uiHead = g_uiTail = g_uiCount = 0;
	LeaveCriticalSection(&g_uiLock);
	EnterCriticalSection(&g_actionLock);
	for (DWORD index = 0; index < PM_MAX_ACTIONS; ++index)
		FreeIncident(&g_actions[index]);
	g_actionHead = g_actionTail = g_actionCount = 0;
	LeaveCriticalSection(&g_actionLock);
	if (g_workerConfig)
	{
		FreeConfiguration(g_workerConfig);
		HeapFree(GetProcessHeap(), 0, g_workerConfig);
		g_workerConfig = NULL;
	}
	FreeConfiguration(&g_config);
	CloseGpuQuery();
	if (g_pdhModule) FreeLibrary(g_pdhModule);
	if (g_psapiModule) FreeLibrary(g_psapiModule);
	g_pdhModule = NULL;
	g_psapiModule = NULL;
	g_pdhOpenQuery = NULL;
	g_pdhAddEnglishCounter = NULL;
	g_pdhCollectQueryData = NULL;
	g_pdhGetFormattedCounterArray = NULL;
	g_pdhCloseQuery = NULL;
	g_pdhLoadAttempted = FALSE;
	g_gpuFailureCount = 0;
	g_gpuUsageCount = 0;
	g_gpuAvailable = FALSE;
	g_getProcessMemoryInfo = NULL;
	g_isProcessCritical = NULL;
	if (g_stopEvent) CloseHandle(g_stopEvent);
	if (g_configChangedEvent) CloseHandle(g_configChangedEvent);
	if (g_actionEvent) CloseHandle(g_actionEvent);
	g_stopEvent = g_configChangedEvent = g_actionEvent = NULL;
	DeleteCriticalSection(&g_runtimeLock);
	DeleteCriticalSection(&g_logLock);
	DeleteCriticalSection(&g_actionLock);
	DeleteCriticalSection(&g_uiLock);
	DeleteCriticalSection(&g_configLock);
	g_locksInitialized = FALSE;
	g_mainWindow = NULL;
	g_instance = NULL;
	g_appIcon = NULL;
	g_trayIconId = 0;
	g_taskDialogRunning = FALSE;
	g_alertNotificationActive = FALSE;
	g_alertTemporaryTrayIcon = FALSE;
	g_trayIconAvailable = FALSE;
	g_alertNotificationIconId = 0;
	g_alertTimerId = 0;
	g_runtimeCount = 0;
	g_sampleCycle = 0;
	ZeroMemory(g_states, sizeof(g_states));
	ZeroMemory(g_runtime, sizeof(g_runtime));
	HeapFree(GetProcessHeap(), 0, g_moduleDirectory);
	HeapFree(GetProcessHeap(), 0, g_selfPath);
	g_moduleDirectory = NULL;
	g_selfPath = NULL;
}

void ProcessMonitorOpenRulesWindow(HWND owner)
{
	if (!g_locksInitialized || !g_instance)
	{
		MessageBoxW(owner, L"进程资源监控模块当前不可用。", L"TrayS", MB_ICONWARNING | MB_OK);
		return;
	}
	if (g_rulesWindow)
	{
		ShowWindow(g_rulesWindow, SW_RESTORE);
		SetForegroundWindow(g_rulesWindow);
		return;
	}
	g_rulesWindow = CreateDialogParamW(g_instance, MAKEINTRESOURCEW(IDD_PROCESS_RULES), owner, RulesDialogProc, 0);
	if (g_rulesWindow)
	{
		ShowWindow(g_rulesWindow, SW_SHOWNORMAL);
		SetForegroundWindow(g_rulesWindow);
	}
}

void ProcessMonitorDispatchUi(BOOL trayIconAvailable)
{
	g_trayIconAvailable = trayIconAvailable;
	while (PopUiEvent(&g_dispatchEvent))
	{
		if (g_dispatchEvent.type == PM_UI_ALERT)
		{
			if (IncidentStillCurrentForUi(&g_dispatchEvent.incident))
				EnqueueAlert(&g_dispatchEvent.incident);
		}
		else if (g_dispatchEvent.type == PM_UI_CLEAR)
			RemoveAlertForIncident(&g_dispatchEvent.incident);
		else if (g_dispatchEvent.type == PM_UI_RESULT)
			ShowResultNotification(g_dispatchEvent.message, trayIconAvailable);
		else if (g_dispatchEvent.type == PM_UI_CLEAR_ALL)
		{
			ClearAllAlerts();
			EnterCriticalSection(&g_uiLock);
			DWORD remaining = g_uiCount;
			PM_UI_EVENT* retained = (PM_UI_EVENT*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
				PM_MAX_UI_EVENTS * sizeof(PM_UI_EVENT));
			if (!retained)
			{
				LeaveCriticalSection(&g_uiLock);
				FreeIncident(&g_dispatchEvent.incident);
				continue;
			}
			DWORD retainedCount = 0;
			while (remaining-- > 0)
			{
				PM_UI_EVENT* queued = &g_uiEvents[g_uiHead];
				if (queued->type == PM_UI_RESULT || queued->type == PM_UI_CLEAR_ALL)
				{
					retained[retainedCount] = *queued;
					queued->incident.path = NULL;
					++retainedCount;
				}
				else
					FreeIncident(&queued->incident);
				ZeroMemory(queued, sizeof(*queued));
				g_uiHead = (g_uiHead + 1) % PM_MAX_UI_EVENTS;
				--g_uiCount;
			}
			g_uiHead = g_uiTail = 0;
			for (DWORD index = 0; index < retainedCount; ++index)
			{
				g_uiEvents[g_uiTail] = retained[index];
				retained[index].incident.path = NULL;
				g_uiTail = (g_uiTail + 1) % PM_MAX_UI_EVENTS;
				++g_uiCount;
			}
			LeaveCriticalSection(&g_uiLock);
			HeapFree(GetProcessHeap(), 0, retained);
		}
		FreeIncident(&g_dispatchEvent.incident);
	}
}

BOOL ProcessMonitorHandleNotification(WPARAM wParam, LPARAM lParam)
{
	UNREFERENCED_PARAMETER(wParam);
	if (!g_alertNotificationActive)
		return FALSE;
	UINT notification = LOWORD(lParam);
	if (notification == NIN_BALLOONUSERCLICK)
	{
		DismissAlertNotification();
		if (!g_hasCurrentAlert)
			return TRUE;
		int selectedButton = 0;
		g_taskDialogRunning = TRUE;
		BOOL shownBySystem = ShowSystemAlert(&selectedButton);
		if (shownBySystem)
		{
			if (g_hasCurrentAlert)
				FinishCurrentAlert(selectedButton == PM_TASK_ALERT_TERMINATE);
			g_taskDialogRunning = FALSE;
			ShowNextAlert();
			return TRUE;
		}
		g_taskDialogRunning = FALSE;
		g_alertWindow = CreateDialogParamW(g_instance, MAKEINTRESOURCEW(IDD_PROCESS_ALERT), NULL, AlertDialogProc, 0);
		if (g_alertWindow)
		{
			ShowWindow(g_alertWindow, SW_SHOWNORMAL);
			SetForegroundWindow(g_alertWindow);
		}
		else
			FinishCurrentAlert(FALSE);
		return TRUE;
	}
	if (notification == NIN_BALLOONTIMEOUT || notification == NIN_BALLOONHIDE)
	{
		FinishCurrentAlert(FALSE);
		return TRUE;
	}
	return notification == NIN_BALLOONSHOW;
}

BOOL ProcessMonitorIsDialogMessage(MSG* message)
{
	if (!message)
		return FALSE;
	return (g_rulesWindow && IsDialogMessageW(g_rulesWindow, message)) ||
		(g_alertWindow && IsDialogMessageW(g_alertWindow, message)) ||
		(g_resultWindow && IsDialogMessageW(g_resultWindow, message));
}
