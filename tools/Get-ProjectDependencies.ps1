# Read the same SDK paths used by MSBuild; no machine-specific dependency paths.
function Get-ProjectDependencies {
    param([string]$ProjectRoot = (Split-Path $PSScriptRoot -Parent))
    [xml]$props = Get-Content -LiteralPath (Join-Path $ProjectRoot 'Dependencies.props') -Raw
    $values = @{ MSBuildThisFileDirectory = [IO.Path]::GetFullPath($ProjectRoot).TrimEnd('\','/') + '\' }
    $values.CUDA_PATH = $env:CUDA_PATH
    if (!$values.CUDA_PATH) { $values.CUDA_PATH = '' }
    foreach ($node in $props.Project.PropertyGroup.ChildNodes) {
        if ($node.NodeType -ne [System.Xml.XmlNodeType]::Element) { continue }
        # These property defaults use only an empty-value check and optional
        # Exists() clauses. Honor them without evaluating arbitrary code.
        $condition = $node.GetAttribute('Condition')
        if ($condition) {
            $emptyCheck = "'`$($($node.Name))' == ''"
            if ($condition.Contains($emptyCheck) -and $values[$node.Name]) { continue }
            foreach ($key in @($values.Keys)) { $condition = $condition.Replace('$(' + $key + ')', $values[$key]) }
            $missing = $false
            foreach ($exists in [regex]::Matches($condition,"Exists\('([^']*)'\)")) {
                if (!(Test-Path -LiteralPath $exists.Groups[1].Value)) { $missing = $true; break }
            }
            if ($missing) { continue }
        }
        $value = $node.InnerText
        foreach ($key in @($values.Keys)) { $value = $value.Replace('$(' + $key + ')', $values[$key]) }
        $values[$node.Name] = $value
    }
    return $values
}
