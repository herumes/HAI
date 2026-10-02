#pragma once
// =============================================================================
// Marking out a region of the world
// =============================================================================
// Copying something that already exists starts with saying which part of the
// world you mean, and that is worth getting right on its own terms: it is the
// part you will actually be fiddling with, block by block, trying to get the
// box to sit exactly on a building.
//
// Two ways in, because they suit different situations:
//
//   * Two corners. Stand in one corner, press it, stand in the other, press
//     that. Exact, and the obvious thing when you can walk the building.
//   * A box around you. One press puts a box at your feet, and then you push
//     its faces out until it fits. Better when the thing is above you, or too
//     big to walk, or you just want to grab everything nearby.
//
// Either way it ends up as the same two corners, and every adjustment works on
// both. The box is drawn in the world as a single scaled cube - one debug
// shape, not twelve hundred - with the two corner blocks marked so you can tell
// which end you are moving.
#include "pch.h"
#include <atomic>
#include <string>

// The two corners, in world block coordinates. Kept exactly as set rather than
// sorted, so "corner 1" stays the one you put down first - sorting them behind
// your back makes the nudge buttons move the wrong end.
extern std::atomic<int> g_selX1, g_selY1, g_selZ1;
extern std::atomic<int> g_selX2, g_selY2, g_selZ2;
extern std::atomic<bool> g_sel1Set, g_sel2Set;

// Drawn in the world. Takes over the shape list while it is on, because the
// build guide's outline wants the same list and two things fighting over it
// just flickers.
extern std::atomic<bool> g_selShow;

void SelSetCorner1();          // to where you are standing
void SelSetCorner2();
void SelClear();

// A box centred on you, `radius` out horizontally and from `below` under your
// feet to `above` over your head. The usual way to start.
void SelBoxAtPlayer(int radius, int below, int above);

// Pushes one face out (positive) or pulls it in (negative). Faces are
// -X, +X, -Y, +Y, -Z, +Z in that order, which is how the UI lays them out.
void SelNudgeFace(int face, int amount);

// Moves the whole box without changing its size.
void SelMove(int dx, int dy, int dz);

// Grows or shrinks every face at once.
void SelExpandAll(int amount);

// Low and high corners, sorted. False when both corners are not set.
bool SelBounds(int& minX, int& minY, int& minZ, int& maxX, int& maxY, int& maxZ);

// Width, height, depth and total blocks. Zeroes when nothing is selected.
void SelSize(int& w, int& h, int& d, long long& volume);

// One line describing the selection, for the UI.
std::string SelSummary();

// Draws the box. Game thread, once per frame, before the shape queue drains.
void SelFrameTick();
