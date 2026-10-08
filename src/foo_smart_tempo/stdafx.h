// stdafx.h – precompiled header guards for Windows SDK includes
#pragma once
// NOTE: Do NOT define WIN32_LEAN_AND_MEAN here.  The foobar2000 SDK's
// playlist.h, ui.h and library_manager.h depend on full COM/OLE headers
// (IDataObject, IUnknown, the 'interface' keyword) and pfc/timers.h
// uses timeGetTime from mmsystem.h – all of which WIN32_LEAN_AND_MEAN would exclude.
#define NOMINMAX              // prevent min()/max() macro conflicts with <algorithm>
#define STRICT                // enforce strict handle type checking
#define _WIN32_WINNT 0x0601   // minimum: Windows 7 (foobar2000 v2 requirement)
