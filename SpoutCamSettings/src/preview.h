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

// Whether Spout counts frames between senders and receivers. Off by default,
// and with it off no sender rate can be had from anywhere: the SDK hands back
// the monitor refresh rate instead, which looks like an answer and is not one.
bool SpoutFrameCountEnabled();

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

	// The sender's own frame rate, or zero when there is nothing to ask: the
	// preview is collapsed, or Spout is not counting frames at all. Worth
	// having separately from the filter's copy, because this one answers
	// before any streaming program has opened the camera.
	double SenderFps();

	bool IsVisible()   const { return m_bShow; }
	bool IsConnected() const { return m_bConnected; }
	bool HasAlpha()    const { return m_bHasAlpha; }
	unsigned int SenderWidth()  const { return m_SenderWidth; }
	unsigned int SenderHeight() const { return m_SenderHeight; }
	const char*  SenderName()   const { return m_SenderName; }

private:
	static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
	void Paint(HDC hdc);
	void DrawChecker(HDC hdc, int w, int h) const;
	bool GrabFrame();

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
