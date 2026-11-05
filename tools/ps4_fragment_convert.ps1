param(
    [Parameter(Mandatory=$true)][string]$Source,
    [Parameter(Mandatory=$true)][string]$Output,
    # CodeWalker.Core.dll: -CodeWalker, else the CODEWALKER_DLL environment variable.
    [string]$CodeWalker = $env:CODEWALKER_DLL
)

$ErrorActionPreference = 'Stop'

function Assert-LegacyFragmentHeader([byte[]]$Data) {
    # YftFile.GetVersion(false) returns a constant; it does not inspect the input.
    if ($Data.Length -lt 16 -or [BitConverter]::ToUInt32($Data, 0) -ne 0x37435352) {
        throw 'input is not an RSC7 resource'
    }
    $version = [BitConverter]::ToUInt32($Data, 4)
    if ($version -ne 162) { throw "input is not a Legacy fragment (version $version)" }
}

function Get-Ps4DeclarationTypes([uint32]$Flags, [uint64]$Types) {
    if (($Flags -band 4) -eq 0) { return $Types }
    # Legacy PC marks BlendIndices as Colour (9). Stock PS4 Adder/Asea use
    # UByte4 (8): integer bone indices, not normalized colour channels. Both
    # occupy four bytes; the payload, stride and every other component stay intact.
    $indexType = ($Types -shr 8) -band 15
    if ($indexType -eq 8) { return $Types }
    if ($indexType -ne 9) { throw "unsupported BlendIndices component type $indexType" }
    return [uint64]($Types - 0x100)
}

function Get-BytesHash([byte[]]$Data) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($Data)).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Get-FragmentBlocks($Fragment) {
    if ($null -eq $Fragment) { throw 'resource has no fragment' }
    $system = $null
    $graphics = $null
    # Use the serializer's reference/embedded-part walk. This covers main,
    # array, cloth and physics-child drawables, every LOD and the extra list,
    # deduplicating shared declarations and buffers by object identity.
    [CodeWalker.GameFiles.ResourceBuilder]::GetBlocks($Fragment, [ref]$system, [ref]$graphics)
    return @($system) + @($graphics)
}

function Convert-FragmentDeclarations($Blocks) {
    $changed = 0
    $declarations = 0
    foreach ($block in $Blocks) {
        if ($block -isnot [CodeWalker.GameFiles.VertexDeclaration]) { continue }
        $declarations++
        $types = [uint64]$block.Types
        $converted = Get-Ps4DeclarationTypes $block.Flags $types
        if ($converted -ne $types) {
            $block.Types = [Enum]::ToObject($block.Types.GetType(), $converted)
            $changed++
        }
    }
    return @{ declarations = $declarations; changed = $changed }
}

function Get-GeometryFingerprint($Blocks) {
    $records = [Collections.Generic.List[string]]::new()
    foreach ($block in $Blocks) {
        if ($block -is [CodeWalker.GameFiles.VertexDeclaration]) {
            $types = [uint64]$block.Types
            if ((Get-Ps4DeclarationTypes $block.Flags $types) -ne $types) {
                throw 'serialized fragment retains PC BlendIndices encoding'
            }
            $records.Add("declaration:$($block.Flags):$($block.Stride):$($block.Count):$($block.Unknown_6h):$types")
        } elseif ($block -is [CodeWalker.GameFiles.VertexBuffer]) {
            foreach ($slot in @('Data1', 'Data2')) {
                $data = $block.$slot
                if ($null -eq $data) {
                    $records.Add("vertices:${slot}:absent")
                    continue
                }
                if ($null -eq $data.VertexBytes -or $data.VertexBytes.Length -ne $data.VertexCount * $data.VertexStride) {
                    throw 'vertex payload length does not match count and stride'
                }
                $records.Add("vertices:${slot}:$($data.VertexCount):$($data.VertexStride):$(Get-BytesHash $data.VertexBytes)")
            }
        } elseif ($block -is [CodeWalker.GameFiles.IndexBuffer]) {
            if ($null -eq $block.Indices) { throw 'index buffer has no indices' }
            $bytes = [byte[]]::new($block.Indices.Length * 2)
            [Buffer]::BlockCopy($block.Indices, 0, $bytes, 0, $bytes.Length)
            $records.Add("indices:$($block.Indices.Length):$(Get-BytesHash $bytes)")
        }
    }
    # Include duplicates, so a lost buffer/declaration cannot hide behind a set.
    $records.Sort([StringComparer]::Ordinal)
    return [string]::Join("`n", $records)
}

if (-not $CodeWalker) { throw 'CodeWalker.Core.dll is not set: pass -CodeWalker PATH or set CODEWALKER_DLL' }
if (-not (Test-Path -LiteralPath $CodeWalker -PathType Leaf)) { throw "CodeWalker assembly not found: $CodeWalker" }
$inputPath = (Resolve-Path -LiteralPath $Source).Path
if (Test-Path -LiteralPath $Output) { throw "refusing to overwrite $Output" }
$sourceBytes = [IO.File]::ReadAllBytes($inputPath)
Assert-LegacyFragmentHeader $sourceBytes
Add-Type -Path $CodeWalker
[CodeWalker.GameFiles.RpfManager]::IsGen9 = $false

$yft = New-Object CodeWalker.GameFiles.YftFile
$yft.Load($sourceBytes)
$blocks = @(Get-FragmentBlocks $yft.Fragment)
$converted = Convert-FragmentDeclarations $blocks
$before = Get-GeometryFingerprint $blocks
$blob = $yft.Save()
Assert-LegacyFragmentHeader $blob
$check = New-Object CodeWalker.GameFiles.YftFile
$check.Load($blob)
$after = Get-GeometryFingerprint @(Get-FragmentBlocks $check.Fragment)
if ($before -cne $after) { throw 'fragment serialization changed geometry declarations or vertex/index payloads' }
$parent = Split-Path -Parent $Output
if ($parent) { [IO.Directory]::CreateDirectory($parent) | Out-Null }
# Publish only after all checks, and do not overwrite a file created concurrently.
$stream = [IO.File]::Open($Output, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
try { $stream.Write($blob, 0, $blob.Length) }
finally { $stream.Dispose() }
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Output).Hash.ToLowerInvariant()
Write-Output (@{
    input = $inputPath
    output = (Resolve-Path -LiteralPath $Output).Path
    version = 162
    bytes = $blob.Length
    sha256 = $hash
    vertexDeclarations = $converted.declarations
    blendIndicesConverted = $converted.changed
    geometryPayloadsPreserved = $true
    codeWalkerSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $CodeWalker).Hash.ToLowerInvariant()
} | ConvertTo-Json -Compress)
