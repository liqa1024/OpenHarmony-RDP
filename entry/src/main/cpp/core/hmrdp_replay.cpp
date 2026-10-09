#include "hmrdp_replay.h"

#include <native_window/external_window.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "hmrdp_decode_tuning.h"
#include "hmrdp_gfx_capture.h"
#include "hmrdp_gfx_cpu.h"
#include "hmrdp_gfx_driver.h"
#include "hmrdp_log.h"
#include "hmrdp_parallel.h"
#include "hmrdp_presenter.h"

namespace hmrdp {

namespace {

constexpr int kLogEvery = 120;
constexpr int64_t kMaxRunUs = 120ll * 1000000ll;
constexpr int64_t kMaxRealtimeRunUs = 900ll * 1000000ll;
constexpr int kStartWaitUs = 3000000;

std::string CaptureTag(const std::string& capturePath) {
  FILE* f = std::fopen(capturePath.c_str(), "rb");
  if (f == nullptr) {
    return std::string("nofile");
  }
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  uint64_t h = 1469598103934665603ull;
  h = (h ^ static_cast<uint64_t>(size > 0 ? size : 0)) * 1099511628211ull;
  const size_t chunk = 256u * 1024u;
  std::vector<uint8_t> buf(chunk);
  const long last = size > static_cast<long>(chunk) ? size - static_cast<long>(chunk) : 0;
  const long offsets[2] = {0, last};
  for (int i = 0; i < 2; ++i) {
    std::fseek(f, offsets[i], SEEK_SET);
    const size_t got = std::fread(buf.data(), 1, chunk, f);
    for (size_t b = 0; b < got; ++b) {
      h = (h ^ buf[b]) * 1099511628211ull;
    }
  }
  std::fclose(f);
  char out[24];
  std::snprintf(out, sizeof(out), "%08llx", static_cast<unsigned long long>(h & 0xffffffffull));
  return std::string(out);
}

std::string RefSibling(const std::string& capturePath, const std::string& name) {
  const size_t slash = capturePath.find_last_of("/\\");
  const std::string dir =
      slash == std::string::npos ? std::string() : capturePath.substr(0, slash + 1);
  return dir + name;
}

uint64_t HashFrame(const uint8_t* data, int stride, int width, int height) {
  // Four independent FNV-1a lanes: the scalar chain is dependency-bound on the one
  // multiply per word and costs several ms per whole-screen frame at this clock;
  // the lanes hide that latency. Any stable hash would do - it is only ever
  // compared against itself.
  uint64_t h[4] = {1469598103934665603ull, 1099511628211ull, 0x9e3779b97f4a7c15ull,
                   0xc2b2ae3d27d4eb4full};
  h[0] ^= static_cast<uint64_t>(static_cast<uint32_t>(width)) * 0x9e3779b97f4a7c15ull;
  h[2] ^= static_cast<uint64_t>(static_cast<uint32_t>(height)) * 0xc2b2ae3d27d4eb4full;
  const size_t rowBytes = static_cast<size_t>(width) * 4;
  for (int y = 0; y < height; ++y) {
    const uint8_t* row = data + static_cast<size_t>(y) * static_cast<size_t>(stride);
    size_t i = 0;
    for (; i + 32 <= rowBytes; i += 32) {
      for (int k = 0; k < 4; ++k) {
        uint64_t w = 0;
        std::memcpy(&w, row + i + static_cast<size_t>(k) * 8u, 8);
        h[k] = (h[k] ^ w) * 1099511628211ull;
      }
    }
    for (; i + 8 <= rowBytes; i += 8) {
      uint64_t w = 0;
      std::memcpy(&w, row + i, 8);
      h[0] = (h[0] ^ w) * 1099511628211ull;
    }
    for (; i < rowBytes; ++i) {
      h[1] = (h[1] ^ row[i]) * 1099511628211ull;
    }
  }
  return h[0] ^ (h[1] * 0x9e3779b97f4a7c15ull) ^ (h[2] * 0xc2b2ae3d27d4eb4full) ^ h[3];
}

bool WriteBmp(const std::string& path, const uint8_t* data, int stride, int width, int height) {
  if (data == nullptr || width <= 0 || height <= 0) {
    return false;
  }
  const uint32_t rowBytes = static_cast<uint32_t>(width) * 4u;
  const uint32_t imageBytes = rowBytes * static_cast<uint32_t>(height);
  uint8_t header[54] = {0};
  header[0] = 'B';
  header[1] = 'M';
  const uint32_t fileBytes = 54u + imageBytes;
  auto put32 = [&header](size_t off, uint32_t v) { std::memcpy(header + off, &v, 4); };
  put32(2, fileBytes);
  put32(10, 54u);
  put32(14, 40u);
  put32(18, static_cast<uint32_t>(width));
  put32(22, static_cast<uint32_t>(height));
  put32(26, 1u);
  put32(28, 32u);
  put32(34, imageBytes);
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) {
    return false;
  }
  bool ok = std::fwrite(header, 1, sizeof(header), f) == sizeof(header);
  for (int y = height - 1; ok && y >= 0; --y) {
    ok = std::fwrite(data + static_cast<size_t>(y) * static_cast<size_t>(stride), 1, rowBytes, f) ==
         rowBytes;
  }
  std::fclose(f);
  return ok;
}

bool ReadBmp(const std::string& path, std::vector<uint8_t>* out, int* width, int* height) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    return false;
  }
  uint8_t header[54] = {0};
  const bool read = std::fread(header, 1, sizeof(header), f) == sizeof(header);
  uint32_t w = 0;
  uint32_t h = 0;
  uint16_t bpp = 0;
  if (read) {
    std::memcpy(&w, header + 18, 4);
    std::memcpy(&h, header + 22, 4);
    std::memcpy(&bpp, header + 28, 2);
  }
  const uint32_t offset = 54u;
  const bool usable = read && header[0] == 'B' && header[1] == 'M' && bpp == 32u && w > 0 &&
                      w <= 16384u && (h != 0u);
  if (!usable) {
    std::fclose(f);
    return false;
  }
  const bool bottomUp = (h & 0x80000000u) == 0u;
  const uint32_t uh = h & 0x7fffffffu;
  const uint32_t rowBytes = w * 4u;
  out->assign(static_cast<size_t>(rowBytes) * uh, 0);
  std::fseek(f, static_cast<long>(offset), SEEK_SET);
  for (uint32_t row = 0; row < uh; ++row) {
    const uint32_t y = bottomUp ? (uh - 1u - row) : row;
    if (std::fread(out->data() + static_cast<size_t>(y) * rowBytes, 1, rowBytes, f) != rowBytes) {
      std::fclose(f);
      return false;
    }
  }
  std::fclose(f);
  *width = static_cast<int>(w);
  *height = static_cast<int>(uh);
  return true;
}

