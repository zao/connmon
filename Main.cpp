#include <WinSock2.h>
#include <WS2tcpip.h>
#include <iphlpapi.h>
#include <Psapi.h>
#include <tlhelp32.h>
#include <Ip2string.h>
#include <shellapi.h>
#include <CommCtrl.h>
#include <strsafe.h>
#include <windowsx.h>

#include "resource.h"

#include <chrono>
#include <format>
#include <iostream>
#include <map>
#include <set>
#include <thread>
#include <vector>

#pragma comment(lib,"ws2_32.lib")
#pragma comment(lib,"iphlpapi.lib")
#pragma comment(lib,"psapi.lib")
#pragma comment(lib,"ntdll.lib")
#pragma comment(lib,"winmm.lib")
#pragma comment(lib,"Comctl32.lib")

std::set<uint16_t> ListRemotePortsForProcess(DWORD pid)
{
	std::set<uint16_t> ports;
	wchar_t localAddr[47]{}, remoteAddr[47]{};
	// IPv4
	{
		MIB_TCPTABLE2 probeTable{};
		MIB_TCPTABLE2* table = &probeTable;
		ULONG tableSize = sizeof(MIB_TCPTABLE2);

		if (DWORD rc = GetTcpTable2(table, &tableSize, FALSE); rc == ERROR_INSUFFICIENT_BUFFER) {
			table = (MIB_TCPTABLE2*)malloc(tableSize);
		}
		if (DWORD rc = GetTcpTable2(table, &tableSize, FALSE); rc == NO_ERROR) {
			for (DWORD i = 0; i < table->dwNumEntries; ++i) {
				auto& entry = table->table[i];
				if (entry.dwOwningPid == pid) {
					ports.insert(ntohs((uint16_t)entry.dwRemotePort));
					//RtlIpv4AddressToStringW((in_addr*)&entry.dwLocalAddr, localAddr);
					//RtlIpv4AddressToStringW((in_addr*)&entry.dwRemoteAddr, remoteAddr);
					//OutputDebugStringW(std::format(L"{}:{}, {}:{}\n",
					//	localAddr,
					//	ntohs((uint16_t)entry.dwLocalPort),
					//	remoteAddr,
					//	ntohs((uint16_t)entry.dwRemotePort)).c_str());
				}
			}
		}
		if (table != &probeTable) {
			free(table);
		}
	}
	// IPv6
	{
		MIB_TCP6TABLE2 probeTable{};
		MIB_TCP6TABLE2* table = &probeTable;
		ULONG tableSize = sizeof(MIB_TCP6TABLE2);

		if (DWORD rc = GetTcp6Table2(table, &tableSize, FALSE); rc == ERROR_INSUFFICIENT_BUFFER) {
			table = (MIB_TCP6TABLE2*)malloc(tableSize);
		}
		if (DWORD rc = GetTcp6Table2(table, &tableSize, FALSE); rc == NO_ERROR) {
			for (DWORD i = 0; i < table->dwNumEntries; ++i) {
				auto& entry = table->table[i];
				if (entry.dwOwningPid == pid) {
					ports.insert(ntohs((uint16_t)entry.dwRemotePort));
					//RtlIpv6AddressToStringW(&entry.LocalAddr, localAddr);
					//RtlIpv6AddressToStringW(&entry.RemoteAddr, remoteAddr);
					//OutputDebugStringW(std::format(L"{}:{}, {}:{}\n",
					//	localAddr,
					//	ntohs((uint16_t)entry.dwLocalPort),
					//	remoteAddr,
					//	ntohs((uint16_t)entry.dwRemotePort)).c_str());
				}
			}
		}
		if (table != &probeTable) {
			free(table);
		}
	}
	return ports;
}

struct ProcessMatch
{
	DWORD pid;
};

std::set<DWORD> FindMatchingProcesses(std::wstring_view processName)
{
	std::set<DWORD> matches;

	HANDLE procSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (procSnap == INVALID_HANDLE_VALUE) {
		return {};
	}

	PROCESSENTRY32W pe{ .dwSize = sizeof(pe) };
	if (Process32FirstW(procSnap, &pe)) {
		do {
			std::wstring_view name = pe.szExeFile;
			if (name == processName) {
				matches.insert(pe.th32ProcessID);
			}
		} while (Process32NextW(procSnap, &pe));
	}
	CloseHandle(procSnap);

	return matches;
}

struct GameState
{
	GameState() {}

