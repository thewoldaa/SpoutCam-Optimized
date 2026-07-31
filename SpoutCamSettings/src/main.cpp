//
// SpoutCam settings panel
//
// Settings panel for the SpoutCam DirectShow filter. The filter itself reads
// nothing but the registry, so this program is only a front end for a handful
// of values under HKCU\Software\Leading Edge\SpoutCam, plus a live preview of
// the sender and a button to register the filter.
//
// The interface is HTML in a WebView2 control. The preview is a plain child
// window placed on top of a slot in the page - pumping video frames through
// the JavaScript bridge would cost far more than drawing them directly.
//

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl.h>
#include <string>
#include <vector>
#include "WebView2.h"
#include "preview.h"
#include "resource.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using namespace Microsoft::WRL;

static const wchar_t* kRegPath = L"Software\\Leading Edge\\SpoutCam";
static const wchar_t* kWndClass = L"SpoutCamSettingsMain";
static const UINT_PTR kTimerId = 1;

// The filter watches for this event and shows its name plate when it is gone,
// so quitting here stops the camera the way quitting OBS stops its virtual
// camera. Minimising to the tray keeps it alive without keeping it in the way.
static const wchar_t* kRunEventName = L"SpoutCamSettingsRunning";
static HANDLE g_hRunEvent = nullptr;

static const UINT kTrayMsg = WM_APP+1;
static const UINT kTrayIconId = 1;
static const UINT kMenuShow = 100;
static const UINT kMenuExit = 101;
static NOTIFYICONDATAW g_tray = {};

static HWND  g_hMain = nullptr;
static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2>           g_webview;
static SpoutPreview g_preview;
static bool g_bReadyForScript = false;
static std::wstring g_lastTally;

// ---------------------------------------------------------------- registry

static DWORD ReadDword(const wchar_t* name, DWORD fallback)
{
	DWORD value = 0, size = sizeof(value), type = 0;
	if (RegGetValueW(HKEY_CURRENT_USER, kRegPath, name, RRF_RT_REG_DWORD,
			&type, &value, &size) == ERROR_SUCCESS)
		return value;
	return fallback;
}

static std::wstring ReadString(const wchar_t* name)
{
	wchar_t buf[512] = {};
	DWORD size = sizeof(buf);
	if (RegGetValueW(HKEY_CURRENT_USER, kRegPath, name, RRF_RT_REG_SZ,
			nullptr, buf, &size) == ERROR_SUCCESS)
		return buf;
	return L"";
}

static bool WriteDword(const wchar_t* name, DWORD value)
{
	HKEY key = nullptr;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0,
			KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
		return false;
	const LONG r = RegSetValueExW(key, name, 0, REG_DWORD,
		(const BYTE*)&value, sizeof(value));
	RegCloseKey(key);
	return r == ERROR_SUCCESS;
}

static bool WriteString(const wchar_t* name, const std::wstring& value)
{
	HKEY key = nullptr;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0,
			KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
		return false;
	const LONG r = RegSetValueExW(key, name, 0, REG_SZ,
		(const BYTE*)value.c_str(), (DWORD)((value.size()+1)*sizeof(wchar_t)));
	RegCloseKey(key);
	return r == ERROR_SUCCESS;
}

// ---------------------------------------------------------------- text bits

static std::wstring Widen(const char* utf8, int bytes)
{
	if (bytes <= 0) return L"";
	const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, bytes, nullptr, 0);
	std::wstring out((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8, bytes, out.data(), n);
	return out;
}

static std::string Narrow(const std::wstring& w)
{
	if (w.empty()) return "";
	const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
		nullptr, 0, nullptr, nullptr);
	std::string out((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
		out.data(), n, nullptr, nullptr);
	return out;
}

//
// The page sends flat objects of numbers and short strings, so picking values
// out by name is enough and saves carrying a JSON parser for six fields.
//
static bool JsonNumber(const std::wstring& json, const wchar_t* key, double& out)
{
	std::wstring needle = L"\"";
	needle += key;
	needle += L"\":";
	const size_t at = json.find(needle);
	if (at == std::wstring::npos) return false;
	const wchar_t* start = json.c_str()+at+needle.size();
	wchar_t* end = nullptr;
	const double v = wcstod(start, &end);
	if (end == start) return false;
	out = v;
	return true;
}

static int JsonInt(const std::wstring& json, const wchar_t* key, int fallback)
{
	double v = 0;
	return JsonNumber(json, key, v) ? (int)v : fallback;
}

static bool JsonBool(const std::wstring& json, const wchar_t* key)
{
	std::wstring needle = L"\"";
	needle += key;
	needle += L"\":";
	const size_t at = json.find(needle);
	if (at == std::wstring::npos) return false;
	return json.compare(at+needle.size(), 4, L"true") == 0
		|| json.compare(at+needle.size(), 1, L"1") == 0;
}

static std::wstring JsonString(const std::wstring& json, const wchar_t* key)
{
	std::wstring needle = L"\"";
	needle += key;
	needle += L"\":\"";
	const size_t at = json.find(needle);
	if (at == std::wstring::npos) return L"";
	size_t i = at+needle.size();
	std::wstring out;
	while (i < json.size() && json[i] != L'"') {
		if (json[i] == L'\\' && i+1 < json.size()) i++;
		out += json[i++];
	}
	return out;
}