constexpr char kRefMagic[8] = {'H', 'M', 'R', 'D', 'R', 'E', 'F', '1'};

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int64_t ProcessCpuUs() {
  struct timespec ts = {};
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) {
    return 0;
  }
  return static_cast<int64_t>(ts.tv_sec) * 1000000 + static_cast<int64_t>(ts.tv_nsec) / 1000;
}

}  // namespace

GfxReplay::GfxReplay() = default;

GfxReplay::~GfxReplay() {
  Stop();
}

bool GfxReplay::Start(void* nativeWindow, int surfaceW, int surfaceH,
                      const std::string& gfxPath, bool realtime, GfxReplayRefMode refMode) {
  Stop();
  if (nativeWindow == nullptr || surfaceW <= 0 || surfaceH <= 0 || gfxPath.empty()) {
    if (nativeWindow != nullptr) {
      OH_NativeWindow_DestroyNativeWindow(static_cast<OHNativeWindow*>(nativeWindow));
    }
    return false;
  }
  // An older (version 0) capture has no arrival times, so realtime falls back to
  // the unpaced fast mode (labelled fast(untimed)).
  const bool effectiveRealtime = realtime && GfxCaptureHasTimestamps(gfxPath);
  if (realtime && !effectiveRealtime) {
    HMRDP_LOGW("gfx replay: capture has no arrival times, realtime mode unavailable");
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    {
      std::lock_guard<std::mutex> err(errorMutex_);
      lastError_.clear();
    }
    window_ = nativeWindow;
    surfaceW_ = surfaceW;
    surfaceH_ = surfaceH;
    gfxPath_ = gfxPath;
    frames_.store(0);
    presents_.store(0);
    presentFailures_.store(0);
    imgDiffChecks_.store(0);
    imgDiffBad_.store(0);
    imgDiffRgbPx_.store(0);
    imgDiffAlphaPx_.store(0);
    imgDiffBBoxX0_.store(-1);
    imgDiffBBoxY0_.store(-1);
    imgDiffBBoxX1_.store(-1);
    imgDiffBBoxY1_.store(-1);
    imgDiffMaxDelta_.store(0);
    imgDiffSmallPx_.store(0);
    refHashes_.clear();
    refImage_.clear();
    refImageW_ = 0;
    refImageH_ = 0;
    refImageFrame_ = -1;
    refHashUs_.store(0);
    refChecks_.store(0);
    refBad_.store(0);
    refFirstBad_.store(-1);
    refNote_.clear();
    refMode_.store(static_cast<int>(refMode));
    runId_.fetch_add(1);
    aborted_.store(false);
    refTag_ = CaptureTag(gfxPath);
    if (refMode_.load() == static_cast<int>(GfxReplayRefMode::kCompare)) {
      if (!RefLoad(gfxPath)) {
        HMRDP_LOGW("gfx replay: reference unusable (%{public}s)", refNote_.c_str());
      }
    }
    presentUs_.store(0);
    uploadBytes_.store(0);
    uploadBoxBytes_.store(0);
    uploadRectPresents_.store(0);
    uploadTruncated_.store(0);
    uploadMaxRects_.store(0);
    cpuStartUs_.store(0);
    cpuEndUs_.store(0);
    pumpUs_.store(0);
    pumpStartUs_.store(0);
    paceUs_.store(0);
    realtimeRequested_.store(realtime ? 1 : 0);
    realtimeActive_.store(effectiveRealtime ? 1 : 0);
    realtimeLagUs_.store(0);
    recordBaseUs_ = 0;
    recordWallBaseUs_ = 0;
    firstFrameEndUs_.store(0);
    startUs_.store(NowUs());
    endUs_.store(0);
    pipeline_.presenter()->SetSurface(window_, surfaceW_, surfaceH_);
    running_.store(true);
    thread_ = std::thread(&GfxReplay::Run, this);
  }
  const int64_t deadline = NowUs() + kStartWaitUs;
  while (NowUs() < deadline) {
    if (!running_.load() || presents_.load() > 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (!running_.load()) {
    std::lock_guard<std::mutex> err(errorMutex_);
    HMRDP_LOGE("gfx replay: start failed (%{public}s)",
               lastError_.empty() ? "unknown" : lastError_.c_str());
    Stop();
    return false;
  }
  HMRDP_LOGI(
      "gfx replay: started mode=%{public}s surface=%{public}dx%{public}d path=%{public}s",
      effectiveRealtime ? "realtime" : "fast", surfaceW, surfaceH, gfxPath.c_str());
  HMRDP_LOGI("gfx replay: decode %{public}s", DecodeThreadsInfo().c_str());
  return true;
}

void GfxReplay::Resize(int width, int height) {
  if (width <= 0 || height <= 0) {
    return;
  }
  pendingW_.store(width);
  pendingH_.store(height);
}

void GfxReplay::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_.exchange(false)) {
      if (!thread_.joinable()) {
        if (window_ != nullptr) {
          OH_NativeWindow_DestroyNativeWindow(static_cast<OHNativeWindow*>(window_));
          window_ = nullptr;
        }
        return;
      }
    }
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  std::lock_guard<std::mutex> lock(mutex_);
  pipeline_.presenter()->DestroySurface();
  if (window_ != nullptr) {
    OH_NativeWindow_DestroyNativeWindow(static_cast<OHNativeWindow*>(window_));
    window_ = nullptr;
  }
  HMRDP_LOGI("gfx replay: stopped frames=%{public}llu presents=%{public}llu",
             static_cast<unsigned long long>(frames_.load()),
             static_cast<unsigned long long>(presents_.load()));
}

