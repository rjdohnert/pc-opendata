function Publish-OpenDataRelease {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Stage,
        [Parameter(Mandatory)][string]$Destination,
        [Parameter(Mandatory)][string[]]$Names
    )

    $installed = [System.Collections.Generic.List[object]]::new()
    try {
        foreach ($name in $Names) {
            $source = Join-Path $Stage $name
            $target = Join-Path $Destination $name
            $backup = Join-Path $Stage ($name + '.previous')
            $existed = Test-Path -LiteralPath $target -PathType Leaf
            if ($existed) {
                [System.IO.File]::Replace($source, $target, $backup)
            } else {
                [System.IO.File]::Move($source, $target)
            }
            $installed.Add(@{ Target = $target; Backup = $backup; Existed = $existed })
        }
    }
    catch {
        $publishError = $_
        $rollbackErrors = @()
        for ($i = $installed.Count - 1; $i -ge 0; $i--) {
            $entry = $installed[$i]
            try {
                if ($entry.Existed) {
                    [System.IO.File]::Replace($entry.Backup, $entry.Target,
                        ($entry.Backup + '.rejected'))
                } else {
                    [System.IO.File]::Delete($entry.Target)
                }
            }
            catch {
                $rollbackErrors += $_.Exception.Message
            }
        }
        if ($rollbackErrors.Count) {
            throw "Release publication failed: $publishError. Rollback incomplete; preserve $Stage for recovery: $($rollbackErrors -join '; ')"
        }
        throw "Release publication failed; previous release restored: $publishError"
    }
}