static std::wstring JsonEscape(const std::wstring& in)
{
	std::wstring out;
	for (wchar_t c : in) {
		if (c == L'"' || c == L'\\') { out += L'\\'; out += c; }
		else if (c == L'\n' || c == L'\r') out += L' ';
		else out += c;
	}
	return out;
}

// ---------------------------------------------------------------- filter

//
// Look for the filter next to this program first, then in the layout the
// SpoutCam repository produces. Registration needs administrator rights, so
// it is handed to regsvr32 through the elevation prompt rather than attempted
// in process.
//
static std::wstring FindFilterFor(int bits)
{
	wchar_t exe[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	std::wstring dir = exe;
	dir.resize(dir.find_last_of(L'\\')+1);

	const wchar_t* name = bits == 64 ? L"SpoutCam64.ax" : L"SpoutCam32.ax";
	const wchar_t* sub  = bits == 64
		? L"binaries\\SPOUTCAM\\SpoutCam64\\SpoutCam64.ax"
		: L"binaries\\SPOUTCAM\\SpoutCam32\\SpoutCam32.ax";

	// Next to the program first, for a folder that has simply been copied
	// somewhere, then the flat layout an install produces. The rest walk back
	// out of build\<arch> to the repository layout, where the filter lives in a
	// sibling SpoutCam folder.
	const std::wstring up[] = { L"", L"..\\", L"..\\..\\", L"..\\..\\..\\", L"..\\..\\..\\..\\" };

	std::vector<std::wstring> candidates;
	candidates.push_back(dir+L"filter\\"+name);
	candidates.push_back(dir+name);
	for (const auto& u : up) {
		candidates.push_back(dir+u+sub);
		candidates.push_back(dir+u+L"SpoutCam\\"+sub);
	}

	for (const auto& path : candidates) {
		if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
			return path;
	}
	return L"";
}

// The filter matching this build, which is the one that has to be present
static std::wstring FindFilter()
{
#ifdef _WIN64
	return FindFilterFor(64);
#else
	return FindFilterFor(32);
#endif
}


// The filter's own class id, as declared in the SpoutCam sources
static const wchar_t* kFilterClsid =
	L"SOFTWARE\\Classes\\CLSID\\{8E14549A-DB61-4309-AFA1-3578E927E933}\\InprocServer32";

//
// Where Windows currently thinks the camera lives, if anywhere.
// Empty means nothing is registered.
//
static std::wstring RegisteredFilterPath()
{
	wchar_t buf[MAX_PATH] = {};
	DWORD size = sizeof(buf);
	if (RegGetValueW(HKEY_LOCAL_MACHINE, kFilterClsid, nullptr,
			RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
		return buf;
	return L"";
}

static bool SamePath(const std::wstring& a, const std::wstring& b)
{
	if (a.size() != b.size())
		return false;
	return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

//
// Tell the page whether the camera is installed, so it can offer the right
// action instead of a button that means nothing until it is pressed.
//
static void PushCameraStatus()
{
	if (!g_webview || !g_bReadyForScript)
		return;

	const std::wstring mine = FindFilter();
	const std::wstring live = RegisteredFilterPath();

	const wchar_t* state = L"missing";   // no filter file to install
	if (!mine.empty()) {
		if (live.empty())            state = L"none";    // nothing registered
		else if (SamePath(mine, live)) state = L"ours";  // this build is live
		else                          state = L"other";  // a different copy is live
	}

	std::wstring script = L"window.setCameraStatus(\"";
	script += state;
	script += L"\",\"";
	script += JsonEscape(live);
	script += L"\");";

	g_webview->ExecuteScript(script.c_str(), nullptr);
}

// ---------------------------------------------------------------- installer
//
// The program installs itself rather than shipping a separate installer.
// Everything here runs in a second, elevated copy of this same executable,
// started with /install or /uninstall, so nothing the user sees is a console
// window. A DirectShow filter has to be registered machine wide, which is
// where the one elevation prompt comes from.
//

static const wchar_t* kUninstallKey =
	L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\SpoutCam";

static std::wstring SelfPath()
{
	wchar_t buf[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, buf, MAX_PATH);
	return buf;
}

static std::wstring EnvPath(const wchar_t* name)
{
	wchar_t buf[MAX_PATH] = {};
	GetEnvironmentVariableW(name, buf, MAX_PATH);
	return buf;
}

static std::wstring InstallDir()
{
	// ProgramW6432 is the 64 bit folder even when a 32 bit build reads it,
	// which keeps both builds installing to the same place
	std::wstring root = EnvPath(L"ProgramW6432");
	if (root.empty()) root = EnvPath(L"ProgramFiles");
	if (root.empty()) root = L"C:\\Program Files";
	return root + L"\\SpoutCam";
}

static std::wstring ShortcutPath()
{
	std::wstring data = EnvPath(L"ProgramData");
	if (data.empty()) data = L"C:\\ProgramData";
	return data + L"\\Microsoft\\Windows\\Start Menu\\Programs\\SpoutCam.lnk";
}

static bool IsInstalledCopy()
{
	const std::wstring self = SelfPath();
	const std::wstring dest = InstallDir() + L"\\SpoutCam.exe";
	return _wcsicmp(self.c_str(), dest.c_str()) == 0;
}

//
// Run a command with no window at all. ShellExecute with SW_HIDE still lets
// a console flash on some machines, CREATE_NO_WINDOW does not.
//
static bool RunHidden(const std::wstring& cmdline, DWORD waitMs = 30000)
{
	std::wstring buf = cmdline; // CreateProcess writes to this
	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi = {};

	if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
			CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
		return false;

	WaitForSingleObject(pi.hProcess, waitMs);
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return code == 0;
}

// Same, but without waiting. Used when the thing being started is waiting for
// this process to exit.
static bool RunDetached(const std::wstring& cmdline)
{
	std::wstring buf = cmdline;
	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi = {};

	if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
			CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi))
		return false;

	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return true;
}

static bool Regsvr(const std::wstring& ax, bool bUnregister, bool b32)
{
	// A 64 bit regsvr32 cannot load a 32 bit filter, so the WOW64 copy
	// registers that one
	std::wstring exe = EnvPath(L"SystemRoot");
	if (exe.empty()) exe = L"C:\\Windows";
	exe += b32 ? L"\\SysWOW64\\regsvr32.exe" : L"\\System32\\regsvr32.exe";

	if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
		exe = L"regsvr32.exe"; // let the search path find it

	std::wstring cmd = L"\"" + exe + L"\" /s ";
	if (bUnregister) cmd += L"/u ";
	cmd += L"\"" + ax + L"\"";
	return RunHidden(cmd);
}

//
// Copy over a file that something may still be holding open.
//
// A DLL that any program has loaded cannot be overwritten, and enumerating
// cameras is enough to load this one, so a browser left running is enough to
// block a reinstall. Windows will not let the file be written, but it will let
// it be renamed out of the way, which leaves the loaded copy running in the
// processes that have it and the new one on disk for everything after.
//
static bool CopyOver(const std::wstring& src, const std::wstring& dst)
{
	if (CopyFileW(src.c_str(), dst.c_str(), FALSE))
		return true;

	// A fixed name would collide with the leftover from a previous install
	// that something is still holding, and renaming onto a held file fails
	// just as writing over one does. Count up until a free name turns up.
	std::wstring stale;
	for (int n = 0; n < 100; n++) {
		wchar_t suffix[16];
		swprintf_s(suffix, L".old%d", n);
		stale = dst + suffix;
		if (GetFileAttributesW(stale.c_str()) == INVALID_FILE_ATTRIBUTES)
			break;
		DeleteFileW(stale.c_str()); // gone if nothing holds it, in use if not
		if (GetFileAttributesW(stale.c_str()) == INVALID_FILE_ATTRIBUTES)
			break;
		stale.clear();
	}
	if (stale.empty())
		return false;

	if (!MoveFileExW(dst.c_str(), stale.c_str(), 0))
		return false;

	// Cleared on the next restart, by which point nothing is holding it
	MoveFileExW(stale.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);

	return CopyFileW(src.c_str(), dst.c_str(), FALSE) != FALSE;
}

//
// Left behind for the unelevated half to read back. An install that fails with
// nothing but a number is not worth reporting at all.
//
static void SetInstallError(const wchar_t* what)
{
	wchar_t buf[512];
	swprintf_s(buf, L"%s (Windows error %lu)", what, GetLastError());
	WriteString(L"lastinstallerror", buf);
}

static bool MakeShortcut(const std::wstring& link, const std::wstring& target)
{
	IShellLinkW* sl = nullptr;
	if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
			IID_IShellLinkW, (void**)&sl)) || !sl)
		return false;

	sl->SetPath(target.c_str());
	sl->SetDescription(L"Send a Spout sender to a virtual webcam");
	std::wstring dir = target.substr(0, target.find_last_of(L'\\'));
	sl->SetWorkingDirectory(dir.c_str());

	bool ok = false;
	IPersistFile* pf = nullptr;
	if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf)) && pf) {
		ok = SUCCEEDED(pf->Save(link.c_str(), TRUE));
		pf->Release();
	}
	sl->Release();
	return ok;
}

