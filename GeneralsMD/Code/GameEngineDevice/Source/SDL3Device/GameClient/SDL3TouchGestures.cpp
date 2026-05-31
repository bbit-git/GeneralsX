/*
**	Command & Conquer Generals Zero Hour(tm)
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
** SDL3TouchGestures.cpp
**
** BlueOps @feature Android touch-gesture recognizer. See SDL3TouchGestures.h
** for the gesture table.
*/

#ifndef _WIN32

#include "SDL3Device/GameClient/SDL3TouchGestures.h"

#include <SDL3/SDL.h>
#include <cmath>

#include "Lib/BaseType.h"
#include "GameClient/Mouse.h"
#include "GameClient/View.h"
#include "GameClient/Display.h"
#include "SDL3Device/GameClient/SDL3Mouse.h"

namespace
{
	// ---- one-finger tuning (mirrors Platform/SDL3/sdl3_touch_shared.cpp) ----
	const int    DRAG_THRESHOLD = 10;    // px of travel before a drag is a drag
	const Uint64 SELECT_HOLD_MS = 180;   // stillness -> band-select on later drag
	const Uint64 LONG_PRESS_MS  = 350;   // stillness -> right click on lift

	// ---- two-finger camera dead-zones / sensitivities (device-tuned later) ----
	// The dead-zones are accumulated against a baseline that only re-bases when a
	// gesture actually fires, so slow gestures still cross the threshold.
	const float  PINCH_DEADZONE     = 12.0f;  // px change in finger spread
	const float  TWIST_DEADZONE     = 0.10f;  // rad change in finger-line angle
	const float  PITCH_DEADZONE     = 14.0f;  // px change in finger centroid Y
	const float  ZOOM_HEIGHT_PER_PX = 3.0f;   // userZoom() height per spread px
	const float  PITCH_RAD_PER_PX   = 0.01f;  // matches LookAtXlat pitch factor

	enum State { ST_IDLE, ST_MENU, ST_DOWN, ST_DRAG, ST_SELECT, ST_MULTI };

	struct Finger { SDL_FingerID id; float x, y; bool used; };

	State        g_state = ST_IDLE;
	Finger       g_fingers[2] = {};
	int          g_count = 0;
	SDL_FingerID g_primaryId = 0;
	bool         g_inGame = false;

	float   g_downX = 0, g_downY = 0;   // window px at first finger down
	float   g_lastX = 0, g_lastY = 0;   // window px, previous motion
	Uint64  g_downTime = 0;
	bool    g_lmbDown = false;          // a synthetic LEFT button-down is held
	Uint32  g_winId = 0;
	int     g_winW = 1, g_winH = 1;

	// two-finger baseline
	bool    g_multiBased = false;
	float   g_baseSpread = 0, g_baseAngle = 0, g_baseCy = 0;

	SDL3Mouse *getMouse(void) { return dynamic_cast<SDL3Mouse *>(TheMouse); }

	void refreshWindow(Uint32 id)
	{
		g_winId = id;
		SDL_Window *w = id ? SDL_GetWindowFromID(id) : NULL;
		if (!w) w = SDL_GetMouseFocus();
		if (w) {
			if (!g_winId) g_winId = SDL_GetWindowID(w);
			SDL_GetWindowSize(w, &g_winW, &g_winH);
		}
		if (g_winW <= 0) g_winW = 1;
		if (g_winH <= 0) g_winH = 1;
	}

	void countFingers(void)
	{
		g_count = (g_fingers[0].used ? 1 : 0) + (g_fingers[1].used ? 1 : 0);
	}

	void addFinger(const SDL_TouchFingerEvent &f)
	{
		for (int i = 0; i < 2; ++i) {
			if (!g_fingers[i].used) {
				g_fingers[i].used = true;
				g_fingers[i].id = f.fingerID;
				g_fingers[i].x = f.x * (float)g_winW;
				g_fingers[i].y = f.y * (float)g_winH;
				break;
			}
		}
		countFingers();
	}

	void removeFinger(const SDL_TouchFingerEvent &f)
	{
		for (int i = 0; i < 2; ++i)
			if (g_fingers[i].used && g_fingers[i].id == f.fingerID)
				g_fingers[i].used = false;
		countFingers();
	}

	void updateFinger(const SDL_TouchFingerEvent &f)
	{
		for (int i = 0; i < 2; ++i)
			if (g_fingers[i].used && g_fingers[i].id == f.fingerID) {
				g_fingers[i].x = f.x * (float)g_winW;
				g_fingers[i].y = f.y * (float)g_winH;
			}
	}

	void resetAll(void)
	{
		g_state = ST_IDLE;
		g_fingers[0].used = g_fingers[1].used = false;
		g_count = 0;
		g_lmbDown = false;
		g_multiBased = false;
	}

	// ---- synthetic mouse events (drive the normal Mouse pipeline) ----
	// button.x/y are raw window pixels; SDL3Mouse::scaleMouseCoordinates() maps
	// them to the game's internal resolution, exactly like a real mouse.

