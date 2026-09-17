# 2) Force client/common to be built as a shared library (default is static and
#    would not export the client-common symbols the NAPI wrapper needs).
Patch-File "$Source\client\common\CMakeLists.txt" @{
  'addtargetwithresourcefile(${MODULE_NAME} FALSE' = 'addtargetwithresourcefile(${MODULE_NAME} SHARED'
}

