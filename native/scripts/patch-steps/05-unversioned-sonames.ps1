# 5) Build unversioned libX.so libraries (no libX.so.3 suffix). Replaces the
#    upstream AddTargetWithResourceFile.cmake so that, with
#    -DWITH_LIBRARY_VERSIONING=OFF (see build-freerdp.ps1), the non-versioning
#    branch keeps the "lib" prefix and emits an explicit SONAME on OHOS/Linux.
Copy-PatchData (Join-Path $PatchData "AddTargetWithResourceFile.cmake") `
  "$Source\cmake\AddTargetWithResourceFile.cmake"