	void synthButton(Uint8 button, bool down, float x, float y)
	{
		SDL3Mouse *m = getMouse();
		if (!m) return;
		SDL_Event e;
		SDL_zero(e);
		e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
		e.button.timestamp = SDL_GetTicksNS();
		e.button.windowID = g_winId;
		e.button.which = SDL_TOUCH_MOUSEID;
		e.button.button = button;
		e.button.down = down;
		e.button.clicks = 1;
		e.button.x = x;
		e.button.y = y;
		m->addSDLEvent(&e);
	}

	void synthMotion(float x, float y, float dx, float dy, bool leftHeld)
	{
		SDL3Mouse *m = getMouse();
		if (!m) return;
		SDL_Event e;
		SDL_zero(e);
		e.type = SDL_EVENT_MOUSE_MOTION;
		e.motion.timestamp = SDL_GetTicksNS();
		e.motion.windowID = g_winId;
		e.motion.which = SDL_TOUCH_MOUSEID;
		e.motion.state = leftHeld ? (Uint32)SDL_BUTTON_LMASK : 0u;
		e.motion.x = x;
		e.motion.y = y;
		e.motion.xrel = dx;
		e.motion.yrel = dy;
		m->addSDLEvent(&e);
	}

	// ---- one-finger pan (grab & drag the tactical camera) ----
	void panBy(float curX, float curY)
	{
		if (!TheTacticalView || !TheDisplay) return;
		// Scroll the view opposite to the per-frame finger delta so the grabbed
		// point stays pinned under the finger, matching the desktop
		// m_dragScrollEnabled path (LookAtXlat.cpp: offset = lastPos - currentPos,
		// in display pixels). Convert window px -> display px for a 1:1 grab.
		const float scaleX = (float)TheDisplay->getWidth()  / (float)g_winW;
		const float scaleY = (float)TheDisplay->getHeight() / (float)g_winH;
		Coord2D offset;
		offset.x = (g_lastX - curX) * scaleX;
		offset.y = (g_lastY - curY) * scaleY;
		TheTacticalView->userScrollBy(&offset);
	}

	float angleDelta(float a, float b)
	{
		const float kPi    = 3.14159265f;
		const float kTwoPi = 6.28318531f;
		float d = a - b;
		while (d >  kPi) d -= kTwoPi;
		while (d < -kPi) d += kTwoPi;
		return d;
	}

	// ---- two-finger camera (zoom / rotate / pitch) ----
	void handleMultiMotion(void)
	{
		if (g_count < 2 || !TheTacticalView) return;

		const float ax = g_fingers[0].x, ay = g_fingers[0].y;
		const float bx = g_fingers[1].x, by = g_fingers[1].y;
		const float dx = bx - ax, dy = by - ay;
		const float spread = sqrtf(dx * dx + dy * dy);
		const float angle  = atan2f(dy, dx);
		const float cy     = (ay + by) * 0.5f;

		if (!g_multiBased) {
			g_baseSpread = spread;
			g_baseAngle = angle;
			g_baseCy = cy;
			g_multiBased = true;
			return;
		}

		const float dSpread = spread - g_baseSpread;
		const float dAngle  = angleDelta(angle, g_baseAngle);
		const float dCy     = cy - g_baseCy;

		// Normalised exceedance: pick the single dominant gesture so a pinch
		// doesn't also rotate/pitch from incidental drift.
		const float zN = fabsf(dSpread) / PINCH_DEADZONE;
		const float rN = fabsf(dAngle)  / TWIST_DEADZONE;
		const float pN = fabsf(dCy)     / PITCH_DEADZONE;
		if (zN < 1.0f && rN < 1.0f && pN < 1.0f)
			return;  // keep baseline; let a slow gesture accumulate past dead-zone

		if (zN >= rN && zN >= pN) {
			// Fingers apart -> zoom in (closer to ground = negative height delta,
			// per the mouse-wheel path in LookAtXlat.cpp).
			TheTacticalView->userZoom(-dSpread * ZOOM_HEIGHT_PER_PX);
		} else if (rN >= pN) {
			TheTacticalView->userSetAngle(TheTacticalView->getAngle() + dAngle);
		} else {
			TheTacticalView->userSetPitch(TheTacticalView->getPitch() - dCy * PITCH_RAD_PER_PX);
		}

		g_baseSpread = spread;
		g_baseAngle = angle;
		g_baseCy = cy;  // re-baseline after firing
	}

	void onFingerDown(const SDL_TouchFingerEvent &f)
	{
		refreshWindow(f.windowID);
		addFinger(f);

		if (g_count == 1) {
			g_primaryId = f.fingerID;
			g_downX = g_lastX = f.x * (float)g_winW;
			g_downY = g_lastY = f.y * (float)g_winH;
			g_downTime = SDL_GetTicks();
			g_lmbDown = false;
			g_inGame = (TheTacticalView != NULL);
			if (g_inGame) {
				g_state = ST_DOWN;            // classify on motion / lift
			} else {
				g_state = ST_MENU;            // pass through as a real left press
				synthButton(SDL_BUTTON_LEFT, true, g_downX, g_downY);
				g_lmbDown = true;
			}
			return;
		}

		// A second (or later) finger landed.
		if (!g_inGame) return;                // menus: only the primary finger acts
		if (g_lmbDown) {                      // don't strand an in-flight left button
			synthButton(SDL_BUTTON_LEFT, false, g_lastX, g_lastY);
			g_lmbDown = false;
		}
		g_state = ST_MULTI;
		g_multiBased = false;
	}

