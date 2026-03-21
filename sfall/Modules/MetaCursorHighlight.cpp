/*
 *    sfall
 *    Copyright (C) 2008-2025  The sfall team
 *
 */

#include <cstring>

#include "..\main.h"
#include "..\FalloutEngine\Fallout2.h"

#include "LoadGameHook.h"
#include "MainLoopHook.h"

#include "MetaCursorHighlight.h"

namespace sfall
{

namespace
{

static const long kInvalidTile = -1;

static long metaCursorTile = 0;
static bool metaCursorTileWritten = false;
static bool pendingCursorSync = false;
static long appliedTile = kInvalidTile;
static long appliedElevation = kInvalidTile;

static __int64 GlobalIdFromName(const char* name) {
	__int64 id = 0;
	std::memcpy(&id, name, 8);
	return id;
}

static const __int64 kMetaTileGlobalId = GlobalIdFromName("MCTILE00");

static bool IsTileValid(long tile) {
	return (tile >= 0 && tile < 40000);
}

static bool ShouldSyncCursor() {
	if (!IsGameLoaded() || LoadGameHook::IsMapLoading() || fo::var::obj_dude == nullptr) {
		return false;
	}

	const DWORD blockedModes =
		WORLDMAP |
		DIALOG |
		ESCMENU |
		SAVEGAME |
		LOADGAME |
		OPTIONS |
		HELP |
		CHARSCREEN |
		PIPBOY |
		INVENTORY |
		AUTOMAP |
		SKILLDEX |
		INTFACEUSE |
		INTFACELOOT |
		BARTER |
		HEROWIN |
		DIALOGVIEW |
		COUNTERWIN |
		PAUSEWIN |
		SPECIAL;

	return (GetLoopFlags() & blockedModes) == 0;
}

static bool MoveMouseToTile(long tile) {
	long x = 0;
	long y = 0;
	if (fo::func::tile_coord(tile, &x, &y) != 0) return false;

	// Aim at the center of the hex so the engine resolves the same tile.
	x += 16;
	y += 8;

	long mouseX = 0;
	long mouseY = 0;
	fo::func::mouse_get_position(&mouseX, &mouseY);
	if (mouseX != x || mouseY != y) {
		fo::func::mouse_set_position(x, y);
	}

	fo::func::gmouse_3d_refresh();
	return true;
}

static void ResetCursorState() {
	metaCursorTile = 0;
	metaCursorTileWritten = false;
	pendingCursorSync = false;
	appliedTile = kInvalidTile;
	appliedElevation = kInvalidTile;
}

static void SyncCursor() {
	if (!pendingCursorSync || !metaCursorTileWritten) return;

	if (!IsTileValid(metaCursorTile)) {
		pendingCursorSync = false;
		appliedTile = kInvalidTile;
		appliedElevation = kInvalidTile;
		return;
	}

	if (!ShouldSyncCursor()) return;

	const long elevation = fo::var::elevation;
	if (!MoveMouseToTile(metaCursorTile)) return;

	appliedTile = metaCursorTile;
	appliedElevation = elevation;
	pendingCursorSync = false;
}

static void MainLoopSync() {
	SyncCursor();
}

static void GameModeChangeSync(DWORD) {
	if (metaCursorTileWritten && IsTileValid(metaCursorTile)) {
		pendingCursorSync = true;
	}
	SyncCursor();
}

}

void MetaCursorHighlight::init() {
	MainLoopHook::OnMainLoop() += MainLoopSync;
	MainLoopHook::OnCombatLoop() += MainLoopSync;
	LoadGameHook::OnGameModeChange() += GameModeChangeSync;
	LoadGameHook::OnBeforeMapLoad() += ResetCursorState;
	LoadGameHook::OnGameReset() += ResetCursorState;
	LoadGameHook::OnBeforeGameClose() += ResetCursorState;
}

void MetaCursorHighlight::exit() {
	ResetCursorState();
}

void MetaCursorHighlight::OnSfallGlobalSet(__int64 varId, int value) {
	if (varId != kMetaTileGlobalId) return;

	const bool tileChanged = !metaCursorTileWritten || metaCursorTile != value;
	metaCursorTile = value;
	metaCursorTileWritten = true;

	if (!IsTileValid(value)) {
		pendingCursorSync = false;
		appliedTile = kInvalidTile;
		appliedElevation = kInvalidTile;
		return;
	}

	if (tileChanged || appliedTile != value) {
		pendingCursorSync = true;
	}
}

}
