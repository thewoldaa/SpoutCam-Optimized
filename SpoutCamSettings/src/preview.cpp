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
	ReleaseGdi();
	m_Receiver.ReleaseReceiver();
	m_Receiver.CloseDirectX11();
	m_bDXok = false;
	m_bDXtried = false;
}

//
// Drop every cached GDI object. Called on a size change and from Destroy, so
// nothing outlives the window it was made for.
//
void SpoutPreview::ReleaseGdi()
{
	if (m_memDc) {
		if (m_memOld) SelectObject(m_memDc, m_memOld);
		DeleteDC(m_memDc);
		m_memDc = nullptr;
		m_memOld = nullptr;
	}
	if (m_memBmp) { DeleteObject(m_memBmp); m_memBmp = nullptr; }
	m_memW = m_memH = 0;

	if (m_srcDc) {
		if (m_srcOld) SelectObject(m_srcDc, m_srcOld);
		DeleteDC(m_srcDc);
		m_srcDc = nullptr;
		m_srcOld = nullptr;
	}
	if (m_srcBmp) { DeleteObject(m_srcBmp); m_srcBmp = nullptr; }
	m_srcBits = nullptr;
	m_srcW = m_srcH = 0;

	if (m_brBack)   { DeleteObject(m_brBack);   m_brBack = nullptr; }
	if (m_brCheckA) { DeleteObject(m_brCheckA); m_brCheckA = nullptr; }
	if (m_brCheckB) { DeleteObject(m_brCheckB); m_brCheckB = nullptr; }
	if (m_fontWait) { DeleteObject(m_fontWait); m_fontWait = nullptr; }
}

//
// Off screen composite surface, client sized. Rebuilt only when the client
// size changes, which is when the panel is resized or the preview is opened.
//
void SpoutPreview::EnsureBackbuffer(HDC hdc, int cw, int ch)
{
	if (m_memDc && m_memW == cw && m_memH == ch)
		return;

	if (m_memDc) {
		if (m_memOld) { SelectObject(m_memDc, m_memOld); m_memOld = nullptr; }
		DeleteDC(m_memDc);
		m_memDc = nullptr;
	}
	if (m_memBmp) { DeleteObject(m_memBmp); m_memBmp = nullptr; }

	m_memDc = CreateCompatibleDC(hdc);
	if (!m_memDc)
		return;

	m_memBmp = CreateCompatibleBitmap(hdc, cw, ch);
	if (!m_memBmp) {
		DeleteDC(m_memDc);
		m_memDc = nullptr;
		return;
	}

	m_memOld = SelectObject(m_memDc, m_memBmp);
	m_memW = cw;
	m_memH = ch;

	if (!m_brBack)   m_brBack   = CreateSolidBrush(kBackdrop);
	if (!m_brCheckA) m_brCheckA = CreateSolidBrush(kCheckA);
	if (!m_brCheckB) m_brCheckB = CreateSolidBrush(kCheckB);
}

//
// DIB section the received frame is copied into. Top down, as received.
// Rebuilt only when the frame size changes.
//
void SpoutPreview::EnsureFrame(HDC hdc, int w, int h)
{
	if (m_srcDc && m_srcW == w && m_srcH == h)
		return;

	if (m_srcDc) {
		if (m_srcOld) { SelectObject(m_srcDc, m_srcOld); m_srcOld = nullptr; }
		DeleteDC(m_srcDc);
		m_srcDc = nullptr;
	}
	if (m_srcBmp) { DeleteObject(m_srcBmp); m_srcBmp = nullptr; }
	m_srcW = m_srcH = 0;

	BITMAPINFO bmi = {};
	bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth       = w;
	bmi.bmiHeader.biHeight      = -h; // negative - top down, as received
	bmi.bmiHeader.biPlanes      = 1;
	bmi.bmiHeader.biBitCount    = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	m_srcDc = CreateCompatibleDC(hdc);
	if (!m_srcDc)
		return;

	m_srcBmp = CreateDIBSection(m_srcDc, &bmi, DIB_RGB_COLORS, &m_srcBits, nullptr, 0);
	if (!m_srcBmp || !m_srcBits) {
		if (m_srcBmp) { DeleteObject(m_srcBmp); m_srcBmp = nullptr; }
		DeleteDC(m_srcDc);
		m_srcDc = nullptr;
		m_srcBits = nullptr;
		return;
	}

	m_srcOld = SelectObject(m_srcDc, m_srcBmp);
	m_srcW = w;
	m_srcH = h;
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
		// The frame DIB can be a few megabytes at sender size and is not used
		// while the panel is collapsed, so it does not stay resident. The
		// brushes and the off screen surface are small and are kept, since
		// reopening the preview is a common toggle.
		if (m_srcDc) {
			if (m_srcOld) { SelectObject(m_srcDc, m_srcOld); m_srcOld = nullptr; }
			DeleteDC(m_srcDc);
			m_srcDc = nullptr;
		}
		if (m_srcBmp) { DeleteObject(m_srcBmp); m_srcBmp = nullptr; }
		m_srcBits = nullptr;
		m_srcW = m_srcH = 0;
		return;
	}

	m_Width = w;
	m_Height = h;
	SetWindowPos(m_hWnd, HWND_TOP, x, y, w, h, SWP_SHOWWINDOW | SWP_NOACTIVATE);
}

