//
// SpoutCamSettings - live preview of the incoming Spout sender.
//
// Draws into a plain child window placed over a slot in the WebView2 page.
// The frame is composited over a checkerboard so transparency is visible at a
// glance - that is the quickest way to tell whether a sender carries alpha at
// all, which decides whether the key colour option is any use.
//
// Deliberately GDI rather than D3D. The panel is around 150 pixels tall and
// only runs while it is on screen, so a swap chain and a pair of shaders would
// be a lot of moving parts for a picture this size.
//

#pragma once

#include <windows.h>
#include <vector>
#include "..\..\SpoutCam\SpoutDX\source\SpoutDX.h"

class SpoutPreview
{
public:
	SpoutPreview() = default;
	~SpoutPreview();

	// Create the child window. Hidden until SetRect asks for it.
	bool Create(HWND hParent, HINSTANCE hInst);
	void Destroy();

	// Position over the page slot, in physical pixels relative to the parent.
	void SetRect(int x, int y, int w, int h, bool bShow);

	// Receive one frame and repaint. Cheap no-op while hidden.
	void Tick();

	// Follow the orientation controls as they are edited, so the preview
	// shows what is being set rather than what was last saved.
	void SetOrientation(bool bMirror, bool bFlip, bool bSwap);

	// Same for the key colour, so the preview shows what the camera will send
	// rather than the checkerboard it uses to reveal alpha.
	void SetKey(bool bOn, unsigned char r, unsigned char g, unsigned char b,
		bool bHardEdge, unsigned char threshold);

	// Is a sender running, and how big? Answered without connecting to it, so
	// the header lamp still works while the preview is collapsed. This reads
	// the sender's shared memory description only, no texture and no DirectX.
	bool ProbeSender(unsigned int& width, unsigned int& height);

	bool IsVisible()   const { return m_bShow; }
	bool IsConnected() const { return m_bConnected; }
	bool HasAlpha()    const { return m_bHasAlpha; }
	unsigned int SenderWidth()  const { return m_SenderWidth; }
	unsigned int SenderHeight() const { return m_SenderHeight; }
	const char*  SenderName()   const { return m_SenderName; }

private:
	static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
	void Paint(HDC hdc);
	void DrawChecker(HDC hdc, int x0, int y0, int w, int h) const;
	bool GrabFrame();

	//
	// GDI objects held between repaints.
	//
	// The preview repaints about thirty times a second, and the first version
	// built every one of these inside Paint and destroyed them again on the way
	// out - two device contexts, a compatible bitmap, a DIB section, three
	// brushes and a font, so on the order of two hundred object creations a
	// second for a picture that changes size only when the window does. They
	// are made once per size and reused.
	//
	// ReleaseGdi drops them on a size change and in Destroy.
	//
	void ReleaseGdi();
	void EnsureBackbuffer(HDC hdc, int cw, int ch);
	void EnsureFrame(HDC hdc, int w, int h);

	HDC     m_memDc   = nullptr;  // off screen composite, client sized
	HBITMAP m_memBmp  = nullptr;
	HGDIOBJ m_memOld  = nullptr;
	int     m_memW    = 0;
	int     m_memH    = 0;

	HDC     m_srcDc   = nullptr;  // frame, at sender display size
	HBITMAP m_srcBmp  = nullptr;
	HGDIOBJ m_srcOld  = nullptr;
	void*   m_srcBits = nullptr;  // the DIB's pixels, held from creation
	int     m_srcW    = 0;
	int     m_srcH    = 0;

	HBRUSH m_brBack  = nullptr;
	HBRUSH m_brCheckA = nullptr;
	HBRUSH m_brCheckB = nullptr;
	HFONT  m_fontWait = nullptr;

	HWND  m_hWnd  = nullptr;
	bool  m_bShow = false;
	int   m_Width  = 0;   // client size of the preview window
	int   m_Height = 0;

	spoutDX m_Receiver;
	bool m_bDXok      = false;
	bool m_bDXtried   = false;
	bool m_bConnected = false;
	bool m_bHasAlpha  = false;

	unsigned int m_SenderWidth  = 0;
	unsigned int m_SenderHeight = 0;
	char m_SenderName[256] = {};

	// RGBA received at preview size, then premultiplied in place for AlphaBlend
	std::vector<unsigned char> m_Pixels;
	int m_PixWidth  = 0;
	int m_PixHeight = 0;
	bool m_bFrameValid = false;

	bool m_bMirror = false;
	bool m_bFlip   = false;
	bool m_bSwap   = false;

	bool m_bKey = false;
	unsigned char m_KeyR = 0, m_KeyG = 255, m_KeyB = 0;
	bool m_bKeyHardEdge = false;
	unsigned char m_KeyThreshold = 128;

	spoutCopy m_Copy;
};