std::string GfxReplay::StatsLines() {
  const int64_t stopUs = endUs_.load();
  const int64_t nowUs = stopUs != 0 ? stopUs : NowUs();
  const uint64_t presents = presents_.load();
  const int64_t firstFrameEndUs = firstFrameEndUs_.load();
  const int64_t elapsedUs = firstFrameEndUs != 0
                                ? nowUs - firstFrameEndUs
                                : (startUs_.load() != 0 ? nowUs - startUs_.load() : 0);
  const double fps =
      (elapsedUs > 0 && presents > 1)
          ? static_cast<double>(presents - 1) * 1000000.0 / static_cast<double>(elapsedUs)
          : 0.0;
  auto avgMs = [](uint64_t total, uint64_t count) {
    return count > 0 ? static_cast<double>(total) / static_cast<double>(count) / 1000.0 : 0.0;
  };
  std::string err;
  {
    std::lock_guard<std::mutex> lock(errorMutex_);
    err = lastError_;
  }
  const uint64_t pace = paceUs_.load();
  const uint64_t elapsed = elapsedUs > 0 ? static_cast<uint64_t>(elapsedUs) : 0;
  const uint64_t measuredPump = pumpUs_.load();
  uint64_t pumpUs = measuredPump > 0
                        ? (measuredPump > pace ? measuredPump - pace : 0)
                        : (elapsed > pace ? elapsed - pace : 0);
  const int64_t firstFrameEnd = firstFrameEndUs_.load();
  const int64_t pumpStart = pumpStartUs_.load();
  if (firstFrameEnd != 0 && pumpStart != 0 && firstFrameEnd > pumpStart &&
      static_cast<uint64_t>(firstFrameEnd - pumpStart) < pumpUs) {
    pumpUs -= static_cast<uint64_t>(firstFrameEnd - pumpStart);
  }

  const char* modeName = realtimeActive_.load() != 0
                             ? "realtime"
                             : (realtimeRequested_.load() != 0 ? "fast(untimed)" : "fast");
  const unsigned long long frames = static_cast<unsigned long long>(frames_.load());
  const char* stateName = running_.load() ? "running" : (aborted_.load() ? "aborted" : "finished");
  char head[420];
  std::snprintf(head, sizeof(head),
                "state=%s  run=%llu  mode=%s  frames=%llu  presents=%llu  fps=%.1f  "
                "fail=%llu\n"
                "feed=%llums   parse=%llums   present=%.2fms   lag=%llums   (running=%d)",
                stateName, static_cast<unsigned long long>(runId_.load()), modeName,
                frames, static_cast<unsigned long long>(presents), fps,
                static_cast<unsigned long long>(presentFailures_.load()),
                static_cast<unsigned long long>(pumpUs / 1000),
                static_cast<unsigned long long>(hmrdp::GfxReplayParseUs() / 1000),
                avgMs(presentUs_.load(), presents),
                static_cast<unsigned long long>(realtimeLagUs_.load() / 1000),
                running_.load() ? 1 : 0);
  std::string out(head);

  const int64_t cpuStart = cpuStartUs_.load();
  const int64_t cpuEnd = cpuEndUs_.load();
  if (cpuStart != 0 && cpuEnd > cpuStart) {
    const std::string freq = hmrdp::CpuFreqInfo();
    char run[260];
    std::snprintf(run, sizeof(run), "\nrun  threads=%d  cpu=%.2fs  cpuKHz=%s",
                  hmrdp::DecodeThreads(),
                  static_cast<double>(cpuEnd - cpuStart) / 1000000.0,
                  freq.empty() ? "n/a" : freq.c_str());
    out += run;
  }

  const uint64_t uploadBoxBytes = uploadBoxBytes_.load();
  if (uploadBoxBytes > 0) {
    const double mb = 1.0 / (1024.0 * 1024.0);
    char up[288];
    std::snprintf(up, sizeof(up),
                  "\nupload rects  rectlist=%llu/%llu  uploaded=%.1fMB (box=%.1fMB)  "
                  "maxRects=%llu  truncated=%llu",
                  static_cast<unsigned long long>(uploadRectPresents_.load()),
                  static_cast<unsigned long long>(presents),
                  static_cast<double>(uploadBytes_.load()) * mb,
                  static_cast<double>(uploadBoxBytes) * mb,
                  static_cast<unsigned long long>(uploadMaxRects_.load()),
                  static_cast<unsigned long long>(uploadTruncated_.load()));
    out += up;
  }

  const GfxWorkMeter::Sample work = pipeline_.meter()->Peek();
  if (work.frames > 0) {
    const uint64_t frames = work.frames;
    const uint64_t workUs =
        (work.zgxParseUs + work.decodeUs + work.composeUs + work.presentUs) / frames;
    char wl[400];
    std::snprintf(wl, sizeof(wl),
                  "\nperFrame work=%lluus = zgx+parse %lluus + decode %lluus"
                  " + compose %lluus + present %lluus"
                  "  (+ sync %lluus + presentWait %lluus blocked)\n"
                  "         frames=%llu cmds/frame=%llu kB/frame=%llu",
                  static_cast<unsigned long long>(workUs),
                  static_cast<unsigned long long>(work.zgxParseUs / frames),
                  static_cast<unsigned long long>(work.decodeUs / frames),
                  static_cast<unsigned long long>(work.composeUs / frames),
                  static_cast<unsigned long long>(work.presentUs / frames),
                  static_cast<unsigned long long>(work.syncUs / frames),
                  static_cast<unsigned long long>(work.presentWaitUs / frames),
                  static_cast<unsigned long long>(frames),
                  static_cast<unsigned long long>(work.commands / frames),
                  static_cast<unsigned long long>(work.bytes / frames / 1024));
    out += wl;

    const uint64_t* su = work.setupUs;
    const uint64_t* sc = work.setupCount;
    if (work.SetupUs() > 0) {
      char st[400];
      auto ms = [&frames](uint64_t us) {
        return static_cast<double>(us) / static_cast<double>(frames) / 1000.0;
      };
      std::snprintf(
          st, sizeof(st),
          "\nsetup ms/frame: reset=%.2f(%llu) create=%.2f(%llu) delete=%.2f(%llu) map=%.2f(%llu)"
          " fill=%.2f(%llu) blit=%.2f(%llu) cache=%.2f(%llu) imp=%.2f(%llu)  total=%.2fms/frame",
          ms(su[static_cast<int>(GfxSetupKind::kReset)]),
          static_cast<unsigned long long>(sc[static_cast<int>(GfxSetupKind::kReset)]),
          ms(su[static_cast<int>(GfxSetupKind::kCreate)]),
          static_cast<unsigned long long>(sc[static_cast<int>(GfxSetupKind::kCreate)]),
          ms(su[static_cast<int>(GfxSetupKind::kDelete)]),
          static_cast<unsigned long long>(sc[static_cast<int>(GfxSetupKind::kDelete)]),
          ms(su[static_cast<int>(GfxSetupKind::kMap)]),
          static_cast<unsigned long long>(sc[static_cast<int>(GfxSetupKind::kMap)]),
          ms(su[static_cast<int>(GfxSetupKind::kFill)]),
          static_cast<unsigned long long>(sc[static_cast<int>(GfxSetupKind::kFill)]),
          ms(su[static_cast<int>(GfxSetupKind::kBlit)]),
          static_cast<unsigned long long>(sc[static_cast<int>(GfxSetupKind::kBlit)]),
          ms(su[static_cast<int>(GfxSetupKind::kCache)]),
          static_cast<unsigned long long>(sc[static_cast<int>(GfxSetupKind::kCache)]),
          ms(su[static_cast<int>(GfxSetupKind::kImport)]),
          static_cast<unsigned long long>(sc[static_cast<int>(GfxSetupKind::kImport)]),
          ms(work.SetupUs()));
      out += st;
    }

  }

  const int refMode = refMode_.load();
  if (refMode != 0) {
    char rf[320];
    if (refMode == static_cast<int>(GfxReplayRefMode::kExport)) {
      std::snprintf(rf, sizeof(rf),
                    "\nref export: frames=%llu -> hmrdp_ref_%s.hash + .bmp   hashMs=%.1f",
                    static_cast<unsigned long long>(refFrames_.load()), refTag_.c_str(),
                    static_cast<double>(refHashUs_.load()) / 1000.0);
    } else {
      std::snprintf(rf, sizeof(rf),
                    "\nref compare: refFrames=%llu checks=%llu bad=%llu firstBad=%lld "
                    "imageFrame=%lld rgbPx=%llu alphaPx=%llu maxDelta=%d bbox=(%d,%d)-(%d,%d) hashMs=%.1f%s%s",
                    static_cast<unsigned long long>(refFrames_.load()),
                    static_cast<unsigned long long>(refChecks_.load()),
                    static_cast<unsigned long long>(refBad_.load()),
                    static_cast<long long>(refFirstBad_.load()),
                    static_cast<long long>(refImageFrame_),
                    static_cast<unsigned long long>(imgDiffRgbPx_.load()),
                    static_cast<unsigned long long>(imgDiffAlphaPx_.load()),
                    imgDiffMaxDelta_.load(), imgDiffBBoxX0_.load(), imgDiffBBoxY0_.load(),
                    imgDiffBBoxX1_.load(), imgDiffBBoxY1_.load(),
                    static_cast<double>(refHashUs_.load()) / 1000.0,
                    refNote_.empty() ? "" : "  note=", refNote_.c_str());
    }
    out += rf;
  }
  if (!err.empty()) {
    out += "\nerr=";
    out += err;
  }
  return out;
}