	enum class Phase
	{
		Login,
		Gateway,
		Ingame,
	};
	Phase phase = Phase::Login;
	using Timepoint = std::chrono::steady_clock::time_point;
	std::optional<Timepoint> disconnectionTime, notificationTime;
};

std::wstring_view PhaseName(GameState::Phase phase)
{
	switch (phase) {
		using _ = GameState::Phase;
	case _::Login: return L"Login screen";
	case _::Gateway: return L"Character select";
	case _::Ingame: return L"In-game";
	default: std::unreachable();
	}
}

HINSTANCE g_hinst{};
HWND g_wnd{};

enum class IconKind
{
	Dormant,
	Offline,
	Online,
};

struct NotificationIcon
{
	NotificationIcon(HWND wnd)
		: wnd(wnd)
	{
		nid.hWnd = wnd;
		StringCchCopy(nid.szTip, _countof(nid.szTip), L"Path of Exile: dormant");
		LoadIconMetric(g_hinst, MAKEINTRESOURCE(IDI_DORMANT), LIM_SMALL, &iconDormant);
		LoadIconMetric(g_hinst, MAKEINTRESOURCE(IDI_OFFLINE), LIM_SMALL, &iconOffline);
		LoadIconMetric(g_hinst, MAKEINTRESOURCE(IDI_ONLINE), LIM_SMALL, &iconOnline);
		nid.hIcon = iconDormant;

		contextMenuParent = LoadMenuW(g_hinst, (LPCWSTR)MAKEINTRESOURCE(IDR_CTXMENU));
		menu = GetSubMenu(contextMenuParent, 0);

		HRESULT hr{};
		hr = Shell_NotifyIconW(NIM_ADD, &nid);
		hr = Shell_NotifyIconW(NIM_SETVERSION, &nid);
	}

	~NotificationIcon()
	{
		HRESULT hr{};
		hr = Shell_NotifyIconW(NIM_DELETE, &nid);
		DestroyMenu(menu);
	}

	void SetIcon(IconKind kind)
	{
		if (kind == IconKind::Dormant)
			nid.hIcon = iconDormant;
		else if (kind == IconKind::Offline)
			nid.hIcon = iconOffline;
		else
			nid.hIcon = iconOnline;
		HRESULT hr{};
		hr = Shell_NotifyIconW(NIM_MODIFY, &nid);
	}

	void HandleDisconnect()
	{
		StringCchCopy(nid.szTip, _countof(nid.szTip), L"Path of Exile: disconnected");
		StringCchCopy(nid.szInfoTitle, _countof(nid.szInfoTitle), L"Path of Exile disconnected");
		StringCchCopy(nid.szInfo, _countof(nid.szInfo), L"Path of Exile seems to have disconnected.");
		nid.uFlags |= NIF_INFO;
		HRESULT hr{};
		hr = Shell_NotifyIconW(NIM_MODIFY, &nid);
		*nid.szInfoTitle = L'\0';
		*nid.szInfo = L'\0';
		nid.uFlags &= ~NIF_INFO;
	}

	enum { TrayNotificationMessage = WM_APP };

	HMENU menu{};

private:
	HWND wnd{};
	HMENU contextMenuParent;
	HICON iconDormant, iconOffline, iconOnline;

	// {B41BC200-A022-41A5-8291-E38E02F662DE}
	static inline GUID s_notificationGuid =
	{ 0xb41bc200, 0xa022, 0x41a5, { 0x82, 0x91, 0xe3, 0x8e, 0x2, 0xf6, 0x62, 0xde } };

	NOTIFYICONDATAW nid{
		.cbSize = sizeof(nid),
		.hWnd{},
		.uFlags = NIF_ICON | NIF_TIP | NIF_GUID | NIF_MESSAGE,
		.uCallbackMessage = TrayNotificationMessage,
		.uVersion = NOTIFYICON_VERSION_4,
		.guidItem = s_notificationGuid,
	};
};

std::unique_ptr<NotificationIcon> g_notificationIcon;

void NotifyConnect()
{

}

void NotifyDisconnect()
{
	OutputDebugStringW(L"Honk!\n");
	if (g_notificationIcon) {
		g_notificationIcon->HandleDisconnect();
	}
}

struct MonitorContext
{
	std::map<DWORD, GameState> processState;
	UINT_PTR timerPtr;
	uint32_t milliInterval = 5000;

	void StartTimer();
	void StopTimer();
	void CheckGameState();
};