void SpoutPreview::SetOrientation(bool bMirror, bool bFlip, bool bSwap)
{
	m_bMirror = bMirror;
	m_bFlip   = bFlip;
	m_bSwap   = bSwap;
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

	// Fit the sender inside the panel, keeping its shape
	const unsigned int sw = m_Receiver.GetSenderWidth();
	const unsigned int sh = m_Receiver.GetSenderHeight();

	int w = m_Width;
	int h = m_Height;
	if (sw > 0 && sh > 0) {
		const double scale = min((double)m_Width/(double)sw, (double)m_Height/(double)sh);
		w = max(1, (int)(sw*scale));
		h = max(1, (int)(sh*scale));
	}

	const int rw = w;
	const int rh = h;

	if (w != m_PixWidth || h != m_PixHeight) {
		m_Pixels.assign((size_t)rw*rh*4, 0);
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

	// Mirrored here rather than by the receiver, which ignores the flag on the
	// resampling path the panel always takes
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
	unsigned char* p = m_Pixels.data();
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

void SpoutPreview::DrawChecker(HDC hdc, int x0, int y0, int w, int h) const
{
	// Brushes are owned by the caller and reused across repaints
	if (!m_brCheckA || !m_brCheckB)
		return;

	// Drawn at the destination position rather than offsetting the window
	// origin and clipping to a region. The region was created and destroyed on
	// every repaint, and the checkerboard is only ever wanted under the image,
	// which is exactly what these bounds say.
	for (int y = 0; y < h; y += kCheckSize) {
		for (int x = 0; x < w; x += kCheckSize) {
			RECT cell = {
				x0+x, y0+y,
				x0+min(x+kCheckSize, w), y0+min(y+kCheckSize, h)
			};
			const bool bAlt = ((x/kCheckSize) + (y/kCheckSize)) % 2 == 0;
			FillRect(hdc, &cell, bAlt ? m_brCheckA : m_brCheckB);
		}
	}
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
	// flickers against the frame on every repaint.
	//
	// The surfaces and brushes are cached across repaints - see the note on
	// ReleaseGdi in preview.h. This runs at 30 Hz, and building them here each
	// time was the single largest cost in the panel.
	EnsureBackbuffer(hdc, cw, ch);
	if (!m_memDc) {
		// Nothing to composite into - leave the window as it is rather than
		// painting into the screen DC and flickering
		return;
	}

	HDC mem = m_memDc;

	FillRect(mem, &rc, m_brBack);

	if (m_bFrameValid && m_PixWidth > 0 && m_PixHeight > 0) {

		const int ox = (cw-m_PixWidth)/2;
		const int oy = (ch-m_PixHeight)/2;

		// Checkerboard only under the image, so letterbox bars stay black
		DrawChecker(mem, ox, oy, m_PixWidth, m_PixHeight);

		EnsureFrame(mem, m_PixWidth, m_PixHeight);
		if (m_srcDc && m_srcBits) {
			memcpy(m_srcBits, m_Pixels.data(),
				(size_t)m_PixWidth*m_PixHeight*4);

			BLENDFUNCTION bf = {};
			bf.BlendOp             = AC_SRC_OVER;
			bf.SourceConstantAlpha = 255;
			bf.AlphaFormat         = AC_SRC_ALPHA; // premultiplied above

			AlphaBlend(mem, ox, oy, m_PixWidth, m_PixHeight,
				m_srcDc, 0, 0, m_PixWidth, m_PixHeight, bf);
		}
	}
	else {
		// Nothing arriving - say so rather than showing an empty black box
		SetBkMode(mem, TRANSPARENT);
		SetTextColor(mem, RGB(120, 126, 136));
		if (!m_fontWait) {
			m_fontWait = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
				DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
				DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
		}
		HGDIOBJ oldfont = nullptr;
		if (m_fontWait)
			oldfont = SelectObject(mem, m_fontWait);
		DrawTextW(mem, L"waiting for a sender", -1, &rc,
			DT_SINGLELINE | DT_CENTER | DT_VCENTER);
		if (oldfont)
			SelectObject(mem, oldfont);
	}

	BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
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