std::string GfxReplay::Stats() {
  std::string s = StatsLines();
  for (char& c : s) {
    if (c == '\n') {
      c = ' ';
    }
  }
  return s;
}

void GfxReplay::RecordPresent(uint64_t micros) {
  presentUs_.fetch_add(micros);
}

void GfxReplay::MarkFrameEnd(bool presented) {
  if (!presented) {
    return;
  }
  if (firstFrameEndUs_.load() == 0) {
    firstFrameEndUs_.store(static_cast<int64_t>(NowUs()));
  }
}

void GfxReplay::PaceRecord(uint64_t timestampUs) {
  const int64_t now = static_cast<int64_t>(NowUs());
  if (recordBaseUs_ == 0) {
    recordBaseUs_ = timestampUs;
    recordWallBaseUs_ = now;
    return;
  }
  const int64_t target =
      recordWallBaseUs_ + static_cast<int64_t>(timestampUs - recordBaseUs_);
  if (target > now) {
    // Sleep in slices so a long gap in the recording cannot make Stop() wait for
    // the whole gap before the pump notices it must stop.
    constexpr int64_t kSliceUs = 20000;
    const int64_t start = static_cast<int64_t>(NowUs());
    while (running_.load() && static_cast<int64_t>(NowUs()) < target) {
      const int64_t remaining = target - static_cast<int64_t>(NowUs());
      std::this_thread::sleep_for(
          std::chrono::microseconds(remaining > kSliceUs ? kSliceUs : remaining));
    }
    paceUs_.fetch_add(static_cast<uint64_t>(static_cast<int64_t>(NowUs()) - start));
    return;
  }
  const uint64_t behind = static_cast<uint64_t>(now - target);
  if (behind > realtimeLagUs_.load()) {
    realtimeLagUs_.store(behind);
  }
}

