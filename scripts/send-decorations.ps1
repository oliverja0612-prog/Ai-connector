[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [object[]] $Objects,

    [switch] $KeepAboveGround,

    [switch] $SnapToGrid,

    [switch] $ExactCoordinates,

    [ValidateNotNullOrEmpty()]
    [string] $BaseUrl = "http://127.0.0.1:8765"
)

$requestObjects = @($Objects)
if ($KeepAboveGround -or $SnapToGrid -or $ExactCoordinates) {
    $requestObjects = foreach ($object in $Objects) {
        $copy = [ordered]@{}
        if ($object -is [System.Collections.IDictionary]) {
            foreach ($key in $object.Keys) {
                $copy[$key] = $object[$key]
            }
        } else {
            foreach ($property in $object.PSObject.Properties) {
                $copy[$property.Name] = $property.Value
            }
        }
        if ($KeepAboveGround) {
            $copy["keepAboveGround"] = $true
        }
        if ($SnapToGrid -or $ExactCoordinates) {
            $copy["snapToGrid"] = -not $ExactCoordinates
        }
        $copy
    }
}

$payload = @{ objects = @($requestObjects) } | ConvertTo-Json -Depth 5
Invoke-RestMethod `
    -Uri "$BaseUrl/api/objects" `
    -Method Post `
    -ContentType "application/json" `
    -Body $payload