static void WriteMachineString(const wchar_t* name, const std::wstring& value)
{
	HKEY key = nullptr;
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, nullptr, 0,
			KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
		return;
	RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)value.c_str(),
		(DWORD)((value.size()+1)*sizeof(wchar_t)));
	RegCloseKey(key);
}

static void WriteMachineDword(const wchar_t* name, DWORD value)
{
	HKEY key = nullptr;
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, nullptr, 0,
			KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
		return;
	RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
	RegCloseKey(key);
}

//
// Copy the program and its filter somewhere permanent, then register from
// there. Registering in place is the trap worth avoiding: the registry records
// the path of the .ax file, so a downloads folder that gets tidied away later
// leaves a camera Windows cannot load.
//
static int DoInstall()
{
	const std::wstring dest   = InstallDir();
	const std::wstring filter = dest + L"\\filter";
	const std::wstring ax64   = FindFilterFor(64);
	const std::wstring ax32   = FindFilterFor(32);

	if (ax64.empty()) {
		WriteString(L"lastinstallerror", L"SpoutCam64.ax was not found next to the program");
		return 2;
	}

	// Whatever is registered now goes first, wherever it came from, or Windows
	// would keep loading that copy in preference to this one
	const std::wstring live = RegisteredFilterPath();
	if (!live.empty())
		Regsvr(live, true, false);

	CreateDirectoryW(dest.c_str(), nullptr);
	CreateDirectoryW(filter.c_str(), nullptr);

	const std::wstring self = SelfPath();
	const std::wstring exe  = dest + L"\\SpoutCam.exe";
	if (_wcsicmp(self.c_str(), exe.c_str()) != 0) {
		if (!CopyOver(self, exe)) {
			SetInstallError(L"could not write SpoutCam.exe");
			return 3;
		}
	}

	if (!CopyOver(ax64, filter + L"\\SpoutCam64.ax")) {
		SetInstallError(L"could not write SpoutCam64.ax");
		return 3;
	}
	if (!ax32.empty())
		CopyOver(ax32, filter + L"\\SpoutCam32.ax");

	if (!Regsvr(filter + L"\\SpoutCam64.ax", false, false)) {
		SetInstallError(L"regsvr32 could not register the camera");
		return 4;
	}

	// 32 bit hosts load their own build. Not every download carries one, and
	// failing to register it is not a reason to fail the install.
	if (!ax32.empty())
		Regsvr(filter + L"\\SpoutCam32.ax", false, true);

	MakeShortcut(ShortcutPath(), exe);

	WriteMachineString(L"DisplayName",     L"SpoutCam");
	WriteMachineString(L"DisplayVersion",  L"1.0.0");
	WriteMachineString(L"Publisher",       L"SpoutCam contributors");
	WriteMachineString(L"InstallLocation", dest);
	WriteMachineString(L"DisplayIcon",     exe);
	WriteMachineString(L"UninstallString", L"\"" + exe + L"\" /uninstall");
	WriteMachineDword (L"NoModify", 1);
	WriteMachineDword (L"NoRepair", 1);

	return 0;
}