void GfxReplay::Run() {
  RunCpuReplay(gfxPath_);
  endUs_.store(NowUs());
  running_.store(false);
}

void GfxReplay::RunCpuReplay(const std::string& gfxPath) {
  pipeline_.presenter()->Prepare();
  GfxCpuDesktop cpu;
  cpu.SetPresenter(pipeline_.presenter());
  cpu.SetMeter(pipeline_.meter());
  std::string error;
  if (!cpu.Init(surfaceW_, surfaceH_, &error)) {
    std::lock_guard<std::mutex> err(errorMutex_);
    lastError_ = error;
    return;
  }
  cpu.SetPrePresentFn([this](rdpGdi*) { return OnCpuFramePre(); });
  cpu.SetPresentedFn(
      [this, &cpu](bool presented, uint64_t presentUs, const PresentUploadInfo& upload) {
        OnCpuFramePresent(&cpu, presented, presentUs, upload);
      });

  pipeline_.meter()->Reset();
  GfxWorkInstall(cpu.gfx(), pipeline_.meter());
  GfxWorkSetFrameBeginHook(cpu.gfx(), [&cpu]() { cpu.OnFrameBegin(); });

  hmrdp::GfxDumpSetReplaying(true);
  hmrdp::GfxReplayResetParseUs();

  const int64_t pumpStart = NowUs();
  pumpStartUs_.store(pumpStart);
  cpuStartUs_.store(ProcessCpuUs());

  ReplayPaceFn pace;
  if (realtimeActive_.load() != 0) {
    pace = [this](uint64_t tsUs) { PaceRecord(tsUs); };
  }
  const ReplayPaceAccumFn paceAccum = [this]() { return paceUs_.load(); };
  bool aborted = false;
  bool ok = false;
  {
    DecodeExecutorScope decodeScope(pipeline_.executor());
    ok = GfxReplayPump(gfxPath, cpu.gfx(), &running_, &error, pace, paceAccum, &aborted);
  }
  aborted_.store(aborted);
  cpuEndUs_.store(ProcessCpuUs());
  pumpUs_.store(static_cast<uint64_t>(NowUs() - pumpStart));
  HMRDP_LOGI("gfx replay: pump %{public}llu ms (paced %{public}llu ms)",
             static_cast<unsigned long long>(pumpUs_.load() / 1000),
             static_cast<unsigned long long>(paceUs_.load() / 1000));

  if (refMode_.load() == static_cast<int>(GfxReplayRefMode::kExport) && !aborted_.load()) {
    RefWrite(gfxPath, &cpu);
  }

  hmrdp::GfxDumpSetReplaying(false);
  cpu.SetPrePresentFn(nullptr);
  cpu.SetPresentedFn(nullptr);
  GfxWorkSetFrameBeginHook(cpu.gfx(), nullptr);
  GfxWorkUninstall(cpu.gfx());
  if (!ok) {
    std::lock_guard<std::mutex> err(errorMutex_);
    lastError_ = error;
  }
  HMRDP_LOGI("gfx replay: finished (cpu): %{public}s", Stats().c_str());
}

