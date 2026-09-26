//
// How the camera tells the settings program what it is managing.
//
// This was the registry first, which worked everywhere except the one place it
// needed to. A DirectShow filter runs inside whatever program opened the
// camera, and streaming software tends to put its capture in a low integrity
// process. Such a process may open a named object, which is a read, but a write
// to HKEY_CURRENT_USER is refused, and RegSetValueEx says so only in a return
// value nobody was checking. The camera looked silent when it was talking to a
// wall.
//
// Shared memory carrying a low mandatory label is writable from there. It is
// the same class of object Spout itself passes textures through, which is why
// the filter can already receive frames from inside that process while being
// unable to write four bytes back out.
//
// Sixteen bytes, written whole once a second and read whenever the panel
// repaints. No lock: every field is an aligned DWORD, so a reader sees old or
// new and never half of either, and the worst case is one stale second on a
// number that only exists to be looked at.
//
#pragma once

#include <windows.h>
#include <sddl.h>
#include <string>

// Session local, like the event the filter watches for the settings program.
// Both sides are in the same session or neither mechanism would work at all.
static const wchar_t* kRateMapName = L"Local\\SpoutCamRate";

// Set once the camera has written a full set of values, so a mapping that has
// been created but never filled reads as nothing rather than as zero frames.
static const DWORD kRateMagic = 0x53437231; // 'SCr1'

struct SpoutCamRate {
	DWORD magic;
	DWORD camfps;  // tenths of a frame per second, as delivered
	DWORD tick;    // GetTickCount when this was written
	DWORD spare;   // keeps the block a round sixteen bytes
};

//
// Called by the settings program, which owns the mapping. Closing it takes the
// mapping with it, and the camera goes back to writing nowhere, which is the
// right answer because with the panel closed the camera is idle anyway.
//
inline HANDLE CreateRateMap(SpoutCamRate** ppView)
{
	//
	// Who is allowed to touch this.
	//
	// The mapping carries the frame rate the camera is delivering, read from
	// whatever process the host put the capture in. That process is often low
	// integrity, so it has to be able to write, and the descriptor therefore
	// cannot be the default one that only the creator can open.
	//
	// The first version granted generic all to Everyone, which is more than the
	// job needs - any process on the machine could rewrite or clear a value the
	// panel displays. The grant is now split: the logged on user gets the write
	// the filter needs, Everyone is left with read only, which is enough for
	// the panel to show the number and not enough for anything else to change
	// it. App packages are included because a containerised host still has to
	// report through this. The low mandatory label is what actually lets a low
	// integrity process through; lowering the label on an object we are
	// creating ourselves needs no privilege.
	//
	// The name is in the Local\ namespace, so it is per session and a process
	// in another session cannot reach it at all.
	//
	HANDLE hMap = nullptr;

	HANDLE hToken = nullptr;
	BYTE   userSid[SECURITY_MAX_SID_SIZE] = {};
	DWORD  sidSize = sizeof(userSid);

	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
		if (!GetTokenInformation(hToken, TokenUser, userSid, sidSize, &sidSize))
			sidSize = 0;
		CloseHandle(hToken);
	}

	if (sidSize > 0) {
		LPWSTR sidString = nullptr;
		if (ConvertSidToStringSidW((PSID)userSid, &sidString) && sidString) {
			std::wstring sddl = L"D:(A;;GA;;;";
			sddl += sidString;
			sddl += L")(A;;GR;;;WD)(A;;GA;;;AC)S:(ML;;NW;;;LW)";
			LocalFree(sidString);

			SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, FALSE };
			if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
					sddl.c_str(), SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr)) {
				hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
					0, sizeof(SpoutCamRate), kRateMapName);
				LocalFree(sa.lpSecurityDescriptor);
			}
		}
	}

	if (!hMap) {
		// Fall back to the permissive descriptor rather than leaving the panel
		// without a rate line. A SID that will not convert is not a reason to
		// lose the feature.
		SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, FALSE };
		if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
				L"D:(A;;GA;;;WD)(A;;GA;;;AC)S:(ML;;NW;;;LW)",
				SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr)) {
			hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
				0, sizeof(SpoutCamRate), kRateMapName);
			LocalFree(sa.lpSecurityDescriptor);
		}
	}

	if (!hMap)
		return nullptr;

	*ppView = (SpoutCamRate*)MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, sizeof(SpoutCamRate));
	if (!*ppView) {
		CloseHandle(hMap);
		return nullptr;
	}

	ZeroMemory(*ppView, sizeof(SpoutCamRate));
	return hMap;
}

//
// Called by the filter. Fails while the settings program is not running, which
// is expected and costs an opening attempt a second.
//
inline HANDLE OpenRateMap(SpoutCamRate** ppView)
{
	HANDLE hMap = OpenFileMappingW(FILE_MAP_WRITE, FALSE, kRateMapName);
	if (!hMap)
		return nullptr;

	*ppView = (SpoutCamRate*)MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, sizeof(SpoutCamRate));
	if (!*ppView) {
		CloseHandle(hMap);
		return nullptr;
	}
	return hMap;
}

inline void CloseRateMap(HANDLE& hMap, SpoutCamRate*& pView)
{
	if (pView) { UnmapViewOfFile(pView); pView = nullptr; }
	if (hMap)  { CloseHandle(hMap);      hMap  = nullptr; }
}