LRESULT WINAPI WndProc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	SetProcessDPIAware();
	if (msg == WM_NCCREATE) {
		auto cs = (CREATESTRUCT*)lparam;
		SetWindowLongPtrW(wnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
	}
	auto* ctx = (MonitorContext*)GetWindowLongPtrW(wnd, GWLP_USERDATA);

	switch (msg)
	{
	case WM_TIMER: {
		ctx->CheckGameState();
	} break;
	case NotificationIcon::TrayNotificationMessage: {
		//OutputDebugStringW(std::format(L"{} {} {} {}\n", LOWORD(lparam), HIWORD(lparam), GET_X_LPARAM(wparam), GET_Y_LPARAM(wparam)).c_str());
		if (LOWORD(lparam) == WM_CONTEXTMENU) {
			int x = GET_X_LPARAM(wparam), y = GET_Y_LPARAM(wparam);
			SetForegroundWindow(wnd);
			auto cmd = TrackPopupMenu(g_notificationIcon->menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON, x, y, 0, wnd, nullptr);
			if (cmd == ID_CONTEXT_EXIT) {
				PostQuitMessage(0);
			}
		}
	} break;
	}
	return DefWindowProcW(wnd, msg, wparam, lparam);
}

void MonitorContext::StartTimer()
{
	timerPtr = ::SetTimer(g_wnd, 42, milliInterval, nullptr);
}

void MonitorContext::StopTimer()
{
	::KillTimer(g_wnd, timerPtr);
}

void MonitorContext::CheckGameState()
{
	using namespace std::string_view_literals;
	auto matches = FindMatchingProcesses(L"PathOfExile.exe"sv);
	bool anyDisconnected = false;
	for (auto pid : matches) {
		auto& state = processState[pid];
		//OutputDebugStringW(std::format(L"pid match {}\n", pid).c_str());
		auto ports = ListRemotePortsForProcess(pid);
		// PoE1: 20481, 6112
		using Phase = GameState::Phase;
		Phase detectedPhase = Phase::Login;
		if (ports.count(20481) || ports.count(20471)) {
			detectedPhase = Phase::Gateway;
		}
		if (ports.count(6112) || ports.count(6103)) {
			detectedPhase = Phase::Ingame;
		}

		if (state.phase != detectedPhase) {
			OutputDebugStringW(std::format(L"Process {} changed from {} to {}\n",
				pid, PhaseName(state.phase), PhaseName(detectedPhase)).c_str());
			state.phase = detectedPhase;
			if (detectedPhase == Phase::Login) {
				state.disconnectionTime = std::chrono::steady_clock::now();
			}
			else {
				state.disconnectionTime.reset();
			}
			state.notificationTime.reset();
		}
		
		if (state.disconnectionTime) {
			anyDisconnected = true;
		}
	}

	std::optional<IconKind> iconChange;
	if (matches.empty()) {
		iconChange = IconKind::Dormant;
	}
	else if (anyDisconnected) {
		iconChange = IconKind::Offline;
	}
	else {
		iconChange = IconKind::Online;
	}

	if (iconChange) {
		g_notificationIcon->SetIcon(*iconChange);
	}

	using namespace std::chrono_literals;
	auto now = std::chrono::steady_clock::now();
	std::set<DWORD> staleProcesses;
	for (auto& [pid, state] : processState) {
		if (matches.count(pid) == 0) {
			// Two-phase culling of process state to avoid modifying while iterating.
			staleProcesses.insert(pid);
		}
		else {
			constexpr auto DisconnectionTimeout = 9.0s;
			if (state.disconnectionTime && !state.notificationTime && now - *state.disconnectionTime > DisconnectionTimeout) {
				NotifyDisconnect();
				state.notificationTime = now;
			}
		}
	}
	for (auto pid : staleProcesses) {
		processState.erase(pid);
	}
}

int WINAPI WinMain(HINSTANCE hinst, HINSTANCE, LPSTR, int)
{
	MonitorContext ctx{};

	g_hinst = hinst;
	InitCommonControls();

	WNDCLASSEX wcex{
		.cbSize = sizeof(wcex),
		.lpfnWndProc = &WndProc,
		.hInstance = hinst,
		.lpszClassName = L"connmon",
	};
	RegisterClassExW(&wcex);
	g_wnd = CreateWindowExW(0, L"connmon", L"connmon", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, &ctx);
	g_notificationIcon = std::make_unique<NotificationIcon>(g_wnd);
	g_notificationIcon->SetIcon(IconKind::Dormant);

	ctx.StartTimer();

	while (true) {
		MSG msg{};
		GetMessage(&msg, 0, 0, 0);
		if (msg.message == WM_QUIT) {
			break;
		}
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	return 0;
}