bool GfxReplay::RefLoad(const std::string& capturePath) {
  const std::string hashPath = RefSibling(capturePath, "hmrdp_ref_" + refTag_ + ".hash");
  FILE* f = std::fopen(hashPath.c_str(), "rb");
  if (f == nullptr) {
    refNote_ = "no " + hashPath;
    return false;
  }
  char magic[8] = {0};
  uint32_t geom[4] = {0, 0, 0, 0};
  bool ok = std::fread(magic, 1, sizeof(magic), f) == sizeof(magic) &&
            std::memcmp(magic, kRefMagic, sizeof(kRefMagic)) == 0 &&
            std::fread(geom, 1, sizeof(geom), f) == sizeof(geom) && geom[3] > 0u &&
            geom[3] < 1000000u;
  if (ok) {
    refHashes_.resize(geom[3]);
    ok = std::fread(refHashes_.data(), 8, geom[3], f) == geom[3];
  }
  std::fclose(f);
  if (!ok) {
    refHashes_.clear();
    refNote_ = "corrupt " + hashPath;
    return false;
  }
  refFrames_.store(refHashes_.size());
  refImageFrame_ = static_cast<int64_t>(geom[2]);
  if (!ReadBmp(RefSibling(capturePath, "hmrdp_ref_" + refTag_ + ".bmp"), &refImage_, &refImageW_, &refImageH_)) {
    refNote_ = "no reference image (hash-only check)";
  }
  HMRDP_LOGI("gfx replay: reference loaded (%{public}zu frames, image %{public}dx%{public}d "
             "@%{public}lld)",
             refHashes_.size(), refImageW_, refImageH_,
             static_cast<long long>(refImageFrame_));
  return true;
}

