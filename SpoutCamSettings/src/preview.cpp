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

void SpoutPreview::SetOrientation(bool bMirror, bool bFlip, bool bSwap, unsigned int rotate)
{
	m_bMirror = bMirror;
	m_bFlip   = bFlip;
	m_bSwap   = bSwap;
	m_Rotate  = (rotate == 90 || rotate == 180 || rotate == 270) ? rotate : 0;

	// Force the next frame to resize, since a quarter turn changes the shape
	m_bFrameValid = false;
	m_PixWidth = 0;
	m_PixHeight = 0;
}

bool SpoutPreview::ProbeSender(unsigned int& width, unsigned int& height)
{
	char name[256] = {};
	if (!m_Receiver.GetActiveSender(name) || !name[0])
		return false;

	HANDLE handle = nullptr;
	DWORD  format = 0;
	unsigned int w = 0, h = 0;
	if (!m_Receiver.GetSenderInfo(name, w, h, handle, format) || w == 0 || h == 0)
		return false;

	strcpy_s(m_SenderName, sizeof(m_SenderName), name);
	width  = w;
	height = h;
	return true;
}

void SpoutPreview::SetKey(bool bOn, unsigned char r, unsigned char g, unsigned char b,
	bool bHardEdge, unsigned char threshold)
{
	m_bKey          = bOn;
	m_KeyR          = r;
	m_KeyG          = g;
	m_KeyB          = b;
	m_bKeyHardEdge  = bHardEdge;
	m_KeyThreshold  = threshold;
}

bool SpoutPreview::GrabFrame()
{
	if (!m_bDXtried) {
		m_bDXtried = true;
		m_bDXok = m_Receiver.OpenDirectX11();
	}
	if (!m_bDXok)
		return false;

	const bool bQuarter = (m_Rotate == 90 || m_Rotate == 270);

	// Fit the sender inside the panel, keeping its shape. A quarter turn
	// swaps what the viewer ends up seeing, so fit the turned shape.
	unsigned int sw = m_Receiver.GetSenderWidth();
	unsigned int sh = m_Receiver.GetSenderHeight();
	if (bQuarter) {
		const unsigned int t = sw; sw = sh; sh = t;
	}

	int w = m_Width;
	int h = m_Height;
	if (sw > 0 && sh > 0) {
		const double scale = min((double)m_Width/(double)sw, (double)m_Height/(double)sh);
		w = max(1, (int)(sw*scale));
		h = max(1, (int)(sh*scale));
	}

	// Size received before the turn is applied
	const int rw = bQuarter ? h : w;
	const int rh = bQuarter ? w : h;

	if (w != m_PixWidth || h != m_PixHeight) {
		m_Pixels.assign((size_t)rw*rh*4, 0);
		m_Rotated.assign((size_t)w*h*4, 0);
		m_PixWidth = w;
		m_PixHeight = h;
		m_bFrameValid = false;
	}

	//
	// Channel order is handled here rather than by the receiver.
	//
	// The panel is far smaller than any sender, so every frame goes through
	// the resampling path, and that path takes no swap flag at all. Asking the
	// receiver to swap therefore did nothing, and an RGBA sender came out with
	// red and blue exchanged: skin tones turned lavender.
	//
	// AlphaBlend wants BGRA. The user's own swap option flips that again,
	// hence the inequality.
	const bool bNeedsSwap = (m_Receiver.GetSenderFormat() == DXGI_FORMAT_R8G8B8A8_UNORM);
	const bool bSwapChannels = (bNeedsSwap != m_bSwap);
	m_Receiver.SetSwap(false);

	if (!m_Receiver.ReceiveImage(m_Pixels.data(), (unsigned int)rw, (unsigned int)rh, false, m_bFlip)) {
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

	// Mirror in the received frame's own space, before any turn, so the two
	// options combine the same way the filter combines them
	if (m_bMirror) {
		for (int y = 0; y < rh; y++) {
			unsigned char* row = m_Pixels.data()+(size_t)y*rw*4;
			for (int x = 0; x < rw/2; x++) {
				unsigned char* a = row+(size_t)x*4;
				unsigned char* b = row+(size_t)(rw-1-x)*4;
				for (int c = 0; c < 4; c++) {
					const unsigned char t = a[c]; a[c] = b[c]; b[c] = t;
				}
			}
		}
	}

	if (m_Rotate != 0) {
		m_Copy.RotateBuffer(m_Pixels.data(), m_Rotated.data(),
			(unsigned int)rw, (unsigned int)rh, 4, m_Rotate);
	}

	// Composite over the key colour first, in the channel order the pixels
	// arrived in, so the swap below carries the key with the image exactly as
	// the filter does it. The key is written positionally for the sender's
	// layout, the same swizzle spoutDX applies.
	const unsigned char key0 = bNeedsSwap ? m_KeyR : m_KeyB;
	const unsigned char key1 = m_KeyG;
	const unsigned char key2 = bNeedsSwap ? m_KeyB : m_KeyR;

	// Put the channels in the order the DIB expects, premultiply for
	// AlphaBlend, and note whether the sender uses alpha at all.
	// The sender carries straight alpha, so the scaling here is needed.
	bool bAlpha = false;
	unsigned char* p = (m_Rotate != 0) ? m_Rotated.data() : m_Pixels.data();
	const size_t count = (size_t)w*h;
	for (size_t i = 0; i < count; i++, p += 4) {

		if (m_bKey) {
			const unsigned int a = p[3];
			if (a != 255)
				bAlpha = true;

			if (m_bKeyHardEdge) {
				if (a < m_KeyThreshold) { p[0] = key0; p[1] = key1; p[2] = key2; }
			}
			else if (a == 0) {
				p[0] = key0; p[1] = key1; p[2] = key2;
			}
			else if (a != 255) {
				const unsigned int ia = 255-a;
				p[0] = (unsigned char)((p[0]*a + key0*ia)/255);
				p[1] = (unsigned char)((p[1]*a + key1*ia)/255);
				p[2] = (unsigned char)((p[2]*a + key2*ia)/255);
			}
			// Flattened, so nothing shows through to the checkerboard - which
			// is the point, since that is what the camera will send
			p[3] = 255;
		}

		if (bSwapChannels) {
			const unsigned char t = p[0];
			p[0] = p[2];
			p[2] = t;
		}

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

		const unsigned char* display = (m_Rotate != 0) ? m_Rotated.data() : m_Pixels.data();
		const size_t displaySize = (size_t)m_PixWidth*m_PixHeight*4;

		HDC     src    = CreateCompatibleDC(mem);
		void*   bits   = nullptr;
		HBITMAP srcbmp = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
		if (srcbmp && bits) {
			memcpy(bits, display, displaySize);
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
