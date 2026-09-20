if ($PSVersionTable.PSVersion.Major -lt 7) {
    throw 'PowerShell 7+ is required.'
}

# Ported from Anomaly's tools/release_artifacts.ps1.  Two helpers, both with a reason:
#
#   * New-DeterministicZip -- entries sorted by ordinal relative path, a fixed 1980-01-01
#     DOS timestamp, no external attributes.  A release archive whose bytes change when
#     nothing changed cannot be attested or compared, which is the whole point of the
#     checksum inventory package_release.ps1 builds around it.
#   * New-SpdxSbom -- one SPDX 2.3 document per component, listing every staged file with
#     SHA1 + SHA256, the package verification code, and the third-party dependencies that
#     component actually contains.

Add-Type -AssemblyName System.IO.Compression

function New-DeterministicZip {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$SourceDirectory,
        [Parameter(Mandatory)][string]$DestinationPath
    )

    $source = [IO.Path]::GetFullPath($SourceDirectory).TrimEnd('\', '/')
    $destination = [IO.Path]::GetFullPath($DestinationPath)
    if (-not (Test-Path -LiteralPath $source -PathType Container)) {
        throw "archive source directory not found: $source"
    }
    $parent = [IO.Path]::GetDirectoryName($destination)
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    if (Test-Path -LiteralPath $destination) {
        Remove-Item -LiteralPath $destination -Force
    }

    $relativePaths = [string[]]@(Get-ChildItem -LiteralPath $source -Recurse -Force -File |
        ForEach-Object {
            [IO.Path]::GetRelativePath($source, $_.FullName).Replace('\', '/')
        })
    [Array]::Sort($relativePaths, [StringComparer]::Ordinal)

    $stream = [IO.File]::Open(
        $destination, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write,
        [IO.FileShare]::None)
    $archive = [IO.Compression.ZipArchive]::new(
        $stream, [IO.Compression.ZipArchiveMode]::Create, $false,
        [Text.Encoding]::UTF8)
    try {
        $timestamp = [DateTimeOffset]::new(1980, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
        foreach ($relative in $relativePaths) {
            $entry = $archive.CreateEntry(
                $relative, [IO.Compression.CompressionLevel]::Optimal)
            $entry.LastWriteTime = $timestamp
            $entry.ExternalAttributes = 0
            $entryStream = $entry.Open()
            $sourcePath = Join-Path $source ($relative.Replace(
                '/', [IO.Path]::DirectorySeparatorChar))
            $sourceStream = [IO.File]::OpenRead($sourcePath)
            try {
                $sourceStream.CopyTo($entryStream)
            } finally {
                $sourceStream.Dispose()
                $entryStream.Dispose()
            }
        }
    } finally {
        $archive.Dispose()
        $stream.Dispose()
    }
}

function Get-CabbirdSbomDependencies([string]$Component) {
    # Versions here are the pins in CMakeLists.txt's FetchContent block and the sources are
    # the same upstream tags NOTICE names.  They are listed per component because a
    # dependency belongs to the archive that actually contains its object code: the runtime
    # statically links all five, cabbird-cli (Tools) links none of them, and the SDK is
    # headers and CMake files only.
    $all = @(
        [pscustomobject]@{
            Id = 'SPDXRef-Dependency-imgui'; Name = 'Dear ImGui'; Version = '1.91.9b'
            License = 'MIT'; Source = 'https://github.com/ocornut/imgui/tree/v1.91.9b'
        },
        [pscustomobject]@{
            Id = 'SPDXRef-Dependency-minhook'; Name = 'MinHook'; Version = '1.3.4'
            License = 'BSD-2-Clause'; Source = 'https://github.com/TsudaKageyu/minhook/tree/v1.3.4'
        },
        [pscustomobject]@{
            Id = 'SPDXRef-Dependency-nlohmann-json'; Name = 'JSON for Modern C++'; Version = '3.11.3'
            License = 'MIT'; Source = 'https://github.com/nlohmann/json/tree/v3.11.3'
        },
        [pscustomobject]@{
            Id = 'SPDXRef-Dependency-json-schema-validator'; Name = 'Modern C++ JSON schema validator'; Version = '2.3.0'
            License = 'MIT'; Source = 'https://github.com/pboettch/json-schema-validator/tree/2.3.0'
        },
        [pscustomobject]@{
            Id = 'SPDXRef-Dependency-miniz'; Name = 'miniz'; Version = '3.0.2'
            License = 'MIT'; Source = 'https://github.com/richgel999/miniz/releases/tag/3.0.2'
        }
    )
    switch ($Component) {
        'Runtime' { return $all }
        default { return @() }
    }
}

function New-SpdxSbom {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$PackageDirectory,
        [Parameter(Mandatory)][ValidateSet('Runtime', 'SDK', 'Tools', 'Symbols')]
        [string]$Component,
        [Parameter(Mandatory)][string]$Version,
        [Parameter(Mandatory)][ValidatePattern('^[0-9a-f]{40}$')]
        [string]$SourceCommit,
        [Parameter(Mandatory)][DateTimeOffset]$SourceCommitUtc,
        [Parameter(Mandatory)][string]$OutputPath
    )

    $package = [IO.Path]::GetFullPath($PackageDirectory).TrimEnd('\', '/')

    $fileRows = [Collections.Generic.List[object]]::new()
    $sha1Values = [Collections.Generic.List[string]]::new()
    $relationships = [Collections.Generic.List[object]]::new()
    $rootPackageId = 'SPDXRef-Package-Cabbird'
    $index = 0
    $inventory = [object[]]@(Get-ChildItem -LiteralPath $package -Recurse -Force -File |
        ForEach-Object {
            [pscustomobject]@{
                Path = [IO.Path]::GetRelativePath($package, $_.FullName).Replace('\', '/')
                Sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            }
        })
    [Array]::Sort($inventory, [Comparison[object]]{
        param($left, $right)
        [StringComparer]::Ordinal.Compare([string]$left.Path, [string]$right.Path)
    })
    foreach ($item in $inventory) {
        ++$index
        $path = Join-Path $package ($item.Path.Replace(
            '/', [IO.Path]::DirectorySeparatorChar))
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "SBOM inventory file is missing: $($item.Path)"
        }
        $sha1 = (Get-FileHash -LiteralPath $path -Algorithm SHA1).Hash.ToLowerInvariant()
        $sha1Values.Add($sha1)
        $fileId = "SPDXRef-File-$index"
        $fileRows.Add([pscustomobject]@{
            fileName = "./$($item.Path)"
            SPDXID = $fileId
            checksums = @(
                [pscustomobject]@{ algorithm = 'SHA1'; checksumValue = $sha1 },
                [pscustomobject]@{ algorithm = 'SHA256'; checksumValue = $item.Sha256 }
            )
            licenseConcluded = 'NOASSERTION'
            copyrightText = 'NOASSERTION'
        })
        $relationships.Add([pscustomobject]@{
            spdxElementId = $rootPackageId
            relationshipType = 'CONTAINS'
            relatedSpdxElement = $fileId
        })
    }
    $orderedSha1 = $sha1Values.ToArray()
    [Array]::Sort($orderedSha1, [StringComparer]::Ordinal)
    $verificationBytes = [Text.Encoding]::ASCII.GetBytes(($orderedSha1 -join ''))
    $verificationCode = [Convert]::ToHexString(
        [Security.Cryptography.SHA1]::HashData($verificationBytes)).ToLowerInvariant()

    $dependencyPackages = [Collections.Generic.List[object]]::new()
    foreach ($dependency in @(Get-CabbirdSbomDependencies $Component)) {
        $dependencyPackages.Add([pscustomobject]@{
            name = $dependency.Name
            SPDXID = $dependency.Id
            versionInfo = $dependency.Version
            downloadLocation = $dependency.Source
            filesAnalyzed = $false
            licenseConcluded = $dependency.License
            licenseDeclared = $dependency.License
            copyrightText = 'See NOTICE and third_party/licenses in the release archive.'
        })
        $relationships.Add([pscustomobject]@{
            spdxElementId = $rootPackageId
            relationshipType = 'DEPENDS_ON'
            relatedSpdxElement = $dependency.Id
        })
    }

    $created = $SourceCommitUtc.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    $componentSlug = $Component.ToLowerInvariant()
    # documentNamespace is an IDENTIFIER, not a link: it only has to be a unique absolute
    # URI.  Upstream uses its own domain; this project has no published one yet, so the
    # value stays in the same shape and is swapped here when it does.
    $document = [pscustomobject]@{
        spdxVersion = 'SPDX-2.3'
        dataLicense = 'CC0-1.0'
        SPDXID = 'SPDXRef-DOCUMENT'
        name = "Cabbird-$Version-$componentSlug"
        documentNamespace = "https://cabbird.invalid/spdx/$Version/$componentSlug/$SourceCommit"
        creationInfo = [pscustomobject]@{
            created = $created
            creators = @('Tool: Cabbird tools/release_artifacts.ps1')
        }
        documentDescribes = @($rootPackageId)
        packages = @(
            [pscustomobject]@{
                name = "Cabbird $Component"
                SPDXID = $rootPackageId
                versionInfo = $Version
                downloadLocation = 'NOASSERTION'
                filesAnalyzed = $true
                packageVerificationCode = [pscustomobject]@{
                    packageVerificationCodeValue = $verificationCode
                }
                licenseConcluded = 'NOASSERTION'
                licenseDeclared = 'AGPL-3.0-only'
                copyrightText = 'NOASSERTION'
                sourceInfo = "Built from Git commit $SourceCommit."
            }
        ) + @($dependencyPackages)
        files = @($fileRows)
        relationships = @($relationships)
    }

    $output = [IO.Path]::GetFullPath($OutputPath)
    $parent = [IO.Path]::GetDirectoryName($output)
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    $json = ($document | ConvertTo-Json -Depth 10).Replace("`r`n", "`n") + "`n"
    [IO.File]::WriteAllText($output, $json, [Text.UTF8Encoding]::new($false))
}