static int DoUninstall()
{
	const std::wstring dest   = InstallDir();
	const std::wstring filter = dest + L"\\filter";

	Regsvr(filter + L"\\SpoutCam64.ax", true, false);
	if (GetFileAttributesW((filter + L"\\SpoutCam32.ax").c_str()) != INVALID_FILE_ATTRIBUTES)
		Regsvr(filter + L"\\SpoutCam32.ax", true, true);

	// Anything still registered, in case it was not the copy installed here
	const std::wstring live = RegisteredFilterPath();
	if (!live.empty())
		Regsvr(live, true, false);

	DeleteFileW(ShortcutPath().c_str());
	RegDeleteKeyW(HKEY_LOCAL_MACHINE, kUninstallKey);

	DeleteFileW((filter + L"\\SpoutCam64.ax").c_str());
	DeleteFileW((filter + L"\\SpoutCam32.ax").c_str());

	// Left by a reinstall that had to rename a loaded filter out of the way.
	// Still held, most likely, so schedule it rather than expecting it to go.
	// They are numbered, and there may be several, so match rather than guess
	WIN32_FIND_DATAW found = {};
	HANDLE search = FindFirstFileW((filter + L"\\*.ax.old*").c_str(), &found);
	if (search != INVALID_HANDLE_VALUE) {
		do {
			const std::wstring path = filter + L"\\" + found.cFileName;
			SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
			if (!DeleteFileW(path.c_str()))
				MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
		} while (FindNextFileW(search, &found));
		FindClose(search);
	}

	RemoveDirectoryW(filter.c_str());

	// The copy being uninstalled is usually the one that started this, and
	// Windows holds an executable open until its process has fully gone. Keep
	// trying for a few seconds rather than leaving the file behind.
	const std::wstring exe = dest + L"\\SpoutCam.exe";
	for (int i = 0; i < 40; i++) {
		if (DeleteFileW(exe.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND)
			break;
		Sleep(250);
	}
	RemoveDirectoryW(dest.c_str());

	// This build is running from the temporary folder, so it cannot delete
	// itself either. Hand that to the next restart.
	MoveFileExW(SelfPath().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);

	// Settings under HKCU are left alone, so reinstalling keeps them
	return 0;
}

//
// Ask for the elevation this needs, by starting the same executable again with
// the work to do. Returns false if the user declined the prompt.
//
static bool RunElevated(const wchar_t* verb)
{
	SHELLEXECUTEINFOW sei = {};
	sei.cbSize       = sizeof(sei);
	sei.fMask        = SEE_MASK_NOCLOSEPROCESS;
	sei.hwnd         = g_hMain;
	sei.lpVerb       = L"runas";
	const std::wstring self = SelfPath();
	sei.lpFile       = self.c_str();
	sei.lpParameters = verb;
	sei.nShow        = SW_HIDE;

	if (!ShellExecuteExW(&sei))
		return false;

	DWORD code = 1;
	if (sei.hProcess) {
		WaitForSingleObject(sei.hProcess, 120000);
		GetExitCodeProcess(sei.hProcess, &code);
		CloseHandle(sei.hProcess);
	}
	return code == 0;
}

static void InstallOrRemove(bool bRemove)
{
	if (!bRemove && FindFilterFor(64).empty()) {
		MessageBoxW(g_hMain,
			L"Could not find SpoutCam64.ax.\n\n"
			L"It should sit in a SpoutCam folder next to this program. If this "
			L"came from a release download, unzip the whole thing and run it "
			L"from there rather than moving the program out on its own.",
			L"SpoutCam", MB_OK | MB_ICONWARNING);
		return;
	}

	WriteString(L"lastinstallerror", L"");
	const bool ok = RunElevated(bRemove ? L"/uninstall" : L"/install");

	// Removing can finish in a detached process, so give it a moment before
	// asking the registry what is installed
	if (ok && bRemove)
		Sleep(1500);

	if (ok) {
		MessageBoxW(g_hMain,
			bRemove
				? L"SpoutCam has been removed.\n\n"
				  L"Close and reopen any program that was using the camera."
				: L"SpoutCam is installed.\n\n"
				  L"It is in the Start menu, and the camera appears as "
				  L"\"SpoutCam\". Programs that were already open need "
				  L"restarting before they will see it.",
			L"SpoutCam", MB_OK | MB_ICONINFORMATION);
	}
	else if (!bRemove) {
		// Whatever went wrong knows more than this side of the elevation does,
		// so it leaves a note rather than making the user guess
		std::wstring why = ReadString(L"lastinstallerror");
		std::wstring text = L"Could not install SpoutCam.\n\n";
		if (!why.empty())
			text += why + L"\n\n";
		text += L"If a program is using the camera, close it and try again.";

		MessageBoxW(g_hMain, text.c_str(), L"SpoutCam", MB_OK | MB_ICONERROR);
	}
	else {
		MessageBoxW(g_hMain,
			bRemove
				? L"Could not remove SpoutCam.\n\n"
				  L"If a program is using the camera, close it and try again."
				: L"Could not install SpoutCam.\n\n"
				  L"If a program is using the camera, close it and try again.",
			L"SpoutCam", MB_OK | MB_ICONERROR);
	}

	PushCameraStatus();
}

// ---------------------------------------------------------------- bridge

static void PushSettingsToPage()
{
	if (!g_webview) return;

	wchar_t buf[1024];
	swprintf_s(buf,
		L"window.applySettings({fps:%u,res:%u,sender:\"%s\",mirror:%u,flip:%u,"
		L"swap:%u,keyon:%u,keyrgb:%u,hard:%u,thr:%u,preview:%u});",
		ReadDword(L"fps", 3),
		ReadDword(L"resolution", 0),
		JsonEscape(ReadString(L"senderstart")).c_str(),
		ReadDword(L"mirror", 0),
		ReadDword(L"flip", 0),
		ReadDword(L"swap", 0),
		ReadDword(L"keycolour", 0),
		ReadDword(L"keyrgb", 0x0000FF00),
		ReadDword(L"keyhardedge", 0),
		ReadDword(L"keythreshold", 128),
		ReadDword(L"previewopen", 0));

	g_webview->ExecuteScript(buf, nullptr);
}

static void SaveSettings(const std::wstring& json)
{
	WriteDword(L"fps",        (DWORD)JsonInt(json, L"fps", 3));
	WriteDword(L"resolution", (DWORD)JsonInt(json, L"res", 0));
	WriteDword(L"mirror",     (DWORD)JsonInt(json, L"mirror", 0));
	WriteDword(L"flip",       (DWORD)JsonInt(json, L"flip", 0));
	WriteDword(L"swap",       (DWORD)JsonInt(json, L"swap", 0));

	WriteDword(L"keycolour",    (DWORD)JsonInt(json, L"keyon", 0));
	WriteDword(L"keyrgb",       (DWORD)JsonInt(json, L"keyrgb", 0x0000FF00));
	WriteDword(L"keyhardedge",  (DWORD)JsonInt(json, L"hard", 0));
	WriteDword(L"keythreshold", (DWORD)JsonInt(json, L"thr", 128));

	// Not read by the filter - only so the panel comes back as it was left
	WriteDword(L"previewopen", (DWORD)JsonInt(json, L"preview", 0));

	WriteString(L"senderstart", JsonString(json, L"sender"));
}

//
// Height the page gains when the preview slot opens: the slot itself plus the
// margin under it. Kept in step with #preview-slot in ui.html.
//
static const int kPreviewGrowDip = 162;
static bool g_bGrown = false;

static void GrowForPreview(bool bGrow)
{
	if (bGrow == g_bGrown || !g_hMain)
		return;
	g_bGrown = bGrow;

	const int delta = MulDiv(kPreviewGrowDip, GetDpiForWindow(g_hMain), 96);

	RECT rc = {};
	GetWindowRect(g_hMain, &rc);
	SetWindowPos(g_hMain, nullptr, 0, 0,
		rc.right-rc.left,
		(rc.bottom-rc.top) + (bGrow ? delta : -delta),
		SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

	// The page sees the resize and reports the slot position again, so the
	// preview window lands in the right place without being told twice here.
}

static void HandleMessage(const std::wstring& json)
{
	const std::wstring type = JsonString(json, L"type");

	if (type == L"ready") {
		g_bReadyForScript = true;
		PushSettingsToPage();
		PushCameraStatus();
	}
	else if (type == L"save") {
		// The page sends this whenever a control changes, so there is nothing
		// to confirm and nothing to lose by closing the window.
		SaveSettings(json);
	}
	else if (type == L"register") {
		InstallOrRemove(false);
	}
	else if (type == L"unregister") {
		InstallOrRemove(true);
	}
	else if (type == L"orient") {
		g_preview.SetOrientation(
			JsonInt(json, L"mirror", 0) != 0,
			JsonInt(json, L"flip", 0) != 0,
			JsonInt(json, L"swap", 0) != 0);

		const int rgb = JsonInt(json, L"keyrgb", 0x0000FF00);
		int thr = JsonInt(json, L"thr", 128);
		if (thr < 0)   thr = 0;
		if (thr > 255) thr = 255;

		g_preview.SetKey(
			JsonInt(json, L"keyon", 0) != 0,
			(unsigned char)((rgb >> 16) & 0xFF),
			(unsigned char)((rgb >> 8) & 0xFF),
			(unsigned char)(rgb & 0xFF),
			JsonInt(json, L"hard", 0) != 0,
			(unsigned char)thr);
	}
	else if (type == L"preview") {
		// Grow the window to make room rather than letting the panel scroll.
		// The preview is a separate window and would not be clipped by the
		// page, so a scrolled slot would leave it floating over the buttons.
		GrowForPreview(JsonBool(json, L"show"));

		double dpr = 1.0;
		JsonNumber(json, L"dpr", dpr);
		if (dpr < 0.5) dpr = 1.0;

		const bool show = JsonBool(json, L"show");
		g_preview.SetRect(
			(int)(JsonInt(json, L"x", 0)*dpr),
			(int)(JsonInt(json, L"y", 0)*dpr),
			(int)(JsonInt(json, L"w", 0)*dpr),
			(int)(JsonInt(json, L"h", 0)*dpr),
			show);
	}
}

//
// Keep the header lamp in step with the preview. Only pushed when the text
// actually changes, so the page is not scripted 30 times a second.
//
static void UpdateTally()
{
	if (!g_webview || !g_bReadyForScript)
		return;

	std::wstring text;
	bool live = false;

	if (!g_preview.IsVisible()) {
		// The lamp reports the sender, not the preview, so collapsing the panel
		// must not make it look as though nothing is running. Reading the
		// sender's description costs nothing - no texture and no DirectX.
		unsigned int w = 0, h = 0;
		if (g_preview.ProbeSender(w, h)) {
			live = true;
			wchar_t buf[128];
			swprintf_s(buf, L"%ux%u", w, h);
			text = buf;
		}
		else {
			text = L"no signal";
		}
	}
	else if (!g_preview.IsConnected()) {
		text = L"no signal";
	}
	else {
		live = true;
		wchar_t buf[128];
		swprintf_s(buf, L"%ux%u%s",
			g_preview.SenderWidth(), g_preview.SenderHeight(),
			g_preview.HasAlpha() ? L" \x2022 alpha" : L"");
		text = buf;
	}

	if (text == g_lastTally)
		return;
	g_lastTally = text;

	wchar_t script[256];
	swprintf_s(script, L"window.setTally(%s,\"%s\");",
		live ? L"true" : L"false", text.c_str());
	g_webview->ExecuteScript(script, nullptr);
}

//
// What the camera is actually managing, next to the setting that asked for it.
//
// The frame rate control states an intention and nothing more. DirectShow
// fixes the rate when the pins connect, so a change needs the source removed
// and added again; a sender running slower caps it regardless; and a machine
// that cannot convert a frame inside the frame time quietly produces fewer.
// All three look the same from the outside, which is why this is measured.
//
static std::wstring g_lastRate;

static void UpdateRate()
{
	if (!g_webview || !g_bReadyForScript)
		return;

	// Index of the frame rate control, in the order the page lists them
	static const int kFpsChoices[] = { 10, 15, 25, 30, 50, 60 };
	const DWORD sel = ReadDword(L"fps", 3);
	const int wanted = kFpsChoices[sel < 6 ? sel : 3];

	const DWORD stamp = ReadDword(L"ratestamp", 0);
	const DWORD camfps = ReadDword(L"camfps", 0);     // tenths

	// The sender's rate from this program's own receiver where it can be had,
	// falling back to the filter's reading. Preferring the local one means the
	// sender can be checked before any streaming program is even started,
	// which is when it is most worth knowing.
	double sender = g_preview.SenderFps();
	if (sender <= 0.0)
		sender = ReadDword(L"senderfps", 0)/10.0;

	std::wstring text;
	bool warn = false;
	wchar_t buf[160];

	// The filter only writes its numbers while a host has the camera open, so
	// a stale stamp means nothing is running rather than nothing is arriving.
	// GetTickCount rather than timeGetTime only to avoid pulling in winmm for
	// one call. Both count milliseconds since boot and agree far more closely
	// than the three seconds being tested for.
	const bool camlive = (stamp != 0 && (GetTickCount() - stamp) <= 3000);

	if (!camlive && sender > 0.0) {
		swprintf_s(buf, L"Sender %.1f fps \x2022 no program has the camera open", sender);
		text = buf;
	}
	else if (!camlive) {
		text = L"No program has the camera open";
	}
	else {
		if (sender > 0.0)
			swprintf_s(buf, L"Sender %.1f fps \x2022 camera %.1f fps", sender, camfps/10.0);
		else
			swprintf_s(buf, L"Camera %.1f fps \x2022 no sender", camfps/10.0);
		text = buf;

		// A tenth of a frame either way is measurement noise. Ten percent down
		// is not, and it is the case worth pointing at.
		warn = (camfps < (DWORD)(wanted*9));
	}

	if (text == g_lastRate)
		return;
	g_lastRate = text;

	wchar_t script[320];
	swprintf_s(script, L"window.setRate(\"%s\",%s);",
		text.c_str(), warn ? L"true" : L"false");
	g_webview->ExecuteScript(script, nullptr);
}

// ---------------------------------------------------------------- webview

static std::wstring LoadHtmlResource()
{
	HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_UI_HTML), RT_RCDATA);
	if (!res) return L"<h3>UI resource missing</h3>";
	HGLOBAL block = LoadResource(nullptr, res);
	if (!block) return L"<h3>UI resource missing</h3>";
	const char* data = (const char*)LockResource(block);
	const DWORD size = SizeofResource(nullptr, res);
	return Widen(data, (int)size);
}