void GfxReplay::RefWrite(const std::string& capturePath, GfxCpuDesktop* cpu) {
  rdpGdi* gdi = cpu != nullptr ? cpu->gdi() : nullptr;
  if (gdi == nullptr || gdi->primary_buffer == nullptr || refHashes_.empty()) {
    return;
  }
  const std::string hashPath = RefSibling(capturePath, "hmrdp_ref_" + refTag_ + ".hash");
  FILE* f = std::fopen(hashPath.c_str(), "wb");
  if (f == nullptr) {
    HMRDP_LOGW("gfx replay: cannot write %{public}s", hashPath.c_str());
    return;
  }
  const uint32_t geom[4] = {static_cast<uint32_t>(gdi->width),
                            static_cast<uint32_t>(gdi->height),
                            static_cast<uint32_t>(refHashes_.size() - 1u),
                            static_cast<uint32_t>(refHashes_.size())};
  std::fwrite(kRefMagic, 1, sizeof(kRefMagic), f);
  std::fwrite(geom, 1, sizeof(geom), f);
  std::fwrite(refHashes_.data(), 8, refHashes_.size(), f);
  std::fclose(f);
  const bool bmp = WriteBmp(RefSibling(capturePath, "hmrdp_ref_" + refTag_ + ".bmp"), gdi->primary_buffer,
                            static_cast<int>(gdi->stride), static_cast<int>(gdi->width),
                            static_cast<int>(gdi->height));
  HMRDP_LOGI("gfx replay: reference written (%{public}zu frames, image %{public}s)",
             refHashes_.size(), bmp ? "ok" : "FAILED");
}

void GfxReplay::RefCompareImage(const uint8_t* data, int stride, int width, int height) {
  const size_t rowBytes = static_cast<size_t>(width) * 4u;
  if (refImageW_ != width || refImageH_ != height ||
      refImage_.size() != rowBytes * static_cast<size_t>(height)) {
    refNote_ = "stored image geometry differs";
    return;
  }
  uint64_t rgbPx = 0;
  uint64_t alphaPx = 0;
  uint64_t smallPx = 0;
  int maxDelta = 0;
  int x0 = -1;
  int y0 = -1;
  int x1 = -1;
  int y1 = -1;
  for (int y = 0; y < height; ++y) {
    const uint8_t* cur = data + static_cast<size_t>(y) * static_cast<size_t>(stride);
    const uint8_t* ref = refImage_.data() + static_cast<size_t>(y) * rowBytes;
    for (int x = 0; x < width; ++x) {
      const uint8_t* a = cur + static_cast<size_t>(x) * 4u;
      const uint8_t* b = ref + static_cast<size_t>(x) * 4u;
      const int dR = std::abs(static_cast<int>(a[2]) - static_cast<int>(b[2]));
      const int dG = std::abs(static_cast<int>(a[1]) - static_cast<int>(b[1]));
      const int dB = std::abs(static_cast<int>(a[0]) - static_cast<int>(b[0]));
      const int d = std::max(dR, std::max(dG, dB));
      if (d > 0) {
        ++rgbPx;
        if (d > maxDelta) {
          maxDelta = d;
        }
        if (d <= 2) {
          ++smallPx;
        }
        if (x0 < 0 || x < x0) {
          x0 = x;
        }
        if (x1 < x) {
          x1 = x;
        }
        if (y0 < 0 || y < y0) {
          y0 = y;
        }
        y1 = y;
      }
      if (a[3] != b[3]) {
        ++alphaPx;
      }
    }
  }
  imgDiffChecks_.fetch_add(1);
  if (rgbPx > 0) {
    imgDiffBad_.fetch_add(1);
  }
  imgDiffRgbPx_.store(rgbPx);
  imgDiffAlphaPx_.store(alphaPx);
  imgDiffSmallPx_.store(smallPx);
  imgDiffMaxDelta_.store(maxDelta);
  imgDiffBBoxX0_.store(x0);
  imgDiffBBoxY0_.store(y0);
  imgDiffBBoxX1_.store(x1);
  imgDiffBBoxY1_.store(y1);
  HMRDP_LOGI("gfx replay: reference image compare: rgbPx=%{public}llu maxDelta=%{public}d",
             static_cast<unsigned long long>(rgbPx), maxDelta);
}

