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