//
// WebView2 keeps a browser profile on disk. Left to itself it puts one beside
// the executable, which drops a large cache folder into whatever directory the
// program was run from. Keep it under the user's local app data instead.
//
static std::wstring UserDataFolder()
{
	wchar_t local[MAX_PATH] = {};
	if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH))
		return L""; // let WebView2 fall back to its own default

	std::wstring path = local;
	path += L"\\SpoutCam";
	CreateDirectoryW(path.c_str(), nullptr); // fails harmlessly if it exists
	return path;
}

static void CreateWebView(HWND hWnd)
{
	const std::wstring userData = UserDataFolder();

	CreateCoreWebView2EnvironmentWithOptions(nullptr,
		userData.empty() ? nullptr : userData.c_str(), nullptr,
		Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
			[hWnd](HRESULT, ICoreWebView2Environment* env) -> HRESULT {
				if (!env) return S_OK;

				env->CreateCoreWebView2Controller(hWnd,
					Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
						[hWnd](HRESULT, ICoreWebView2Controller* controller) -> HRESULT {
							if (!controller) return S_OK;

							g_controller = controller;
							g_controller->get_CoreWebView2(&g_webview);

							ICoreWebView2Settings* settings = nullptr;
							if (SUCCEEDED(g_webview->get_Settings(&settings)) && settings) {
								settings->put_AreDefaultContextMenusEnabled(FALSE);
								settings->put_IsStatusBarEnabled(FALSE);
								settings->put_AreDevToolsEnabled(FALSE);
								settings->put_IsZoomControlEnabled(FALSE);
								settings->Release();
							}

							RECT rc;
							GetClientRect(hWnd, &rc);
							g_controller->put_Bounds(rc);

							EventRegistrationToken token;
							g_webview->add_WebMessageReceived(
								Callback<ICoreWebView2WebMessageReceivedEventHandler>(
									[](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
										LPWSTR raw = nullptr;
										if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
											HandleMessage(raw);
											CoTaskMemFree(raw);
										}
										return S_OK;
									}).Get(), &token);

							g_webview->NavigateToString(LoadHtmlResource().c_str());
							return S_OK;
						}).Get());
				return S_OK;
			}).Get());
}

