# 5) Replace AddTargetWithResourceFile.cmake so unversioned libX.so keeps the "lib" prefix and sets SONAME on OHOS/Linux.
Copy-PatchData (Join-Path $PatchData "AddTargetWithResourceFile.cmake") `
  "$Source\cmake\AddTargetWithResourceFile.cmake"

