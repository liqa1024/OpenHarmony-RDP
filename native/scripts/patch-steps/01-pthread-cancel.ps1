# 1) musl/OHOS has no pthread_cancel.
Patch-File "$Source\winpr\libwinpr\thread\thread.c" @{
  '#ifndef ANDROID' = '#if !defined(ANDROID) && !defined(__OHOS__)'
}

