# AI Level Connector

A Windows-only Geode mod source project for letting an AI inspect and edit
the currently open local Geometry Dash level editor over a REST API. The
server binds only to `127.0.0.1`; it is not exposed to other computers on the
network.

The mod has been built for Geode 5.9.0 and Geometry Dash 2.2081. The package is
available at `build\oliver.ai-connector.geode` and is installed in the Steam
Geometry Dash Geode mods folder. Restart Geometry Dash to load it.
The supplied AI-to-GD Editor artwork is packaged as the mod's Geode logo.
Each player's installation runs an API for that player's own game at
`127.0.0.1`; installing this mod does not expose their game to the internet or
connect it to a shared server.

## API

Start Geometry Dash with Geode and open a level in the editor. The mod starts
the API when it loads. the in-editor **CONNECTOR** button opens a compact, scrollable status box with
small text and at most four short log lines.

`GET http://127.0.0.1:8765/api/status` returns server status.
It also reports whether a level snapshot is available and its age.

`GET http://127.0.0.1:8765/api/tools` describes the available operations.

`GET http://127.0.0.1:8765/api/screen` returns the Geometry Dash client area as
a BMP image so an AI client can inspect the actual game view for visual errors.
It targets the Geometry Dash window only and never captures other applications
or the desktop. It can capture the game in the background when Windows and the
game renderer allow `PrintWindow`; if that is unavailable, bring Geometry Dash
to the foreground and retry. The game must not be minimized.

For an AI inspection loop, capture `/api/screen` after placing or editing
objects, inspect the returned BMP, and compare it with `/api/level` and
`/api/level/analyze` before making corrective edits. A screenshot is visual
evidence, not a playability guarantee; use `/api/playtest` to test gameplay.

Save a screenshot from PowerShell:

```powershell
Invoke-WebRequest -UseBasicParsing `
    "http://127.0.0.1:8765/api/screen" `
    -OutFile ".\gd-screen.bmp"
```

`GET http://127.0.0.1:8765/api/logs` returns the last 10 request/action log
entries by default. Set `?limit=1` through `?limit=30` to adjust the response.

`GET http://127.0.0.1:8765/api/level?offset=0&limit=25` reads the currently
open editor's most recently captured local level name, object count, and one
compact page of object details. The default page size is 25; request up to 500
objects at a time with `limit`. `level.groundY`, `level.gridSize`,
`level.songId`, `level.audioTrack`, and `level.platformerMode` report live
editor properties. The mod refreshes this snapshot while the game is
running and keeps the last snapshot available while Geometry Dash is
minimized or temporarily not processing frames. Check `snapshotAgeMs` and
`snapshotFresh` in the response: while minimized, the snapshot is the last
captured editor state, not a live read of changes made after the game paused.
Each object includes its current `index`, object `id`, `x`, `y`, `rotation`,
and `scale`, plus current collision `bounds` (`minX`, `minY`, `maxX`, `maxY`)
in editor units. It also includes the game's sprite `frame`, runtime-derived
`type`, and a plain-language `behavior` summary for flags such as triggers,
portals, speed changers, decorations, and start positions. Descriptions use
the active game object state and avoid claiming exact behavior when the game
does not expose it. `descriptionSource` distinguishes `game_runtime` flags
from `sprite_frame_hint` classifications (for example, likely spikes, saws,
orbs, and pads). The `gameFlags` object also gives machine-readable runtime
flags for `trigger`, `hazardousSlope`, `decoration`, `passable`, `speedObject`, `portal`,
and `startPosition` (`hazardousSlope` specifically means a slope the game marks
as hazardous). Request `GET /api/objects/info?index=0` for these details
about one current object. Use the returned index in edit/delete requests, and
re-read the level before editing if other actions may have changed its contents.
`limit` may be between 1 and 500.

For compact output, prefer `GET /api/level/clusters`. It groups nearby objects
by their actual bounding boxes and X spacing and returns only each cluster's
bounds, object count, trigger count, IDs, and gap to the next cluster.
`gap` is in editor units (default: two current grid cells); `limit` returns up
to 100 clusters:

```powershell
Invoke-RestMethod "http://127.0.0.1:8765/api/level/clusters?limit=20"
```

For a compact PowerShell table instead of the full REST response, use the
included helper. Clusters are shown by default:

```powershell
.\scripts\get-level-summary.ps1
.\scripts\get-level-summary.ps1 -View Reachability
.\scripts\get-level-summary.ps1 -View Objects -Limit 20
```

To create a small ground-backed demonstration course, first open a separate
blank local level in the editor. The helper refuses to place anything unless
the active level is empty:

```powershell
.\scripts\create-demo-course.ps1
```

It creates a snapped continuous block floor and four widely spaced spikes.
Inspect it in Geometry Dash and save it manually only if you want to keep it.

`GET /api/level/analyze?platformIds=1` reports off-grid objects and estimates
whether a cube can traverse platforms of the selected object IDs. It uses the
live editor grid, object collision bounds, and conservative configurable
`maxJumpTiles`, `maxRiseTiles`, and `maxDropTiles` estimates. This is a
geometry-based warning, not proof of playability: it does not simulate timing,
player speed, spikes, orbs, triggers, or multi-mode interactions. Always
confirm with the in-editor playtest.

