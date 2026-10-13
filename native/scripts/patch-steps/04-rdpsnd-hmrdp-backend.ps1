# 4) Replace the rdpsnd OpenSL ES backend with the HmRdp one: OHOS's deprecated shim proved
#    fragile, so this decodes to 16-bit PCM and hands it to HmrdpSetAudioSink (ArkTS AudioRenderer).
foreach ($pair in @(
    @("rdpsnd_opensles.c", "$Source\channels\rdpsnd\client\opensles\rdpsnd_opensles.c"),
    @("rdpsnd_opensl_io.c", "$Source\channels\rdpsnd\client\opensles\opensl_io.c"),
    @("rdpsnd_opensl_io.h", "$Source\channels\rdpsnd\client\opensles\opensl_io.h")
  )) {
  Copy-PatchData (Join-Path $PatchData $pair[0]) $pair[1]
}

