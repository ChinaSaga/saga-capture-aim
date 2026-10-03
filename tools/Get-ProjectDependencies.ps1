# Read the same SDK paths used by MSBuild; no machine-specific dependency paths.
function Get-ProjectDependencies {
    param([string]$ProjectRoot = (Split-Path $PSScriptRoot -Parent))
    [xml]$props = Get-Content -LiteralPath (Join-Path $ProjectRoot 'Dependencies.props') -Raw
    $values = @{ MSBuildThisFileDirectory = [IO.Path]::GetFullPath($ProjectRoot).TrimEnd('\','/') + '\' }
    $values.CUDA_PATH = $env:CUDA_PATH
    if (!$values.CUDA_PATH) { $values.CUDA_PATH = '' }
    foreach ($node in $props.Project.PropertyGroup.ChildNodes) {
        if ($node.NodeType -ne [System.Xml.XmlNodeType]::Element) { continue }
        $value = $node.InnerText
        foreach ($key in @($values.Keys)) { $value = $value.Replace('$(' + $key + ')', $values[$key]) }
        $values[$node.Name] = $value
    }
    return $values
}