`GET http://127.0.0.1:8765/api/level/string` returns the most recently
captured complete native Geometry Dash level string, including object
properties not included in the simplified object listing.
Use `GET /api/level/string?preview=1` for a compact preview and character count;
omit `preview=1` only when the full native data is required.

`POST http://127.0.0.1:8765/api/objects` places Geometry Dash objects and
triggers in the open editor. Each item requires a game-supported object `id`
and `x`/`y` coordinates in editor units. The API does not impose an arbitrary
10,000-ID ceiling; Geometry Dash determines whether a supplied ID exists.
`rotation` (degrees) and `scale` are optional. Requested objects are placed
without adding extra blocks by default. To explicitly add a continuous
ground-level block path (object ID 1) from the player start through the
requested horizontal span, set the request's top-level `ensureGroundPath` to
`true`. Safety paths are limited to 2,048 blocks per request.
Ordinary objects snap to the current editor grid by default (triggers retain
their exact positions). Vertical grid rows are anchored to the live ground
line and each object's collision bounds, so a ground-level block rests on the
floor instead of being rounded underneath it or floated a half-cell above it.
For deterministic floor placement, supply `groundOffsetTiles: 0` to set an
object on the floor, `groundOffsetTiles: 1` to put its bottom one grid cell
above it, and so on. This is calculated on Geometry Dash's main thread using
the live floor and the created object's bounds. Set `snapToGrid: false` when
off-grid placement is intentional. Native imports, replacements, and indexed
edits also align ordinary objects to the grid.
All newly placed objects are checked against the live ground height and their
actual bounding box and lifted as needed so they cannot be left below the
ground line. This also applies to native imports, native replacements, and
repositioned objects. The legacy `keepAboveGround` field is still accepted
but is no longer required.

For other native object properties (including trigger settings, groups, colors,
and related fields), `POST /api/objects/native` accepts comma-separated native
Geometry Dash object records. Use records from `GET /api/level/string` and
preserve their native key/value pairs:

```powershell
$record = "1,1,2,200,3,120,57,1"
Invoke-RestMethod "http://127.0.0.1:8765/api/objects/native" `
    -Method Post -ContentType "application/json" `
    -Body (@{ objects = @($record) } | ConvertTo-Json -Depth 5)
```

This can create object types and triggers with their supplied native fields.
To replace an existing object's complete native record and properties, use
`POST /api/objects/native/replace` with its current index from `/api/level`:

```powershell
$replacement = @{
    objects = @(@{ index = 0; data = "1,1,2,200,3,120,57,1" })
} | ConvertTo-Json -Depth 5
Invoke-RestMethod "http://127.0.0.1:8765/api/objects/native/replace" `
    -Method Post -ContentType "application/json" -Body $replacement
```

Replacement creates the supplied native record and removes the old object;
the new object is appended, so its index changes. Existing objects can also be
repositioned, rotated, scaled, or deleted with the indexed endpoints below.
Re-read `/api/level` after mutations before reusing object indices.

`POST http://127.0.0.1:8765/api/objects/edit` changes existing objects by
their index from `/api/level`. Supply one or more of `x`, `y`, `rotation`, or
`scale` for each object.

`POST http://127.0.0.1:8765/api/objects/delete` deletes objects by their
indices from `/api/level`.

`POST http://127.0.0.1:8765/api/playtest` starts an editor playtest;
`POST http://127.0.0.1:8765/api/playtest/stop` stops it. While the playtest is
active, an AI can capture the screen and drive the player-one jump input using
`POST /api/playtest/input`:

```powershell
Invoke-RestMethod "http://127.0.0.1:8765/api/playtest/input" `
    -Method Post -ContentType "application/json" `
    -Body '{"button":"jump","down":true}'

Start-Sleep -Milliseconds 120

Invoke-RestMethod "http://127.0.0.1:8765/api/playtest/input" `
    -Method Post -ContentType "application/json" `
    -Body '{"button":"jump","down":false}'
```

The `down` flag must be set back to `false` to release the jump button. A
computer-use AI can alternate screenshots and short jump presses to attempt
the level. This enables interactive playtesting; it does not automatically
prove a level possible or impossible, and results depend on the AI's timing
and the tested route.

`POST http://127.0.0.1:8765/api/save` queues Geometry Dash's editor save
routine. It returns `202 Accepted` immediately rather than waiting on the game
frame thread; check `/api/logs` for the result. If the game is minimized or
paused, the save runs once the editor resumes processing frames.

`GET /api/songs/newgrounds?query=...` returns a Newgrounds audio search link
for a title or artist. It does not download audio. `GET /api/songs/local`
searches local Geometry Dash save and Resources folders for MP3, OGG, WAV, and
FLAC files, returning filenames and numeric song IDs when available. Local
metadata is not inferred from audio contents; Geometry Dash remains responsible
for downloading and selecting Newgrounds tracks.

Search results are capped at 20 by default; add `&limit=50` for up to 100.

