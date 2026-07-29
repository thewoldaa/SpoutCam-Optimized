//
// SpoutCamSettings
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
static std::wstring FindFilter()
{
	wchar_t exe[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	std::wstring dir = exe;
	dir.resize(dir.find_last_of(L'\\')+1);

#ifdef _WIN64
	const wchar_t* name = L"SpoutCam64.ax";
	const wchar_t* sub  = L"binaries\\SPOUTCAM\\SpoutCam64\\SpoutCam64.ax";
#else
	const wchar_t* name = L"SpoutCam32.ax";
	const wchar_t* sub  = L"binaries\\SPOUTCAM\\SpoutCam32\\SpoutCam32.ax";
#endif

	const std::wstring candidates[] = {
		dir+name,
		dir+sub,
		dir+L"..\\"+sub,
		dir+L"..\\..\\"+sub
	};

	for (const auto& path : candidates) {
		if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
			return path;
	}
	return L"";
}

static void RegisterFilter()
{
	const std::wstring filter = FindFilter();
	if (filter.empty()) {
		MessageBoxW(g_hMain,
			L"Could not find the SpoutCam filter.\n\n"
			L"Put SpoutCam64.ax next to this program, or run it from the "
			L"SpoutCam folder, then try again.",
			L"SpoutCam Settings", MB_OK | MB_ICONWARNING);
		return;
	}

	std::wstring args = L"/s \"";
	args += filter;
	args += L"\"";

	SHELLEXECUTEINFOW sei = {};
	sei.cbSize       = sizeof(sei);
	sei.fMask        = SEE_MASK_NOCLOSEPROCESS;
	sei.hwnd         = g_hMain;
	sei.lpVerb       = L"runas";      // triggers the elevation prompt
	sei.lpFile       = L"regsvr32.exe";
	sei.lpParameters = args.c_str();
	sei.nShow        = SW_HIDE;

	if (!ShellExecuteExW(&sei)) {
		if (GetLastError() != ERROR_CANCELLED) {
			MessageBoxW(g_hMain, L"Could not start regsvr32.",
				L"SpoutCam Settings", MB_OK | MB_ICONERROR);
		}
		return;
	}

	DWORD code = 1;
	if (sei.hProcess) {
		WaitForSingleObject(sei.hProcess, 30000);
		GetExitCodeProcess(sei.hProcess, &code);
		CloseHandle(sei.hProcess);
	}

	MessageBoxW(g_hMain,
		code == 0 ? L"SpoutCam registered."
		          : L"Registration failed. Check that the filter file is intact.",
		L"SpoutCam Settings",
		MB_OK | (code == 0 ? MB_ICONINFORMATION : MB_ICONERROR));
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
	}
	else if (type == L"save") {
		SaveSettings(json);
		DestroyWindow(g_hMain);
	}
	else if (type == L"cancel") {
		DestroyWindow(g_hMain);
	}
	else if (type == L"register") {
		RegisterFilter();
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
		text = L"idle";
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

static void CreateWebView(HWND hWnd)
{
	CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, nullptr,
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

// ---------------------------------------------------------------- window

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {

	case WM_SIZE:
		if (g_controller) {
			RECT rc;
			GetClientRect(hWnd, &rc);
			g_controller->put_Bounds(rc);
		}
		return 0;

	case WM_TIMER:
		if (wp == kTimerId) {
			g_preview.Tick();
			UpdateTally();
		}
		return 0;

	case WM_CLOSE:
		DestroyWindow(hWnd);
		return 0;

	case WM_DESTROY:
		KillTimer(hWnd, kTimerId);
		g_preview.Destroy();
		g_webview.Reset();
		g_controller.Reset();
		PostQuitMessage(0);
		return 0;
	}

	return DefWindowProcW(hWnd, msg, wp, lp);
}

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

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
	const int h = MulDiv(486, dpi, 96); // fits the collapsed panel with nothing to spare

	g_hMain = CreateWindowExW(0, kWndClass, L"SpoutCam Settings",
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
		CW_USEDEFAULT, CW_USEDEFAULT, w, h,
		nullptr, nullptr, hInst, nullptr);

	if (!g_hMain) {
		CoUninitialize();
		return 1;
	}

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
	return 0;
}