// ---------------------------------------------------------------- tray

static void AddTrayIcon(HWND hWnd, HINSTANCE hInst)
{
	g_tray.cbSize           = sizeof(g_tray);
	g_tray.hWnd             = hWnd;
	g_tray.uID              = kTrayIconId;
	g_tray.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	g_tray.uCallbackMessage = kTrayMsg;

	// Ask for the small size rather than letting LoadIcon hand back the 32
	// pixel frame for Windows to squash
	g_tray.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
		GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);

	wcscpy_s(g_tray.szTip, L"SpoutCam");
	Shell_NotifyIconW(NIM_ADD, &g_tray);
}

//
// A window that vanishes with no explanation looks like a crash. Said once and
// then remembered, because it stops being news after the first time.
//
static void ExplainTray()
{
	if (ReadDword(L"trayhint", 0))
		return;
	WriteDword(L"trayhint", 1);

	MessageBoxW(nullptr,
		L"SpoutCam is still running, down in the notification area "
		L"beside the clock.\n\n"
		L"The camera keeps working while it sits there. Click the icon to bring "
		L"this window back, or right click it and choose Quit to stop the camera.\n\n"
		L"This is only said once.",
		L"Minimised to the notification area", MB_OK | MB_ICONINFORMATION);
}

static void ShowPanel(HWND hWnd)
{
	ShowWindow(hWnd, SW_SHOW);
	if (IsIconic(hWnd))
		ShowWindow(hWnd, SW_RESTORE);
	SetForegroundWindow(hWnd);
}

