# 6) Drop rdpei's upstream ~20ms coalescing floor: flush on every wake-up. The rate is set by the
# ArkTS coalescer (services/input/RateCoalescer); steps 26/28 keep emitted frames legal.
$rdpeiMain = "$Source\channels\rdpei\client\rdpei_main.c"
Patch-File $rdpeiMain @{
  '/* Send an event every ~20ms */' = '/* Flush on every wake-up; the ArkTS coalescer owns the rate. */'
  '(now < rdpei->lastPollEventTime) || (now - rdpei->lastPollEventTime < 20ULL)' = 'now < rdpei->lastPollEventTime'
}