void GfxReplay::RefCollectFrame(GfxCpuDesktop* cpu) {
  const int mode = refMode_.load();
  if (mode == 0 || cpu == nullptr) {
    return;
  }
  rdpGdi* gdi = cpu->gdi();
  if (gdi == nullptr || gdi->primary_buffer == nullptr || gdi->width == 0 || gdi->height == 0) {
    return;
  }
  const int width = static_cast<int>(gdi->width);
  const int height = static_cast<int>(gdi->height);
  const int stride = static_cast<int>(gdi->stride);
  // Inside gdi's EndFrame, so its time lands in `compose` - reported separately so
  // a reference run is never read as a performance run.
  const int64_t hashStart = NowUs();
  const uint64_t hash = HashFrame(gdi->primary_buffer, stride, width, height);
  const int64_t index = static_cast<int64_t>(frames_.load()) - 1;
  refHashUs_.fetch_add(static_cast<uint64_t>(NowUs() - hashStart));
  if (mode == static_cast<int>(GfxReplayRefMode::kExport)) {
    refHashes_.push_back(hash);
    refFrames_.store(refHashes_.size());
    return;
  }
  refChecks_.fetch_add(1);
  if (index >= 0 && static_cast<size_t>(index) < refHashes_.size()) {
    if (refHashes_[static_cast<size_t>(index)] != hash) {
      if (refBad_.fetch_add(1) == 0) {
        refFirstBad_.store(index);
        WriteBmp(RefSibling(gfxPath_, "hmrdp_ref_" + refTag_ + "_fail.bmp"), gdi->primary_buffer, stride, width,
                 height);
      }
    }
  } else {
    refBad_.fetch_add(1);
  }
  if (index == refImageFrame_) {
    RefCompareImage(gdi->primary_buffer, stride, width, height);
  }
}

bool GfxReplay::OnCpuFramePre() {
  const int64_t nowUs = static_cast<int64_t>(NowUs());
  const int64_t capUs = realtimeActive_.load() != 0 ? kMaxRealtimeRunUs : kMaxRunUs;
  if (nowUs - startUs_.load() > capUs) {
    HMRDP_LOGI("gfx replay: %{public}ds cap reached",
               static_cast<int>(capUs / 1000000));
    running_.store(false);
    return false;
  }
  const int pw = pendingW_.exchange(0);
  const int ph = pendingH_.exchange(0);
  if (pw > 0 && ph > 0) {
    pipeline_.presenter()->ResizeSurface(pw, ph);
  }
  return true;
}

void GfxReplay::OnCpuFramePresent(GfxCpuDesktop* cpu, bool presented, uint64_t presentUs,
                                  const PresentUploadInfo& upload) {
  RecordPresent(presentUs);
  uploadBytes_.fetch_add(static_cast<uint64_t>(upload.uploadedBytes));
  uploadBoxBytes_.fetch_add(static_cast<uint64_t>(upload.boxBytes));
  if (upload.usedRects) {
    uploadRectPresents_.fetch_add(1);
  }
  if (upload.rectListTruncated) {
    uploadTruncated_.fetch_add(1);
  }
  const uint64_t frameRects = static_cast<uint64_t>(upload.totalRects);
  if (frameRects > uploadMaxRects_.load()) {
    uploadMaxRects_.store(frameRects);
  }
  frames_.fetch_add(1);
  if (presented) {
    presents_.fetch_add(1);
  } else {
    presentFailures_.fetch_add(1);
  }
  if ((frames_.load() % static_cast<uint64_t>(kLogEvery)) == 0) {
    HMRDP_LOGI("gfx replay: %{public}s", Stats().c_str());
  }
  const int64_t paceStart = NowUs();
  MarkFrameEnd(presented);
  pipeline_.meter()->OnPace(static_cast<uint64_t>(NowUs() - paceStart));
  RefCollectFrame(cpu);
}

}  // namespace hmrdp
