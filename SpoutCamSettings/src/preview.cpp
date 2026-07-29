#include "preview.h"

#pragma comment(lib, "msimg32.lib") // AlphaBlend

static const wchar_t* kClassName = L"SpoutCamSettingsPreview";

// Checkerboard shown behind the frame so transparency is obvious
static const int  kCheckSize   = 10;
static const COLORREF kCheckA  = RGB(58, 58, 62);
static const COLORREF kCheckB  = RGB(78, 78, 84);
static const COLORREF kBackdrop = RGB(0, 0, 0);

SpoutPreview::~SpoutPreview()
{
	Destroy();
}

bool SpoutPreview::Create(HWND hParent, HINSTANCE hInst)
{
	static bool bRegistered = false;
	if (!bRegistered) {
		WNDCLASSEXW wc = {};
		wc.cbSize        = sizeof(wc);
		wc.style         = CS_HREDRAW | CS_VREDRAW;
		wc.lpfnWndProc   = WndProc;
		wc.hInstance     = hInst;
		wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
		wc.hbrBackground = nullptr; // painted in full every time
		wc.lpszClassName = kClassName;
		if (!RegisterClassExW(&wc))
			return false;
		bRegistered = true;
	}

	m_hWnd = CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_CLIPSIBLINGS,
		0, 0, 0, 0, hParent, nullptr, hInst, this);

	return m_hWnd != nullptr;
}

void SpoutPreview::Destroy()
{
	if (m_hWnd) {
		DestroyWindow(m_hWnd);
		m_hWnd = nullptr;
	}
	m_Receiver.ReleaseReceiver();
	m_Receiver.CloseDirectX11();
	m_bDXok = false;
	m_bDXtried = false;
}

void SpoutPreview::SetRect(int x, int y, int w, int h, bool bShow)
{
	if (!m_hWnd)
		return;

	// A zero sized window would make the size maths below divide by zero
	if (w < 8 || h < 8)
		bShow = false;

	m_bShow = bShow;

	if (!bShow) {
		ShowWindow(m_hWnd, SW_HIDE);
		// Drop the receiver so the sender is not held open while hidden
		m_Receiver.ReleaseReceiver();
		m_bConnected = false;
		m_bFrameValid = false;
		return;
	}

	m_Width = w;
	m_Height = h;
	SetWindowPos(m_hWnd, HWND_TOP, x, y, w, h, SWP_SHOWWINDOW | SWP_NOACTIVATE);
}

bool SpoutPreview::GrabFrame()
{
	if (!m_bDXtried) {
		m_bDXtried = true;
		m_bDXok = m_Receiver.OpenDirectX11();
	}
	if (!m_bDXok)
		return false;

	// Fit the sender inside the panel, keeping its shape
	unsigned int sw = m_Receiver.GetSenderWidth();
	unsigned int sh = m_Receiver.GetSenderHeight();
	int w = m_Width;
	int h = m_Height;
	if (sw > 0 && sh > 0) {
		const double scale = min((double)m_Width/(double)sw, (double)m_Height/(double)sh);
		w = max(1, (int)(sw*scale));
		h = max(1, (int)(sh*scale));
	}

	if ((int)m_Pixels.size() != w*h*4 || w != m_PixWidth || h != m_PixHeight) {
		m_Pixels.assign((size_t)w*h*4, 0);
		m_PixWidth = w;
		m_PixHeight = h;
		m_bFrameValid = false;
	}

	// AlphaBlend wants BGRA, so swap only when the sender texture is RGBA
	m_Receiver.SetSwap(m_Receiver.GetSenderFormat() == DXGI_FORMAT_R8G8B8A8_UNORM);

	if (!m_Receiver.ReceiveImage(m_Pixels.data(), (unsigned int)w, (unsigned int)h, false, false)) {
		m_bConnected = false;
		m_bFrameValid = false;
		m_SenderName[0] = 0;
		return false;
	}

	m_bConnected = true;

	if (m_Receiver.IsUpdated()) {
		// Size or sender changed - this call produced no pixels, wait for the next
		m_SenderWidth  = m_Receiver.GetSenderWidth();
		m_SenderHeight = m_Receiver.GetSenderHeight();
		const char* name = m_Receiver.GetSenderName();
		if (name) strcpy_s(m_SenderName, sizeof(m_SenderName), name);
		return false;
	}

	// Premultiply for AlphaBlend and note whether the sender uses alpha at all
	bool bAlpha = false;
	unsigned char* p = m_Pixels.data();
	const size_t count = (size_t)w*h;
	for (size_t i = 0; i < count; i++, p += 4) {
		const unsigned int a = p[3];
		if (a != 255) {
			bAlpha = true;
			p[0] = (unsigned char)((p[0]*a)/255);
			p[1] = (unsigned char)((p[1]*a)/255);
			p[2] = (unsigned char)((p[2]*a)/255);
		}
	}
	m_bHasAlpha = bAlpha;
	m_bFrameValid = true;
	return true;
}

