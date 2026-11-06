param([switch]$Apply)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent)).TrimEnd('\')
$projectPrefix = $projectRoot + '\'
foreach ($required in @('SagaApp.vcxproj', 'src\App.h', 'web\圣人视觉识别系统.html')) {
    if (!(Test-Path -LiteralPath (Join-Path $projectRoot $required) -PathType Leaf)) {
        throw "Project marker missing: $required"
    }
}

# Explicitly scoped historical outputs. Models, active SDKs, named presets,
# human training data, source, web assets and the current EXEs are excluded.
$relativeTargets = @(
    '.workbuddy', '.vs', '.deps\downloads', '.deps\onnxruntime-dml-1.24.4',
    'x64\Release\崩溃报告', 'x64\Release\轨迹采集', '人手轨迹\__pycache__',
    'x64\Release\运行日志.txt', 'x64\Release\主机IP.ini', 'x64\Release\软件包.7z',
    '.git\codex-ref-backups', '.git\ORIG_HEAD', '.git\FETCH_HEAD'
)
$targets = @()
[long]$totalBytes = 0
foreach ($relative in $relativeTargets) {
    $target = [IO.Path]::GetFullPath((Join-Path $projectRoot $relative))
    if (!$target.StartsWith($projectPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Target is outside this project: $target"
    }
    # Validate every existing ancestor, not just the final directory.
    $ancestor = $target
    while ($ancestor -ne $projectRoot) {
        if (Test-Path -LiteralPath $ancestor) {
            $item = Get-Item -LiteralPath $ancestor -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Refusing to follow a filesystem link: $ancestor"
            }
        }
        $ancestor = Split-Path $ancestor -Parent
    }
    if (!(Test-Path -LiteralPath $target)) { continue }
    $item = Get-Item -LiteralPath $target -Force
    if ($item.PSIsContainer) {
        $children = @(Get-ChildItem -LiteralPath $target -Recurse -Force)
        if (@($children | Where-Object { $_.PSIsContainer -and ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) }).Count) {
            throw "Target contains a directory link: $target"
        }
        $bytes = ($children | Where-Object { !$_.PSIsContainer } | Measure-Object Length -Sum).Sum
    } else { $bytes = $item.Length }
    $totalBytes += $bytes
    $targets += $item
    Write-Output ('{0,9:N3} GiB  {1}' -f ($bytes / 1GB), $target)
}
Write-Output ('Total historical files: {0:N3} GiB' -f ($totalBytes / 1GB))
Write-Output 'Local Git history will be replaced by one current snapshot. Remote repositories are not modified.'
if (!$Apply) {
    Write-Output 'Preview only. To apply this exact project cleanup: ./tools/clear_project_history.ps1 -Apply'
    return
}

function Invoke-ProjectGit {
    & git -C $projectRoot @args
    if ($LASTEXITCODE) { throw "Git failed ($LASTEXITCODE): $args" }
}
$gitRoot = [IO.Path]::GetFullPath((Invoke-ProjectGit rev-parse --show-toplevel).Trim()).TrimEnd('\')
if ($gitRoot -ne $projectRoot) { throw 'The project must own its Git repository.' }
if (!(Test-Path -LiteralPath (Join-Path $projectRoot '.git') -PathType Container)) {
    throw 'Linked worktrees cannot perform history cleanup.'
}
Invoke-ProjectGit worktree prune --expire=now
$worktrees = @(Invoke-ProjectGit worktree list --porcelain | Where-Object { $_ -like 'worktree *' })
if ($worktrees.Count -ne 1) { throw 'Other live worktrees exist; refusing to invalidate their history.' }
$null = Invoke-ProjectGit var GIT_AUTHOR_IDENT
$branch = (Invoke-ProjectGit symbolic-ref --short HEAD).Trim()
$oldRefs = @(Invoke-ProjectGit for-each-ref '--format=%(refname)')

# In-memory verification only; do not create another backup or history report.
$protected = @()
foreach ($relative in @('src', 'tools', 'web', '人手轨迹', 'UI界面', 'x64\Release')) {
    $directory = Join-Path $projectRoot $relative
    if (!(Test-Path -LiteralPath $directory)) { continue }
    foreach ($file in Get-ChildItem -LiteralPath $directory -Recurse -File -Force) {
        $willDelete = $false
        foreach ($target in $targets) {
            if ($file.FullName -eq $target.FullName -or
                $file.FullName.StartsWith($target.FullName + '\', [StringComparison]::OrdinalIgnoreCase)) {
                $willDelete = $true
                break
            }
        }
        if (!$willDelete) {
            $protected += [pscustomobject]@{ Path = $file.FullName; Hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash }
        }
    }
}

# Save the current tree before making old commits unreachable. The orphan
# checkout retains working files, including uncommitted changes.
$snapshotBranch = 'codex/current-snapshot-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
Invoke-ProjectGit checkout --orphan $snapshotBranch
Invoke-ProjectGit add -u -- .
Invoke-ProjectGit add -- src tools web docs '人手轨迹'
Invoke-ProjectGit commit -q -m 'Current project snapshot'
# Remote HEAD may be symbolic: delete the pointer itself, not its target.
foreach ($reference in $oldRefs) { Invoke-ProjectGit update-ref --no-deref -d $reference }
Invoke-ProjectGit branch -m $branch
foreach ($target in $targets) {
    if ($target.PSIsContainer) { Remove-Item -LiteralPath $target.FullName -Recurse -Force }
    else { Remove-Item -LiteralPath $target.FullName -Force }
}
Invoke-ProjectGit reflog expire --expire=now --expire-unreachable=now --all
Invoke-ProjectGit gc --prune=now
if ((Invoke-ProjectGit rev-list --all --count).Trim() -ne '1') { throw 'Unexpected retained Git history.' }
foreach ($file in $protected) {
    if (!(Test-Path -LiteralPath $file.Path -PathType Leaf) -or
        (Get-FileHash -LiteralPath $file.Path -Algorithm SHA256).Hash -ne $file.Hash) {
        throw "Current project file changed unexpectedly: $($file.Path)"
    }
}
Write-Output "Cleanup complete. Verified $($protected.Count) unchanged current files; one current Git snapshot remains."
