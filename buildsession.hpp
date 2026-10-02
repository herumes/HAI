#pragma once
// =============================================================================
// Build sessions: every build you start is saved as you go, and comes back
// =============================================================================
// A build that lives only in memory is a build the next crash throws away -
// along with where it sits, which way it faces, and which of ten thousand
// blocks you have already placed. None of that can be read back out of the
// world afterwards, so it has to be written down while it is still known.
//
// So a build is a SESSION, kept in its own folder under build_sessions\:
//
//   session.txt   where it is, how it looks, and the state of every block,
//                 rewritten within a second of anything changing
//   session.txt.bak   the previous good write, in case the newest is damaged
//   prefab.json   the session's own copy of the prefab, taken when it started
//
// The copy is what makes resuming exact. Progress is recorded against block
// numbers, and editing, re-exporting or deleting the original file would
// otherwise quietly re-number them. With its own copy a session opens the same
// way however long it has been.
//
// The one that was open when the game went away - crash, relog, server restart,
// closing it for the night - opens again by itself on the next launch, with the
// ghost back up if it was up. Nothing to press.
#include "pch.h"
#include <atomic>
#include <string>
#include <vector>

struct BuildSessionInfo {
    std::string id;                 // the folder name
    std::string name;               // what you called it
    std::string note;               // where it is - a server, a world, a town
    std::string prefabName;
    size_t blocks = 0, done = 0, skipped = 0;
    int64_t createdUnix = 0, updatedUnix = 0;
    int originX = 0, originY = 0, originZ = 0;
    int64_t activeMs = 0;
    bool active = false;            // the one open right now
    bool fromBackup = false;        // the newest save was damaged; this is the one before
};

// Starts a new build from a prefab file: parses it, keeps a copy, puts the
// ghost at your feet and shows it. Whatever was open is saved first. Async.
void SessionStartNew(const std::string& prefabPath);

// Opens a saved build exactly as it was left. Async.
void SessionOpen(const std::string& id);

// Saves and puts the current build away. It stays in the list.
void SessionClose();

// Gone for good - its folder is deleted. Closes it first if it is open.
bool SessionDelete(const std::string& id);

void SessionRename(const std::string& name);
void SessionSetNote(const std::string& note);

// Makes an independent copy of a session - the same build again, somewhere
// else, keeping the original's progress as a starting point or not.
void SessionDuplicate(const std::string& id, bool keepProgress);

bool SessionHasActive();
std::string SessionActiveId();
std::string SessionActiveName();
std::string SessionActiveNote();

// Newest first. Cached; refreshed after every save and every few seconds.
std::vector<BuildSessionInfo> SessionList();
void SessionRefreshList();

// The nearest unfinished build that is not open, within `radius` blocks of
// where you stand. For "you are standing next to the castle - pick it up?".
bool SessionNearby(BuildSessionInfo& out, int radius);

// Writes the open session now, rather than when the autosave next would.
bool SessionSaveNow();

// Steady-clock ms of the last save that reached the disk, 0 if none yet.
long long SessionLastSavedMs();
// "" when saving is working; otherwise what went wrong last time.
std::string SessionSaveError();

std::string SessionStatus();
std::string SessionFolder();

// True while a session is being opened, started or closed. The UI greys out
// anything that would start another one.
extern std::atomic<bool> g_sessionBusy;

// Reopen last launch's build on startup. On unless you turn it off.
extern std::atomic<bool> g_sessionAutoResume;

// Autosave, the startup resume, and arming follow-along. Called from the
// overlay's message loop every iteration - including while the overlay is
// hidden, which is when you are playing and this matters most.
void SessionTick();
