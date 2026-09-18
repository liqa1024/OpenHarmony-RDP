# 24) HmRdp: runtime gate for the progressive-decode dev instrumentation.
#
#     背景：patch step 8b / 11 / 14 / 20 / 21 / 23 在 progressive.c 里埋了一组探针
#     （HmrdpProgStat 的分相计时、每 tile 的 1/16 相位采样、per-chunk 计时）。它们的
#     读者只有 app 的回放统计（`prog` / `prog2` 行），工具栏不展示；而其中
#     `hmrdp_now_ns()`（clock_gettime，本平台不是 vDSO）与
#     `HMRDP_PHASE_ARM()`（每 tile 一次共享计数器原子加）是常开的热路径开销。
#
#     这一步把这整组"读时钟"的探针变成**默认关闭**，由 app 用
#     `HmrdpSetProgSample(1)` 打开（回放 CPU 路线），live 会话保持 0：
#       a) `hmrdp_now_ns()` 在关闭时直接返回 0 —— 所有调用点都只是"两次读数相减"，
#          0-0 让对应槽保持 0，不改任何调用点的写法与格式；
#       b) `HMRDP_PHASE_ARM()` 在关闭时不碰每 tile 的共享采样计数器；
#       c) 纯计数（[4]/[5]/[6]/[8]/[18]/[19]/[20]）保持不变：它们不读时钟，量级可忽略。
#
#     注意 `HmrdpSetDwtCheck` 的逆 DWT 对拍是另一套（step 15），已经由 app 单独门控，
#     不在这里。
#
#     整块按"一次性整体打补丁"设计：改动它要从干净源码重打；本步对已打过 8b/11/20
#     的树同样幂等（带 marker，重跑跳过）。
function Patch-Regex {
  param([string]$Path, [string]$Pattern, [string]$Replacement, [string]$Marker)
  if (-not (Test-Path -LiteralPath $Path)) {
    throw "file not found: $Path"
  }
  $raw = [System.IO.File]::ReadAllText($Path)
  if ($Marker -and $raw.Contains($Marker)) {
    Write-Host "HmRdp prog probe gate already applied to $(Split-Path -Leaf $Path)"
    return
  }
  $crlf = $raw.Contains("`r`n")
  $text = $raw.Replace("`r`n", "`n")
  $Replacement = $Replacement.Replace("`r`n", "`n")
  $re = [regex]::new($Pattern, [System.Text.RegularExpressions.RegexOptions]::Singleline)
  if (-not $re.IsMatch($text)) {
    throw "HmRdp prog probe gate: pattern not found in $Path"
  }
  $evaluator = [System.Text.RegularExpressions.MatchEvaluator] { param($m) $Replacement }
  $text = $re.Replace($text, $evaluator, 1)
  if ($crlf) {
    $text = $text.Replace("`n", "`r`n")
  }
  [System.IO.File]::WriteAllText($Path, $text)
  Write-Host "HmRdp prog probe gate applied to $(Split-Path -Leaf $Path)"
}

$progC = "$Source\libfreerdp\codec\progressive.c"

# (a) the gate itself + hmrdp_now_ns() short-circuit.
Patch-Regex $progC `
  'static INLINE unsigned long long hmrdp_now_ns\(void\)\n\{\n\tstruct timespec ts;\n\tclock_gettime\(CLOCK_MONOTONIC, &ts\);\n\treturn \(\(unsigned long long\)ts\.tv_sec \* 1000000000ull\) \+ \(unsigned long long\)ts\.tv_nsec;\n\}' (@'
/*
 * HmRdp: the progressive decode's timing probes are read back only by the app's
 * replay statistics (`prog` / `prog2`); a live session never displays them.
 * clock_gettime is not a vDSO call on this platform, so the whole group is off
 * unless the app asks for it (HmrdpSetProgSample(1), the CPU replay route) and a
 * live session pays neither the clock reads nor the per-tile sampling counter.
 */
static volatile int g_HmrdpProgSample = 0;

FREERDP_API void HmrdpSetProgSample(int on)
{
	__atomic_store_n(&g_HmrdpProgSample, on ? 1 : 0, __ATOMIC_RELAXED);
}

FREERDP_API int HmrdpGetProgSample(void)
{
	return __atomic_load_n(&g_HmrdpProgSample, __ATOMIC_RELAXED) != 0;
}

static INLINE unsigned long long hmrdp_now_ns(void)
{
	struct timespec ts;

	/* HmRdp: 0 while the instrumentation is off. Every caller only subtracts two
	 * readings, so 0-0 keeps the totals at 0 without changing its shape. */
	if (__atomic_load_n(&g_HmrdpProgSample, __ATOMIC_RELAXED) == 0)
		return 0;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((unsigned long long)ts.tv_sec * 1000000000ull) + (unsigned long long)ts.tv_nsec;
}
'@) 'HmrdpSetProgSample'

# (b) the per-tile sampling counter is untouched while the probe is off.
Patch-Regex $progC `
  '#define HMRDP_PHASE_ARM\(\) \\\n\tg_HmrdpSampleTile = \(\(__atomic_add_fetch\(&g_HmrdpTileSample, 1, __ATOMIC_RELAXED\) & 15u\) == 0\)' (@'
#define HMRDP_PHASE_ARM()                                                         \
	g_HmrdpSampleTile =                                                        \
	    ((__atomic_load_n(&g_HmrdpProgSample, __ATOMIC_RELAXED) != 0) &&        \
	     ((__atomic_add_fetch(&g_HmrdpTileSample, 1, __ATOMIC_RELAXED) & 15u) == 0))
'@) 'g_HmrdpProgSample, __ATOMIC_RELAXED) != 0) &&'