void SpoutPreview::Tick()
{
	if (!m_hWnd || !m_bShow)
		return;

	GrabFrame();
	InvalidateRect(m_hWnd, nullptr, FALSE);
}

void SpoutPreview::DrawChecker(HDC hdc, int w, int h) const
{
	HBRUSH a = CreateSolidBrush(kCheckA);
	HBRUSH b = CreateSolidBrush(kCheckB);

	for (int y = 0; y < h; y += kCheckSize) {
		for (int x = 0; x < w; x += kCheckSize) {
			RECT cell = { x, y, min(x+kCheckSize, w), min(y+kCheckSize, h) };
			const bool bAlt = ((x/kCheckSize) + (y/kCheckSize)) % 2 == 0;
			FillRect(hdc, &cell, bAlt ? a : b);
		}
	}

	DeleteObject(a);
	DeleteObject(b);
}

void SpoutPreview::Paint(HDC hdc)
{
	RECT rc;
	GetClientRect(m_hWnd, &rc);
	const int cw = rc.right-rc.left;
	const int ch = rc.bottom-rc.top;
	if (cw <= 0 || ch <= 0)
		return;

	// Draw the whole panel off screen first, otherwise the checkerboard
	// flickers against the frame on every repaint
	HDC     mem = CreateCompatibleDC(hdc);
	HBITMAP bmp = CreateCompatibleBitmap(hdc, cw, ch);
	HGDIOBJ old = SelectObject(mem, bmp);

	HBRUSH back = CreateSolidBrush(kBackdrop);
	FillRect(mem, &rc, back);
	DeleteObject(back);

	if (m_bFrameValid && m_PixWidth > 0 && m_PixHeight > 0) {

		const int ox = (cw-m_PixWidth)/2;
		const int oy = (ch-m_PixHeight)/2;

		// Checkerboard only under the image, so letterbox bars stay black
		HRGN clip = CreateRectRgn(ox, oy, ox+m_PixWidth, oy+m_PixHeight);
		SelectClipRgn(mem, clip);
		SetWindowOrgEx(mem, -ox, -oy, nullptr);
		DrawChecker(mem, m_PixWidth, m_PixHeight);
		SetWindowOrgEx(mem, 0, 0, nullptr);
		SelectClipRgn(mem, nullptr);
		DeleteObject(clip);

		BITMAPINFO bmi = {};
		bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth       = m_PixWidth;
		bmi.bmiHeader.biHeight      = -m_PixHeight; // negative - top down, as received
		bmi.bmiHeader.biPlanes      = 1;
		bmi.bmiHeader.biBitCount    = 32;
		bmi.bmiHeader.biCompression = BI_RGB;

		HDC     src    = CreateCompatibleDC(mem);
		void*   bits   = nullptr;
		HBITMAP srcbmp = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
		if (srcbmp && bits) {
			memcpy(bits, m_Pixels.data(), m_Pixels.size());
			HGDIOBJ oldsrc = SelectObject(src, srcbmp);

			BLENDFUNCTION bf = {};
			bf.BlendOp             = AC_SRC_OVER;
			bf.SourceConstantAlpha = 255;
			bf.AlphaFormat         = AC_SRC_ALPHA; // premultiplied above

			AlphaBlend(mem, ox, oy, m_PixWidth, m_PixHeight,
				src, 0, 0, m_PixWidth, m_PixHeight, bf);

			SelectObject(src, oldsrc);
			DeleteObject(srcbmp);
		}
		DeleteDC(src);
	}
	else {
		// Nothing arriving - say so rather than showing an empty black box
		SetBkMode(mem, TRANSPARENT);
		SetTextColor(mem, RGB(120, 126, 136));
		HFONT font = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
			DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
		HGDIOBJ oldfont = SelectObject(mem, font);
		DrawTextW(mem, L"waiting for a sender", -1, &rc,
			DT_SINGLELINE | DT_CENTER | DT_VCENTER);
		SelectObject(mem, oldfont);
		DeleteObject(font);
	}

	BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);

	SelectObject(mem, old);
	DeleteObject(bmp);
	DeleteDC(mem);
}

LRESULT CALLBACK SpoutPreview::WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_NCCREATE) {
		auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
		SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
		return DefWindowProcW(hWnd, msg, wp, lp);
	}

	auto* self = reinterpret_cast<SpoutPreview*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

	switch (msg) {
	case WM_ERASEBKGND:
		return 1; // Paint covers the whole client area

	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hWnd, &ps);
		if (self) self->Paint(hdc);
		EndPaint(hWnd, &ps);
		return 0;
	}
	}

	return DefWindowProcW(hWnd, msg, wp, lp);
}
