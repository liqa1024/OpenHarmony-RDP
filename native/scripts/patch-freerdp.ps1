# Applies the HarmonyOS/musl compatibility patches to a FreeRDP source tree.
#
# Usage:  pwsh -File native/scripts/patch-freerdp.ps1 [-Source <path-to-FreeRDP>]
#
# The build pipeline expects the patched tree under native/third_party/FreeRDP
# (see native/scripts/build-freerdp.ps1). Run this script once after cloning
# FreeRDP 3.10.3 and before configuring the CMake build.
param(
  [string]$Source = "$PSScriptRoot\..\third_party\FreeRDP"
)

$ErrorActionPreference = "Stop"

function Patch-File {
  param([string]$Path, [hashtable]$Replacements)
  if (-not (Test-Path -LiteralPath $Path)) {
    throw "file not found: $Path"
  }
  $text = [System.IO.File]::ReadAllText($Path)
  foreach ($key in $Replacements.Keys) {
    $text = $text.Replace($key, $Replacements[$key])
  }
  [System.IO.File]::WriteAllText($Path, $text)
}

# 1) musl/OHOS has no pthread_cancel.
Patch-File "$Source\winpr\libwinpr\thread\thread.c" @{
  '#ifndef ANDROID' = '#if !defined(ANDROID) && !defined(__OHOS__)'
}

# 2) Force client/common to be built as a shared library (default is static and
#    would not export the client-common symbols the NAPI wrapper needs).
Patch-File "$Source\client\common\CMakeLists.txt" @{
  'addtargetwithresourcefile(${MODULE_NAME} FALSE' = 'addtargetwithresourcefile(${MODULE_NAME} SHARED'
}

# 3) OpenSLES: OHOS ships the standard OpenSLES 1.0.1 headers, not the Android
#    extensions, so map the Android-only identifiers to the standard ones.
$sl = @{
  '#include <SLES/OpenSLES_Android.h>'               = '#include <SLES/OpenSLES.h>'
  '#include <SLES/OpenSLES_AndroidConfiguration.h>'  = '#include <SLES/OpenSLES.h>'
  'SL_IID_ANDROIDSIMPLEBUFFERQUEUE'                  = 'SL_IID_BUFFERQUEUE'
  'SLAndroidSimpleBufferQueueItf'                    = 'SLBufferQueueItf'
  'SLDataLocator_AndroidSimpleBufferQueue'           = 'SLDataLocator_BufferQueue'
  'SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE'          = 'SL_DATALOCATOR_BUFFERQUEUE'
}
foreach ($file in @(
    "$Source\channels\rdpsnd\client\opensles\opensl_io.c",
    "$Source\channels\rdpsnd\client\opensles\opensl_io.h",
    "$Source\channels\audin\client\opensles\opensl_io.c",
    "$Source\channels\audin\client\opensles\opensl_io.h"
  )) {
  Patch-File $file $sl
}

# 4) OHOS does not implement the standard SL_IID_BUFFERQUEUE used by the
#    upstream rdpsnd OpenSL ES backend, only the OH-specific
#    SL_IID_OH_BUFFERQUEUE. Replace the audio output backend with an
#    OHOS-compatible one, otherwise playback silently produces no sound.
$Patches = "$PSScriptRoot\..\patches"
foreach ($pair in @(
    @("rdpsnd_opensl_io.c", "$Source\channels\rdpsnd\client\opensles\opensl_io.c"),
    @("rdpsnd_opensl_io.h", "$Source\channels\rdpsnd\client\opensles\opensl_io.h")
  )) {
  $from = Join-Path $Patches $pair[0]
  if (-not (Test-Path -LiteralPath $from)) {
    throw "patch source not found: $from"
  }
  Copy-Item -LiteralPath $from -Destination $pair[1] -Force
}

# 5) Build unversioned libX.so libraries (no libX.so.3 suffix). Replaces the
#    upstream AddTargetWithResourceFile.cmake so that, with
#    -DWITH_LIBRARY_VERSIONING=OFF (see build-freerdp.ps1), the non-versioning
#    branch keeps the "lib" prefix and emits an explicit SONAME on OHOS/Linux.
Copy-Item -LiteralPath (Join-Path $Patches "AddTargetWithResourceFile.cmake") `
  -Destination "$Source\cmake\AddTargetWithResourceFile.cmake" -Force

Write-Host "FreeRDP OHOS patches applied to $Source"
