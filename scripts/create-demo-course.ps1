[CmdletBinding()]
param(
    [ValidateNotNullOrEmpty()]
    [string] $BaseUrl = "http://127.0.0.1:8765",

    [ValidateRange(12, 200)]
    [int] $BlockCount = 48
)

$ErrorActionPreference = "Stop"
$status = Invoke-RestMethod -Uri "$BaseUrl/api/status" -TimeoutSec 5
if (-not $status.editorSnapshotAvailable) {
    throw "Open a blank local level in the editor and wait for its snapshot before creating the demo course."
}

$level = Invoke-RestMethod -Uri "$BaseUrl/api/level?offset=0&limit=1" -TimeoutSec 5
if ($level.snapshotAgeMs -gt 2000) {
    throw "Geometry Dash's editor is paused or minimized (snapshot age $($level.snapshotAgeMs) ms). Restore the editor and retry before queuing placements."
}
if ($level.level.objectCount -ne 0) {
    throw "Refusing to add the demo course: the active level '$($level.level.name)' already has $($level.level.objectCount) objects. Open a blank scratch level first."
}

$grid = [double]$level.level.gridSize
if ($grid -ne $grid -or [double]::IsInfinity($grid) -or $grid -lt 1) {
    throw "The editor reported an invalid grid size: $grid"
}

$objects = [System.Collections.Generic.List[object]]::new()
for ($index = 0; $index -lt $BlockCount; $index++) {
    $objects.Add(@{
        id = 1
        x = $index * $grid
        y = 0
        snapToGrid = $true
        groundOffsetTiles = 0
    })
}

$spikeIndices = @(
    [Math]::Floor($BlockCount * 0.2),
    [Math]::Floor($BlockCount * 0.4),
    [Math]::Floor($BlockCount * 0.62),
    [Math]::Floor($BlockCount * 0.82)
) | Select-Object -Unique
foreach ($index in $spikeIndices) {
    $objects.Add(@{
        id = 8
        x = $index * $grid
        y = 0
        snapToGrid = $true
        groundOffsetTiles = 1
    })
}

$payload = @{
    objects = @($objects)
    ensureGroundPath = $false
} | ConvertTo-Json -Depth 5
$queued = Invoke-RestMethod `
    -Uri "$BaseUrl/api/objects" `
    -Method Post `
    -ContentType "application/json" `
    -Body $payload `
    -TimeoutSec 10
if (-not $queued.queued) {
    throw "Geometry Dash did not queue the demo course placement."
}

$expected = "Placed $($objects.Count) object(s)"
$deadline = [DateTime]::UtcNow.AddSeconds(20)
do {
    Start-Sleep -Seconds 2
    $logs = (Invoke-RestMethod -Uri "$BaseUrl/api/logs?limit=10" -TimeoutSec 5).logs
    $result = $logs | Where-Object { $_ -like "$expected*" } | Select-Object -Last 1
} while (-not $result -and [DateTime]::UtcNow -lt $deadline)

if (-not $result) {
    throw "Placement was queued, but the game did not finish it within 20 seconds. Do not rerun yet: restore the editor, then check $BaseUrl/api/logs because the queued placement may still execute."
}

$after = Invoke-RestMethod -Uri "$BaseUrl/api/level?offset=0&limit=1" -TimeoutSec 5
Write-Output "Added a $BlockCount-block ground-backed course with $($spikeIndices.Count) spaced spikes to '$($after.level.name)'."
Write-Output "Grid: $($after.level.gridSize); ground Y: $($after.level.groundY); total objects: $($after.level.objectCount)."
Write-Output $result
Write-Output "Inspect it in the editor, then save manually if you want to keep it."
