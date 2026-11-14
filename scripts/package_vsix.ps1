Param(
    [string]$SourceDir = "E:\Github\pc-opendata\vscode-extension",
    [string]$OutputFile = "E:\Github\pc-opendata\pc-opendata-vscode-27.0.0.vsix"
)

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$tempDir = [System.IO.Path]::Combine([System.IO.Path]::GetTempPath(), "vsix_staging_" + [System.Guid]::NewGuid().ToString("N"))
$extensionDir = [System.IO.Path]::Combine($tempDir, "extension")

New-Item -ItemType Directory -Path $extensionDir -Force | Out-Null

# 1. Copy extension files into extension/ directory
Copy-Item -Path "$SourceDir\package.json" -Destination "$extensionDir\package.json" -Force
Copy-Item -Path "$SourceDir\README.md" -Destination "$extensionDir\README.md" -Force
Copy-Item -Path "$SourceDir\src" -Destination "$extensionDir\src" -Recurse -Force
Copy-Item -Path "$SourceDir\resources" -Destination "$extensionDir\resources" -Recurse -Force
if (Test-Path "E:\Github\pc-opendata\LICENSE") {
    Copy-Item -Path "E:\Github\pc-opendata\LICENSE" -Destination "$extensionDir\LICENSE" -Force
}

# 2. Generate [Content_Types].xml
$contentTypes = @'
<?xml version="1.0" encoding="utf-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="json" ContentType="application/json" />
  <Default Extension="vsixmanifest" ContentType="text/xml" />
  <Default Extension="png" ContentType="image/png" />
  <Default Extension="svg" ContentType="image/svg+xml" />
  <Default Extension="md" ContentType="text/markdown" />
  <Default Extension="js" ContentType="application/javascript" />
  <Default Extension="txt" ContentType="text/plain" />
</Types>
'@
[System.IO.File]::WriteAllText((Join-Path $tempDir "[Content_Types].xml"), $contentTypes, [System.Text.Encoding]::UTF8)

# 3. Generate extension.vsixmanifest
$vsixManifest = @'
<?xml version="1.0" encoding="utf-8"?>
<PackageManifest Version="2.0.0" xmlns="http://schemas.microsoft.com/developer/vsx-schema/2011">
  <Metadata>
    <Identity Id="pc-opendata-vscode" Version="27.0.0" Language="en-US" Publisher="pc-opensystems" />
    <DisplayName>PC/OpenData SQL Server &amp; Vector Studio</DisplayName>
    <Description xml:space="preserve">Native VS Code extension for PC/OpenData SQL Server: .pcod/.mdf/.dbf binary editor, SIMD Vector viewer, CodeLens query runner, and engine telemetry.</Description>
    <Tags>opendata,sql,vector,embeddings,mssql,oracle</Tags>
    <Categories>Programming Languages,Data Science,Database</Categories>
    <GalleryFlags>Public</GalleryFlags>
    <Icon>extension/resources/Designer.png</Icon>
    <License>extension/LICENSE</License>
  </Metadata>
  <Installation>
    <InstallationTarget Id="Microsoft.VisualStudio.Code" />
  </Installation>
  <Dependencies />
  <Assets>
    <Asset Type="Microsoft.VisualStudio.Code.Manifest" Path="extension/package.json" Addressable="true" />
    <Asset Type="Microsoft.VisualStudio.Services.Content.Details" Path="extension/README.md" Addressable="true" />
    <Asset Type="Microsoft.VisualStudio.Services.Icons.Default" Path="extension/resources/Designer.png" Addressable="true" />
    <Asset Type="Microsoft.VisualStudio.Services.Content.License" Path="extension/LICENSE" Addressable="true" />
  </Assets>
</PackageManifest>
'@
[System.IO.File]::WriteAllText((Join-Path $tempDir "extension.vsixmanifest"), $vsixManifest, [System.Text.Encoding]::UTF8)

# 4. Pack into standard OPC Zip Archive (.vsix)
if (Test-Path $OutputFile) { Remove-Item $OutputFile -Force }

[System.IO.Compression.ZipFile]::CreateFromDirectory($tempDir, $OutputFile, [System.IO.Compression.CompressionLevel]::Optimal, $false)

# 5. Cleanup
Remove-Item -Path $tempDir -Recurse -Force

Write-Host "VSIX package created successfully at: $OutputFile"
Get-Item $OutputFile | Select-Object Name, Length, LastWriteTime
