# Fetches the external native dependencies (ImGui + MinHook) into
# native/third_party/ so that IcarusInternal.vcxproj can compile.
#
# This is the same logic the CI workflow runs (.github/workflows/release.yml);
# keep them in sync.

[CmdletBinding()]
param(
    [string]$ImGuiTag = 'v1.91.8',
    [string]$MinHookBranch = 'master'
)

$ErrorActionPreference = 'Stop'

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
$libs = Join-Path $repoRoot 'native\third_party'

New-Item -ItemType Directory -Force -Path (Join-Path $libs 'imgui')            | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $libs 'minhook\include')  | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $libs 'minhook\src\hde')  | Out-Null

$tmp = $env:TEMP
if (-not $tmp) { $tmp = [System.IO.Path]::GetTempPath() }

function Fetch-Zip([string]$url, [string]$zipPath, [string]$extractPath) {
    if (Test-Path $extractPath) { Remove-Item $extractPath -Recurse -Force }
    Write-Host "Downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $zipPath
    Expand-Archive -Path $zipPath -DestinationPath $extractPath -Force
    (Get-ChildItem $extractPath -Directory | Select-Object -First 1).FullName
}

# --- ImGui ---
$imguiSrc = Fetch-Zip "https://github.com/ocornut/imgui/archive/refs/tags/$ImGuiTag.zip" `
                     (Join-Path $tmp 'imgui.zip') `
                     (Join-Path $tmp 'imgui-extract')

$imguiCore = @(
    'imgui.h','imgui.cpp','imgui_internal.h','imconfig.h',
    'imgui_draw.cpp','imgui_tables.cpp','imgui_widgets.cpp','imgui_demo.cpp',
    'imstb_rectpack.h','imstb_textedit.h','imstb_truetype.h'
)
foreach ($f in $imguiCore) {
    $p = Join-Path $imguiSrc $f
    if (Test-Path $p) { Copy-Item $p (Join-Path $libs 'imgui') -Force }
}

$imguiBackends = @(
    'imgui_impl_dx11.h','imgui_impl_dx11.cpp',
    'imgui_impl_win32.h','imgui_impl_win32.cpp'
)
foreach ($f in $imguiBackends) {
    Copy-Item (Join-Path $imguiSrc "backends\$f") (Join-Path $libs 'imgui') -Force
}

# --- MinHook ---
$mhSrc = Fetch-Zip "https://github.com/TsudaKageyu/minhook/archive/refs/heads/$MinHookBranch.zip" `
                   (Join-Path $tmp 'minhook.zip') `
                   (Join-Path $tmp 'minhook-extract')

Copy-Item (Join-Path $mhSrc 'include\MinHook.h')  (Join-Path $libs 'minhook\include') -Force
Copy-Item (Join-Path $mhSrc 'src\buffer.c')       (Join-Path $libs 'minhook\src') -Force
Copy-Item (Join-Path $mhSrc 'src\buffer.h')       (Join-Path $libs 'minhook\src') -Force
Copy-Item (Join-Path $mhSrc 'src\hook.c')         (Join-Path $libs 'minhook\src') -Force
Copy-Item (Join-Path $mhSrc 'src\trampoline.c')   (Join-Path $libs 'minhook\src') -Force
Copy-Item (Join-Path $mhSrc 'src\trampoline.h')   (Join-Path $libs 'minhook\src') -Force
Copy-Item (Join-Path $mhSrc 'src\hde\*')          (Join-Path $libs 'minhook\src\hde') -Recurse -Force

Write-Host ''
Write-Host "Fetched into $libs" -ForegroundColor Green
Write-Host ''
Write-Host '--- imgui ---'
Get-ChildItem (Join-Path $libs 'imgui') | Select-Object -ExpandProperty Name
Write-Host ''
Write-Host '--- minhook ---'
Get-ChildItem (Join-Path $libs 'minhook') -Recurse -File | ForEach-Object {
    $_.FullName.Substring($libs.Length + 1)
}