	void onFingerMotion(const SDL_TouchFingerEvent &f)
	{
		updateFinger(f);

		if (g_state == ST_MULTI) { handleMultiMotion(); return; }
		if (f.fingerID != g_primaryId) return;

		const float px = f.x * (float)g_winW;
		const float py = f.y * (float)g_winH;

		if (g_state == ST_MENU) {
			synthMotion(px, py, px - g_lastX, py - g_lastY, g_lmbDown);
			g_lastX = px; g_lastY = py;
			return;
		}

		if (g_state == ST_DOWN) {
			const float ddx = px - g_downX, ddy = py - g_downY;
			const bool moved = (ddx * ddx + ddy * ddy) > (float)(DRAG_THRESHOLD * DRAG_THRESHOLD);
			const Uint64 held = SDL_GetTicks() - g_downTime;
			if (moved)
				g_state = g_lmbDown ? ST_SELECT : ST_DRAG;
			else if (held >= SELECT_HOLD_MS)
				g_state = ST_SELECT;
		}

		if (g_state == ST_SELECT) {
			if (!g_lmbDown) {
				// Anchor the band box at the original touch point.
				synthButton(SDL_BUTTON_LEFT, true, g_downX, g_downY);
				g_lmbDown = true;
			} else {
				synthMotion(px, py, px - g_lastX, py - g_lastY, true);
			}
		} else if (g_state == ST_DRAG) {
			panBy(px, py);
		}

		g_lastX = px; g_lastY = py;
	}

	void onFingerUp(const SDL_TouchFingerEvent &f)
	{
		const bool wasMulti = (g_state == ST_MULTI);
		const bool isPrimary = (f.fingerID == g_primaryId);
		removeFinger(f);

		if (wasMulti) {
			// Stay suspended (no click) until every finger lifts.
			if (g_count == 0) resetAll();
			else g_multiBased = false;       // re-baseline for the remaining pair
			return;
		}

		if (!isPrimary) {
			// A non-primary finger we were ignoring (menus) lifted.
			if (g_count == 0) resetAll();
			return;
		}

		const Uint64 held = SDL_GetTicks() - g_downTime;

		if (g_state == ST_MENU) {
			if (g_lmbDown) synthButton(SDL_BUTTON_LEFT, false, g_lastX, g_lastY);
		} else if (g_state == ST_DRAG) {
			// Pan finished -> no click.
		} else if (g_state == ST_SELECT) {
			if (g_lmbDown) {
				synthButton(SDL_BUTTON_LEFT, false, g_lastX, g_lastY);  // close band box
			} else if (held >= LONG_PRESS_MS) {
				synthButton(SDL_BUTTON_RIGHT, true, g_downX, g_downY);
				synthButton(SDL_BUTTON_RIGHT, false, g_downX, g_downY);
			} else {
				synthButton(SDL_BUTTON_LEFT, true, g_downX, g_downY);
				synthButton(SDL_BUTTON_LEFT, false, g_downX, g_downY);
			}
		} else { // ST_DOWN: finger never moved
			if (held >= LONG_PRESS_MS) {
				synthButton(SDL_BUTTON_RIGHT, true, g_downX, g_downY);
				synthButton(SDL_BUTTON_RIGHT, false, g_downX, g_downY);
			} else {
				synthButton(SDL_BUTTON_LEFT, true, g_downX, g_downY);
				synthButton(SDL_BUTTON_LEFT, false, g_downX, g_downY);
			}
		}

		if (g_count == 0) resetAll();
		else { g_state = ST_IDLE; g_lmbDown = false; }
	}

} // anonymous namespace

void SDL3TouchGestures::handleFingerEvent(const SDL_Event &event)
{
	switch (event.type) {
		case SDL_EVENT_FINGER_DOWN:   onFingerDown(event.tfinger);   break;
		case SDL_EVENT_FINGER_MOTION: onFingerMotion(event.tfinger); break;
		case SDL_EVENT_FINGER_UP:     onFingerUp(event.tfinger);     break;
		default: break;
	}
}

void SDL3TouchGestures::tick(void)
{
	// If the primary finger is held still past the select timer, arm band-select
	// even when no FINGER_MOTION events arrive (mirrors Touch_Tick in CNC/RA).
	if (g_state == ST_DOWN && g_inGame) {
		const Uint64 held = SDL_GetTicks() - g_downTime;
		if (held >= SELECT_HOLD_MS) g_state = ST_SELECT;
	}
}

#endif // !_WIN32