static void TrayMenu(HWND hWnd)
{
	HMENU menu = CreatePopupMenu();
	AppendMenuW(menu, MF_STRING, kMenuShow, L"Show settings");
	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(menu, MF_STRING, kMenuExit, L"Quit (stops the camera)");
	SetMenuDefaultItem(menu, kMenuShow, FALSE);

	POINT pt;
	GetCursorPos(&pt);
	// Required so the menu closes when clicked away from
	SetForegroundWindow(hWnd);
	TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
	DestroyMenu(menu);
}

// ---------------------------------------------------------------- window

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {

	case WM_SYSCOMMAND:
		// Minimise goes to the tray rather than the taskbar. The camera keeps
		// running, which is the point of leaving the program open at all.
		if ((wp & 0xFFF0) == SC_MINIMIZE) {
			ShowWindow(hWnd, SW_HIDE);
			ExplainTray();
			return 0;
		}
		break;

	case kTrayMsg:
		if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK)
			ShowPanel(hWnd);
		else if (LOWORD(lp) == WM_RBUTTONUP)
			TrayMenu(hWnd);
		return 0;

	case WM_COMMAND:
		if (LOWORD(wp) == kMenuShow) ShowPanel(hWnd);
		else if (LOWORD(wp) == kMenuExit) DestroyWindow(hWnd);
		return 0;

	case WM_SIZE:
		if (g_controller) {
			RECT rc;
			GetClientRect(hWnd, &rc);
			g_controller->put_Bounds(rc);
		}
		return 0;

	case WM_TIMER:
		// Nothing to draw into while the panel is in the tray
		if (wp == kTimerId && IsWindowVisible(hWnd)) {
			g_preview.Tick();
			UpdateTally();
			UpdateRate();
		}
		return 0;

	case WM_CLOSE:
		// Settings are already saved as they are edited, so closing means
		// quitting, and quitting stops the camera.
		DestroyWindow(hWnd);
		return 0;

	case WM_DESTROY:
		Shell_NotifyIconW(NIM_DELETE, &g_tray);
		KillTimer(hWnd, kTimerId);
		g_preview.Destroy();
		g_webview.Reset();
		g_controller.Reset();
		PostQuitMessage(0);
		return 0;
	}

	return DefWindowProcW(hWnd, msg, wp, lp);
}

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int)
{
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	//
	// The elevated half of the installer. No window, no console, no message
	// loop: copy files, register the filter, write the Start menu entry, exit.
	// COM is needed for the shortcut.
	//
	const std::wstring cmd = lpCmdLine ? lpCmdLine : L"";
	const bool bInstall   = cmd.find(L"/install")   != std::wstring::npos;
	const bool bUninstall = cmd.find(L"/uninstall") != std::wstring::npos;

	if (bInstall || bUninstall) {
		if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
			return 1;

		// Uninstalling deletes the very file that is running, which Windows
		// will not allow. Carry on from a copy in the temporary folder, and do
		// not wait for it: it is waiting for this process to exit so the
		// executable stops being held open.
		if (bUninstall && IsInstalledCopy()) {
			wchar_t tmp[MAX_PATH] = {};
			GetTempPathW(MAX_PATH, tmp);
			const std::wstring copy = std::wstring(tmp) + L"SpoutCamUninstall.exe";
			if (CopyFileW(SelfPath().c_str(), copy.c_str(), FALSE)) {
				RunDetached(L"\"" + copy + L"\" /uninstall");
				CoUninitialize();
				return 0;
			}
		}

		const int rc = bInstall ? DoInstall() : DoUninstall();
		CoUninitialize();
		return rc;
	}

	// Held for the lifetime of the program. The filter opens it by name to
	// decide whether to pass the sender through, and a second copy of this
	// program would only fight the first over the same window.
	g_hRunEvent = CreateEventW(nullptr, TRUE, FALSE, kRunEventName);
	if (g_hRunEvent && GetLastError() == ERROR_ALREADY_EXISTS) {
		HWND existing = FindWindowW(kWndClass, nullptr);
		if (existing) {
			ShowWindow(existing, SW_SHOW);
			SetForegroundWindow(existing);
		}
		CloseHandle(g_hRunEvent);
		return 0;
	}

	if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
		return 1;

	WNDCLASSEXW wc = {};
	wc.cbSize        = sizeof(wc);
	wc.lpfnWndProc   = WndProc;
	wc.hInstance     = hInst;
	wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = CreateSolidBrush(RGB(22, 24, 28));
	wc.lpszClassName = kWndClass;
	wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
	wc.hIconSm       = wc.hIcon;
	RegisterClassExW(&wc);

	// Small enough to sit beside whatever it is being used to configure
	const int dpi = GetDpiForSystem();
	const int w = MulDiv(400, dpi, 96);
	const int h = MulDiv(534, dpi, 96); // fits the collapsed panel with nothing to spare

	g_hMain = CreateWindowExW(0, kWndClass, L"SpoutCam",
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
		CW_USEDEFAULT, CW_USEDEFAULT, w, h,
		nullptr, nullptr, hInst, nullptr);

	if (!g_hMain) {
		CoUninitialize();
		return 1;
	}

	AddTrayIcon(g_hMain, hInst);
	g_preview.Create(g_hMain, hInst);
	CreateWebView(g_hMain);

	ShowWindow(g_hMain, SW_SHOW);
	SetTimer(g_hMain, kTimerId, 33, nullptr); // preview refresh, about 30 fps

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	CoUninitialize();
	if (g_hRunEvent) CloseHandle(g_hRunEvent);
	return 0;
}