```powershell
$query = [uri]::EscapeDataString("artist or song title")
Invoke-RestMethod "http://127.0.0.1:8765/api/songs/newgrounds?query=$query"
Invoke-RestMethod "http://127.0.0.1:8765/api/songs/local?query=optional"
```

Set a Newgrounds custom-song ID on the currently open local level with
`POST /api/song`; the endpoint only changes the level's song ID and does not
download or verify the track. Save the level separately:

```powershell
Invoke-RestMethod "http://127.0.0.1:8765/api/song" `
    -Method Post -ContentType "application/json" `
    -Body '{"songId":123456}'
```

Use `{"songId":0}` to clear the custom song ID and restore the level's built-in
song choice.

The supplied TikTok-song reference reports no verified matching
Geometry Dash/Newgrounds IDs, so the mod does not invent IDs from that list.
Search Newgrounds and supply a real audio ID.

Mutation requests accept at most 100 objects and respond when queued. Check
`/api/logs` for the results. These endpoints do not open arbitrary files or
edit levels outside the currently active editor.

Example from PowerShell:

```powershell
Invoke-RestMethod "http://127.0.0.1:8765/api/objects" `
    -Method Post `
    -ContentType "application/json" `
    -Body '{"objects":[{"id":1,"x":200,"y":100}]}'
```

Or send a batch with the included helper:

```powershell
$objects = @(
    @{ id = 1; x = 200; y = 100; rotation = 0; scale = 1 },
    @{ id = 2; x = 240; y = 100; rotation = 15; scale = 1.5 }
)
.\scripts\send-decorations.ps1 -Objects $objects

# Optional compatibility flag; floor clearance is now automatic.
.\scripts\send-decorations.ps1 -Objects $objects -KeepAboveGround

# Snap to the currently selected editor grid.
.\scripts\send-decorations.ps1 -Objects $objects -SnapToGrid

# Preserve exact coordinates only when off-grid placement is intentional.
.\scripts\send-decorations.ps1 -Objects $objects -ExactCoordinates
```

Read the open level, update an object, delete another, and save the local
level from PowerShell:

```powershell
$level = Invoke-RestMethod "http://127.0.0.1:8765/api/level?offset=0&limit=100"
$level.level
$level.objects
$nativeLevel = Invoke-RestMethod "http://127.0.0.1:8765/api/level/string"
$nativeLevel.levelString
```

To check that the minimized-game snapshot endpoint responds:

```powershell
(curl -UseBasicParsing "http://127.0.0.1:8765/api/level?offset=0&limit=100").Content |
    ConvertFrom-Json |
    ConvertTo-Json -Depth 10

Invoke-RestMethod "http://127.0.0.1:8765/api/objects/edit" `
    -Method Post -ContentType "application/json" `
    -Body (@{ objects = @(@{ index = 0; x = 300; y = 120; rotation = 15 }) } | ConvertTo-Json -Depth 5)

Invoke-RestMethod "http://127.0.0.1:8765/api/objects/delete" `
    -Method Post -ContentType "application/json" `
    -Body '{"indices":[1]}'

Invoke-RestMethod "http://127.0.0.1:8765/api/save" -Method Post
```

An AI client can use these same HTTP endpoints or `Invoke-RestMethod`. This is
a small REST API, not an MCP protocol server.

## Build and install

1. Install Geode CLI with `winget install GeodeSDK.GeodeCLI`.
2. In an Administrator session, install Visual Studio 2022 Build Tools with
   the **Desktop development with C++** workload, including a Windows SDK
   and CMake.
3. Open a new terminal and install the SDK with `geode sdk install`.
4. From this project folder, build for your Geode profile:

   ```powershell
   geode build
   ```

5. If the CLI does not install it automatically, copy the generated
   `build\*.geode` package into:

   ```text
   C:\Program Files (x86)\Steam\steamapps\common\Geometry Dash\geode\mods
   ```

6. Restart Geometry Dash and confirm **AI Level Connector** is enabled in
   Geode's mod list.

The target metadata matches the inspected installation (Geode 5.9.0 and
Geometry Dash 2.2081).

## Publish for the Geode Index

This project includes Windows GitHub Actions workflows to build pull requests
and branches, and to create a downloadable `.geode` package for version tags.
The release workflow checks that the tag matches `mod.json`.

1. Create a **public** GitHub repository and push this source into it. Keep
   `build/` out of the repository; GitHub Actions creates the installable
   package for releases.
2. When releasing an update, change the version in `mod.json`, commit that
   change, then create and push a matching version tag (for example,
   `git tag v1.6.0` followed by `git push origin v1.6.0`). GitHub Actions builds
   the package and attaches it to a GitHub release.
3. Install/update Geode CLI and authenticate with `geode index login`.
4. Submit the direct `.geode` release-asset URL using
   `geode index mods create "<direct GitHub release asset URL>"`. Geode Index
   staff review new submissions before they become available in the in-game
   download browser.

The mod is Windows-only and declares Geode 5.9.0 / Geometry Dash 2.2081 support.
Confirm the current Geode Index submission guidelines before submitting.
