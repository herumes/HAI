# HAI

AI-powered chat bot + live translator for Hytale (Windows DLL overlay).

## Features
- Live chat log with the game's colours, readable on a dark panel
- **Select and copy chat like text**: drag across lines, double-click a word,
  triple-click a message, Ctrl+A / Ctrl+C. Copy as plain text, rich text (Word,
  Google Docs and email keep the colours), `&c` / `&#RRGGBB` / `§c` / `<#RRGGBB>`
  colour codes, a coloured Discord code block, or HTML
- Right-click a line: copy message / line / name, mention or whisper the player,
  translate, highlight, ignore, whitelist, show only that player, export the log
- Search with `from:name`, highlight or hide non-matching lines, "mentions only"
- Mentions of your name or any highlight word: tinted line, optional sound and
  taskbar flash, unread count on the Chat tab
- Chat box with history (Up/Down), player-name completion (Tab), a length
  counter, and long messages split instead of cut off
- Auto replies: your own "when chat says X, send Y" rules (contains / starts with /
  exact / regex, per-rule cooldown, placeholders, a live tester)
- AI replies (Ollama or any OpenAI-compatible endpoint), tools (mob/item lookup,
  reminders, web search, time), live translation into any language, private
  encrypted messages, chat minigame solvers
- **AI Studio** (tab): every wake becomes a draft in an inbox. Choose whether replies go out by
  themselves, wait for your OK (send / edit / retry / retry with a note / discard, with hotkeys and a
  banner in the compact chat), or are only kept as a dry run. The model can be asked for your OK
  before it is called at all, and tool calls can ask first too (allow, edit the arguments, always /
  never). A prompt editor keeps the system prompt as a stack of preset files in `prompts\` that you
  switch on and off (CONTEXT.md is the base). A live view shows exactly what a question would send -
  system prompt, memory, chat lines, tools, a token budget - and lets you forget a memory turn or
  leave a chat line out. Also: cooldowns and a per-minute limit, wake word only at the start, reply
  clean-up (lowercase, no emoji / markdown, length cap, blocklist), per-tool off / ask / auto, and
  new tools (calculator, dice, chat search, recent players). Owner commands: `approve`, `discard`,
  `retry`, `mode auto|ask|dry`, `preset <name> [on|off]`.
- Compact see-through chat window: remembers its place, fades old lines,
  optional text outline, right-click menu; can hide the level / prestige tag,
  rank tag and title servers put before player names, leaving "Name: message"
- **Auto farm** (Farm tab): record one lap of a farm (F10), then it drives the
  lap for you on a loop (F9). Playback keeps lining itself up with the
  recording (position, speed, the tractor's own facing and the recording's
  clock) and presses keys in step with the game's frames, so lag, a slower lap
  or a different frame rate can't knock it off. "Find tractor direction" learns
  where the vehicle's facing lives, so spins end where yours did and a crooked
  tractor is straightened. It lets go of every key when you leave the route, get
  stuck, press a key yourself, or open chat. The map shows the lap, where the
  last run actually went and where it gave up; any step can be moved a few ms
- **Build with an AI-designed prefab** (Build tab > "Make a build with an AI"): describe a build,
  copy one ready-made prompt (the game's block list, the script format, verified stair/roof/door
  rules and design guidance are all in it), paste it into the best AI you have, copy its reply,
  paste it back. The reply is a short build script (boxes, walls, gable and hip roofs, stairs,
  cylinders, domes, mirror, repeat, scatter), not thousands of block records; the program expands it,
  checks every block name against the game's own list (with "did you mean" for invented ones),
  lists the materials to gather, saves the prefab and starts the guide. The guide also says in words
  which way stairs, roofs, beams and windows face. A `road` op lays curved roads, paths and bridge decks
  (waypoints, width, rounded corners, kerbs, centre line, barriers, supports)
- **Server mods**: blocks from mods your server runs are known everywhere - the prompts, the name check and the 3D
  preview. NoCube's Neon Blocks (17 unlit colours) are in; drop another mod's zip into `tools\server_mods`
- **Race tracks**: tick "Race track (the server's kart blocks)" and the copied prompt also knows the race server's
  custom blocks (asphalt, curbs, armco, tyre walls, start grid, pit boxes, podiums, flags...) with layout rules and a
  worked circuit. Their models and pictures come from the server's `BlockTextures.zip`
  (see `tools\race_blocks\README.md`); the block names are guesses from file names until the game's real list is given
- **3D preview of any prefab** ("Preview it in 3D" in the Build tab, and "Preview in 3D" on an AI reply
  before you even save it): opens your browser on the build drawn from the game's own block shapes and
  textures, read out of your `Assets.zip` (plus the race server's and neon mods' blocks). Four ways to look at it:
  **Orbit** (drag, wheel, double-click a block to centre on it, turntable, orthographic), **Free fly** like the game's
  camera (mouse look, WASD, Space up, Ctrl/C down, Shift fast, wheel = speed), **Walk** at head height with gravity,
  jumping, crouching and automatic climbing of stairs and slabs, and a **Layer map** that shows the build one layer at a
  time from above with a grid, coordinates and stair/roof direction arrows. Also: a sky with a time of day (nights
  light up neon and lamp blocks), haze, soft corner shadows, cut the build on any axis, click a block to see what it is,
  measure between two blocks, hide or isolate materials, copy the material list (with stacks), pictures up to 3x
  sharper with a transparent background, a turntable video, a compass, and a full key list on `H`. One-time setup:
  `python toolsuild_preview_pack.py --install` (finds Assets.zip by itself, the race server's `BlockTextures.zip`
  in `toolsace_blocks` or Downloads and server mods in `tools\server_mods`; needs Python with Pillow and numpy).
  The pack is generated from your install and is not committed
- Nearly everything is adjustable in Settings (searchable): colours, fonts,
  timestamps and time zone, hotkeys, which tabs are shown, and more

## Requirements
- Windows 10/11
- Visual Studio 2026 (v145 toolset) + vcpkg
- Ollama **or** any OpenAI-compatible API

## Quick Start
1. Build the project (`x64/Release`)
2. Copy the DLL + the `data` folder next to your Hytale client
3. Inject / load the DLL, then send one message in Hytale's own chat so HyAI can hook it
4. Open Settings in the overlay (Insert shows / hides it) and set your in-game name,
   bot name / wake word and AI provider

## Configuration
Everything is set in the overlay's Settings tab and saved to `hyai_settings.txt`
next to the DLL. Other files there:
- `CONTEXT.md` - the AI's base system prompt; `prompts\` - more prompt presets, switched on and off
  in AI Studio > Prompt
- `auto_replies.txt` - auto reply rules
- `ignored_users.txt`, `whitelist_users.txt`
- `chatlogs\` - daily chat logs, when "Save all chat to disk" is on
- `ailogs\` - one JSON line per AI exchange, when "Save every exchange" is on
- `farms\` - recorded farm laps (`.farm`, plain text); deleted ones go to `farms\deleted\`
- `ai_prefab_prompt.md`, `ai_prefab_prompt_race.md` and `ai_prefab_blocks.txt` - the prompts the Build tab
  copies and the block names it checks a reply against. All made by `python tools\make_ai_prompt.py` from
  `block_ids.txt` and `tools\race_blocks.json` (rerun it after the game updates, or after editing
  `tools\ai_prefab_prompt.template.md` / `ai_prefab_race.template.md` to change the rules or style advice)
- `prefabs\ai_scripts\` - the script behind every AI-made prefab, so an AI can be asked to change it
- `preview\` - the 3D viewer (`preview.html`, `viewer.js`, `mesher.js`, `collide.js`) and its block pack (`pack.js`, about 18 MB);
  `prefab.js` beside them is whatever was last previewed. Made by `tools\build_preview_pack.py --install`
  (`python tools\preview_prefab.py some.json` opens any prefab file in it without the game)

## Tests
- `tests\enginetest\build.bat` - the build-guide engine, no game needed
- `tests\scripttest\build.bat` - AI build scripts: every op (including `road`: widths, curves, kerbs, barriers, loops,
  slopes), block-name checks, the stair/roof/beam
  rotation tables (checked against a real exported house), and that the Build tab's own parser
  reads what is written. Also builds `bs_cli.exe`, which expands a script (or a whole AI reply)
  from the command line and can print every layer as text (`--layers`)
- `node tools\preview\test_collide.js` - the preview's walk physics: standing, walls, stepping onto slabs,
  climbing stairs, jumping, ceilings, flying, hidden materials
- `node tools\preview\test_mesher.js` - the 3D preview's meshing against the real block pack: soft shadows, glow, hidden
  faces, stair/roof/beam/slab rotation, state variants, every block in the pack, a real exported house
- `python tools\hycalibrate.py <Assets.zip> --check` - renders a curated set of blocks the way the preview
  does and compares them with the game's own item icons (mean agreement must stay above 0.87)
- `tests\farmtest\build.bat` - the auto-farm engine driving a simulated tractor (with its
  own facing, spinning in place at corners) round a walled farm and an open one: 30-240
  fps, jittery frames, a slower tractor, a tractor that turns 15% faster or slower, lag,
  rubber-banding, mid-lap starts, old recordings
- `tests\aitest\build.bat` - prompt variables, the preset stack, chat-context rules, reply clean-up,
  tool rules, rate limits, the calculator and dice, plus a fuzz pass
- `tests\flowtest\build.bat` - the whole AI flow against a fake Ollama server: automatic send,
  review, edit, retry with a note, tool questions, limits, dry run, cancel, timeouts, the prompt
  stack and the live context (build the Release objects first)
- `tests\prefixtest\build.bat` - hiding level / rank / title tags before player names
- `tests\uitest\build.bat` - renders the chat tab and settings pages offscreen with
  synthetic input and saves screenshots (build the Release DLL first)
- `tests\thunktest\build.bat` - the hook thunk every game hook goes through: arguments
  pass through untouched, a hook can swallow a call or stand another call in for it,
  and exceptions unwind through it

## Disclaimer
This is an unofficial third-party tool. Use at your own risk.
Respect the game's Terms of Service and anti-cheat rules.
