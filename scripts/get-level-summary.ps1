[CmdletBinding()]
param(
    [ValidateSet("Clusters", "Reachability", "Objects")]
    [string] $View = "Clusters",

    [ValidateRange(1, 100)]
    [int] $Limit = 30,

    [ValidateNotNullOrEmpty()]
    [string] $BaseUrl = "http://127.0.0.1:8765"
)

switch ($View) {
    "Clusters" {
        $result = Invoke-RestMethod `
            -Uri "$BaseUrl/api/level/clusters?limit=$Limit" `
            -Method Get
        Write-Output ("Level: {0} | Objects: {1} | Grid: {2} | Clusters: {3}" -f `
            $result.level, $result.objectCount, $result.gridSize, $result.clusterCount)
        $result.clusters |
            Select-Object index, x, y, objects, triggers, gapToNext |
            Format-Table -AutoSize
    }
    "Reachability" {
        $result = Invoke-RestMethod `
            -Uri "$BaseUrl/api/level/analyze?platformIds=1" `
            -Method Get
        Write-Output ("Level: {0} | Grid: {1} | Off-grid: {2} | Platforms: {3} | Reachable: {4}/{3}" -f `
            $result.level, $result.gridSize, $result.offGridObjects, `
            $result.platformCount, $result.reachablePlatforms)
        Write-Output ("Estimated cube route: {0}" -f `
            $(if ($result.estimatedReachable) { "reachable" } else { "possible gap" }))
        $result.unreachableGaps |
            Select-Object index, id, x, y, fromReachableX |
            Format-Table -AutoSize
    }
    "Objects" {
        $result = Invoke-RestMethod `
            -Uri "$BaseUrl/api/level?offset=0&limit=$Limit" `
            -Method Get
        Write-Output ("Level: {0} | Total objects: {1} | Showing: {2}/{1}" -f `
            $result.level.name, $result.level.objectCount, $result.count)
        $result.objects |
            Select-Object index, id, x, y |
            Format-Table -AutoSize
    }
}
