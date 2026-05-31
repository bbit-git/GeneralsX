/*
**	Command & Conquer Generals(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
** SDL3TouchGestures.h
**
** BlueOps @feature Android touch-gesture recognizer.
**
** Translates SDL_EVENT_FINGER_* into engine input, mirroring the shared CNC
** TD/RA touch machine (Platform/SDL3/sdl3_touch_shared.cpp) so gesture feel is
** consistent across the BlueOps engines:
**
**   In game (TheTacticalView != NULL):
**     1-finger tap                 -> left click (select / press a widget)
**     1-finger hold >=350ms, lift  -> right click (move / attack / cancel)
**     1-finger drag                -> pan the tactical camera (grab & drag)
**     1-finger hold >=180ms + drag -> rubber-band selection
**     2-finger pinch               -> zoom
**     2-finger twist               -> rotate (yaw)
**     2-finger parallel vertical   -> pitch
**
**   In menus (TheTacticalView == NULL):
**     the primary finger passes straight through as a left mouse
**     press / drag / release so buttons, sliders and scrollbars keep working;
**     extra fingers are ignored.
**
** One-finger clicks and band-selects are fed through SDL3Mouse so the normal
** Mouse::createStreamMessages -> Selection/CommandXlat pipeline is reused
** unchanged. Two-finger camera gestures call TheTacticalView->user* directly.
*/

#pragma once

#ifndef _WIN32

#include <SDL3/SDL.h>

namespace SDL3TouchGestures
{
	/// Feed one SDL_EVENT_FINGER_DOWN/MOTION/UP event from the SDL pump.
	void handleFingerEvent(const SDL_Event &event);

	/// Per-frame tick driving time-based transitions (the hold-to-select timer).
	/// Call once per event pump, after draining the SDL event queue.
	void tick(void);
}

#endif // !_WIN32
