/*
 * HmRdp - GPU RemoteFX/Progressive decoder + GPU surface engine
 * (see hmrdp_rfx.h).
 *
 * §1 covers the whole Progressive tile pipeline in GLES 3.1 compute: kFirst
 * (RLGR1 -> dequant -> extrapolate inverse DWT) and kUpgrade (SRL/raw
 * refinement), with the per-(tile,component) current/sign/bit-positions state
 * kept resident in GPU buffers across messages, plus YCbCr->BGRA composition.
 *
 * §2 (GfxGpuDesktop) is the full GFX surface model: a multi-surface registry
 * (CreateSurface/DeleteSurface with FreeRDP's 16-byte alignment and 0xFF fill),
 * per-surface progressive state, a global bitmap cache, solid fill / surface
 * copy / cache blits, uncompressed uploads and the ClearCodec read-modify-write.
 * The offline self-test replays the full captured command stream through it and
 * compares every surface pixel by pixel against the FreeRDP baselines.
 */
#include "hmrdp_rfx.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>

#include "hmrdp_egl.h"
#include "hmrdp_log.h"

#include <freerdp/codec/clear.h>
#include "hmrdp_renderer.h"

namespace hmrdp {
namespace {

#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x0040
#endif
#ifndef EGL_CONTEXT_MINOR_VERSION
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#endif

constexpr uint32_t kMetaStride = 64;   // bytes per (tile,component) stream job
constexpr uint32_t kChunkTiles = 512;  // tiles decoded per dispatch

// YCbCr->BGRX fixed-point factors, matching FreeRDP prim_colors.c
// (general_yCbCrToRGB_16s8u_P3AC4R_BGRX, divisor 16).
constexpr int kKr = static_cast<int>(1.402525f * (1 << 16));
constexpr int kKcrG = static_cast<int>(0.714401f * (1 << 16));
constexpr int kKcbG = static_cast<int>(0.343730f * (1 << 16));
constexpr int kKcbB = static_cast<int>(1.769905f * (1 << 16));

// The compute shader mirrors hmrdp_rfx.cpp exactly. Extrapolate regions only
// (every captured Progressive region uses RFX_DWT_REDUCE_EXTRAPOLATE).
const char* kDecodeShader = R"GLSL(#version 310 es
precision highp int;
precision highp float;

layout(local_size_x = 64) in;

layout(std430, binding = 0) readonly buffer Payload { uint data[]; } pay;
layout(std430, binding = 1) readonly buffer Meta { uint data[]; } meta;
layout(std430, binding = 2) buffer Coef { uint data[]; } coef;
// Persistent per-(tile,component) progressive state (FreeRDP
// RFX_PROGRESSIVE_TILE current/sign/bitPos).
layout(std430, binding = 3) buffer StateCur { uint data[]; } cur;
layout(std430, binding = 4) buffer StateSign { uint data[]; } sgn;
layout(std430, binding = 5) buffer StateBP { uint data[]; } bp;

uniform uint uNumStreams;
uniform uint uCompBase;
uniform uint uTempBase;

uint payByte(uint off) { uint w = pay.data[off >> 2]; return (w >> ((off & 3u) << 3)) & 0xFFu; }
uint metaByte(uint off) { uint w = meta.data[off >> 2]; return (w >> ((off & 3u) << 3)) & 0xFFu; }
uint metaU32(uint off) { return meta.data[off >> 2]; }

int i16(int v) { return (v << 16) >> 16; }
int i16lo(uint w) { return int(w << 16) >> 16; }
int i16hi(uint w) { return int(w) >> 16; }
int coefGet(uint idx) { uint w = coef.data[idx >> 1]; return ((idx & 1u) == 0u) ? i16lo(w) : i16hi(w); }
void coefSet(uint idx, int v) {
  uint wi = idx >> 1;
  uint w = coef.data[wi];
  uint u = uint(v) & 0xFFFFu;
  if ((idx & 1u) == 0u) w = (w & 0xFFFF0000u) | u;
  else w = (w & 0x0000FFFFu) | (u << 16);
  coef.data[wi] = w;
}
int curGet(uint idx) { uint w = cur.data[idx >> 1]; return ((idx & 1u) == 0u) ? i16lo(w) : i16hi(w); }
void curSet(uint idx, int v) {
  uint wi = idx >> 1;
  uint w = cur.data[wi];
  uint u = uint(v) & 0xFFFFu;
  if ((idx & 1u) == 0u) w = (w & 0xFFFF0000u) | u;
  else w = (w & 0x0000FFFFu) | (u << 16);
  cur.data[wi] = w;
}
int signGet(uint idx) { uint w = sgn.data[idx >> 1]; return ((idx & 1u) == 0u) ? i16lo(w) : i16hi(w); }
void signSet(uint idx, int v) {
  uint wi = idx >> 1;
  uint w = sgn.data[wi];
  uint u = uint(v) & 0xFFFFu;
  if ((idx & 1u) == 0u) w = (w & 0xFFFF0000u) | u;
  else w = (w & 0x0000FFFFu) | (u << 16);
  sgn.data[wi] = w;
}
uint bpByte(uint off) { uint w = bp.data[off >> 2]; return (w >> ((off & 3u) << 3)) & 0xFFu; }
void bpSetByte(uint off, uint v) {
  uint wi = off >> 2;
  uint sh = (off & 3u) << 3;
  uint w = bp.data[wi];
  bp.data[wi] = (w & ~(0xFFu << sh)) | ((v & 0xFFu) << sh);
}
uint bitPosGet(uint stream, uint band) { return bpByte(stream * 10u + band); }
void bitPosSet(uint stream, uint band, uint v) { bpSetByte(stream * 10u + band, v); }
int shl16(int v, uint shift) { return i16(int(uint(v) << shift)); }

// ---- 64-bit MSB bit reader (mirrors MsbBitReader in hmrdp_rfx.cpp) --------
struct BitReader { uint base; uint len; uint hi; uint lo; int bits; uint pos; };
BitReader gBr;
void brFetch() {
  while (gBr.bits <= 32 && gBr.pos < gBr.len) {
    uint b = payByte(gBr.base + gBr.pos);
    gBr.pos++;
    int sh = 56 - gBr.bits;  // sh is always in [24, 56] here
    if (sh >= 32) {
      gBr.hi |= b << uint(sh - 32);
    } else {
      // The byte spans the 32-bit boundary: low bits go to lo, high bits to hi.
      gBr.lo |= b << uint(sh);
      gBr.hi |= b >> uint(32 - sh);
    }
    gBr.bits += 8;
  }
}
void brInit(uint base, uint len) {
  gBr.base = base; gBr.len = len; gBr.hi = 0u; gBr.lo = 0u; gBr.bits = 0; gBr.pos = 0u;
  brFetch();
}
uint brPeek() { return gBr.hi; }
uint brRemaining() { int r = gBr.bits + int((gBr.len - gBr.pos) * 8u); return r > 0 ? uint(r) : 0u; }
void brShift(int n) {
  if (n <= 0) return;
  if (n >= 64) { gBr.hi = 0u; gBr.lo = 0u; gBr.bits = 0; }
  else {
    if (n >= 32) { gBr.hi = gBr.lo << uint(n - 32); gBr.lo = 0u; }
    else { gBr.hi = (gBr.hi << uint(n)) | (gBr.lo >> uint(32 - n)); gBr.lo = gBr.lo << uint(n); }
    gBr.bits -= n; if (gBr.bits < 0) gBr.bits = 0;
  }
  brFetch();
}
int lzcnt32(uint x) { return (x == 0u) ? 32 : (31 - findMSB(x)); }
uint brReadBits(int n) {
  if (n <= 0) return 0u;
  if (n >= 32) { uint v = brPeek(); brShift(n); return v; }
  uint v = brPeek() >> uint(32 - n);
  brShift(n);
  return v;
}

// Second reader (SRL stream) so an UPGRADE can consume SRL and RAW at once.
BitReader gSrl;
void srlFetch() {
  while (gSrl.bits <= 32 && gSrl.pos < gSrl.len) {
    uint b = payByte(gSrl.base + gSrl.pos);
    gSrl.pos++;
    int sh = 56 - gSrl.bits;
    if (sh >= 32) gSrl.hi |= b << uint(sh - 32);
    else { gSrl.lo |= b << uint(sh); gSrl.hi |= b >> uint(32 - sh); }
    gSrl.bits += 8;
  }
}
void srlInit(uint base, uint len) {
  gSrl.base = base; gSrl.len = len; gSrl.hi = 0u; gSrl.lo = 0u; gSrl.bits = 0; gSrl.pos = 0u;
  srlFetch();
}
uint srlPeek() { return gSrl.hi; }
void srlShift(int n) {
  if (n <= 0) return;
  if (n >= 64) { gSrl.hi = 0u; gSrl.lo = 0u; gSrl.bits = 0; }
  else {
    if (n >= 32) { gSrl.hi = gSrl.lo << uint(n - 32); gSrl.lo = 0u; }
    else { gSrl.hi = (gSrl.hi << uint(n)) | (gSrl.lo >> uint(32 - n)); gSrl.lo = gSrl.lo << uint(n); }
    gSrl.bits -= n; if (gSrl.bits < 0) gSrl.bits = 0;
  }
  srlFetch();
}

// ---- RLGR1 (port of FreeRDP rfx_rlgr_decode, RLGR1 mode) ------------------
void rlgrDecode(uint payBase, uint payLen, uint outBase) {
  brInit(payBase, payLen);
  int k = 1; int kp = k << 3;
  int kr = 1; int krp = kr << 3;
  uint widx = 0u;
  while (brRemaining() > 0u && widx < 4096u) {
    if (k != 0) {
      uint run = 0u;
      int cnt = lzcnt32(brPeek());
      uint nbits = brRemaining();
      if (uint(cnt) > nbits) cnt = int(nbits);
      int vk = cnt;
      while (cnt == 32 && brRemaining() > 0u) {
        brShift(32);
        cnt = lzcnt32(brPeek());
        nbits = brRemaining();
        if (uint(cnt) > nbits) cnt = int(nbits);
        vk += cnt;
      }
      brShift(vk % 32);
      if (brRemaining() < 1u) break;
      brShift(1);
      while (vk > 0) {
        run += 1u << uint(k);
        kp += 4; if (kp > 80) kp = 80;
        k = kp >> 3;
        vk--;
      }
      if (brRemaining() < uint(k)) break;
      uint maskK = (k > 0) ? ((1u << uint(k)) - 1u) : 0u;
      run += (k > 0) ? ((brPeek() >> uint(32 - k)) & maskK) : 0u;
      brShift(k);
      if (brRemaining() < 1u) break;
      uint sign = ((brPeek() & 0x80000000u) != 0u) ? 1u : 0u;
      brShift(1);

      cnt = lzcnt32(~brPeek());
      nbits = brRemaining();
      if (uint(cnt) > nbits) cnt = int(nbits);
      vk = cnt;
      while (cnt == 32 && brRemaining() > 0u) {
        brShift(32);
        cnt = lzcnt32(~brPeek());
        nbits = brRemaining();
        if (uint(cnt) > nbits) cnt = int(nbits);
        vk += cnt;
      }
      brShift(vk % 32);
      if (brRemaining() < 1u) break;
      brShift(1);
      if (brRemaining() < uint(kr)) break;
      uint maskR = (kr > 0) ? ((1u << uint(kr)) - 1u) : 0u;
      uint code = (kr > 0) ? ((brPeek() >> uint(32 - kr)) & maskR) : 0u;
      brShift(kr);
      code = code | (uint(vk) << uint(kr));
      if (vk == 0) { krp -= 2; if (krp < 0) krp = 0; kr = krp >> 3; }
      else if (vk != 1) { krp += vk; if (krp > 80) krp = 80; kr = krp >> 3; }
      kp -= 6; if (kp < 0) kp = 0; k = kp >> 3;
      int mag = (sign != 0u) ? -(int(code) + 1) : (int(code) + 1);
      uint sz = run;
      if (widx + sz > 4096u) sz = 4096u - widx;
      for (uint i = 0u; i < sz; i++) coefSet(outBase + widx++, 0);
      if (widx < 4096u) coefSet(outBase + widx++, mag);
    } else {
      int cnt = lzcnt32(~brPeek());
      uint nbits = brRemaining();
      if (uint(cnt) > nbits) cnt = int(nbits);
      int vk = cnt;
      while (cnt == 32 && brRemaining() > 0u) {
        brShift(32);
        cnt = lzcnt32(~brPeek());
        nbits = brRemaining();
        if (uint(cnt) > nbits) cnt = int(nbits);
        vk += cnt;
      }
      brShift(vk % 32);
      if (brRemaining() < 1u) break;
      brShift(1);
      if (brRemaining() < uint(kr)) break;
      uint maskR = (kr > 0) ? ((1u << uint(kr)) - 1u) : 0u;
      uint code = (kr > 0) ? ((brPeek() >> uint(32 - kr)) & maskR) : 0u;
      brShift(kr);
      code = code | (uint(vk) << uint(kr));
      if (vk == 0) { krp -= 2; if (krp < 0) krp = 0; kr = krp >> 3; }
      else if (vk != 1) { krp += vk; if (krp > 80) krp = 80; kr = krp >> 3; }
      int mag = 0;
      if (code == 0u) {
        kp += 3; if (kp > 80) kp = 80; k = kp >> 3; mag = 0;
      } else {
        kp -= 3; if (kp < 0) kp = 0; k = kp >> 3;
        mag = ((code & 1u) != 0u) ? -(int((code + 1u) >> 1)) : int(code >> 1);
      }
      if (widx < 4096u) coefSet(outBase + widx++, mag);
    }
  }
  while (widx < 4096u) coefSet(outBase + widx++, 0);
}

void dequantSub(uint base, uint off, uint len, int shift) {
  if (shift == 0) return;
  for (uint i = 0u; i < len; i++) coefSet(base + off + i, i16(coefGet(base + off + i) << uint(shift)));
}
void diffDecode(uint base, uint off, uint size) {
  for (uint i = 0u; i + 1u < size; i++)
    coefSet(base + off + i + 1u, i16(coefGet(base + off + i + 1u) + coefGet(base + off + i)));
}

// ---- extrapolate inverse DWT (ports ProgDwtBlock/ProgIdwtX/Y) ------------
void progIdwtX(uint lowBase, uint lowStep, uint highBase, uint highStep, uint dstBase, uint dstStep,
               uint nLowCount, uint nHighCount, uint nDstCount) {
  for (uint i = 0u; i < nDstCount; i++) {
    uint pl = lowBase + i * lowStep;
    uint ph = highBase + i * highStep;
    uint px = dstBase + i * dstStep;
    int H0 = coefGet(ph);
    int L0 = coefGet(pl);
    int X0 = i16(L0 - H0);
    int X2 = X0;
    uint j = 0u;
    for (; j + 1u < nHighCount; j++) {
      int H1 = coefGet(ph + j + 1u);
      L0 = coefGet(pl + j + 1u);
      X2 = i16(L0 - ((H0 + H1) / 2));
      int X1 = i16(((X0 + X2) / 2) + (2 * H0));
      coefSet(px + 2u * j, X0);
      coefSet(px + 2u * j + 1u, X1);
      X0 = X2;
      H0 = H1;
    }
    uint bx = px + 2u * j;
    if (nLowCount <= nHighCount + 1u) {
      if (nLowCount <= nHighCount) {
        coefSet(bx, X2);
        coefSet(bx + 1u, i16(X2 + 2 * H0));
      } else {
        L0 = coefGet(pl + nHighCount);
        X0 = i16(L0 - H0);
        coefSet(bx, X2);
        coefSet(bx + 1u, i16(((X0 + X2) / 2) + 2 * H0));
        coefSet(bx + 2u, X0);
      }
    } else {
      L0 = coefGet(pl + nHighCount);
      X0 = i16(L0 - (H0 / 2));
      coefSet(bx, X2);
      coefSet(bx + 1u, i16(((X0 + X2) / 2) + 2 * H0));
      coefSet(bx + 2u, X0);
      L0 = coefGet(pl + nHighCount + 1u);
      coefSet(bx + 3u, i16((X0 + L0) / 2));
    }
  }
}

void progIdwtY(uint lowBase, uint lowStep, uint highBase, uint highStep, uint dstBase, uint dstStep,
               uint nLowCount, uint nHighCount, uint nDstCount) {
  for (uint i = 0u; i < nDstCount; i++) {
    uint pl = lowBase + i;
    uint ph = highBase + i;
    uint px = dstBase + i;
    int H0 = coefGet(ph); ph += highStep;
    int L0 = coefGet(pl); pl += lowStep;
    int X0 = i16(L0 - H0);
    int X2 = X0;
    for (uint j = 0u; j + 1u < nHighCount; j++) {
      int H1 = coefGet(ph); ph += highStep;
      L0 = coefGet(pl); pl += lowStep;
      X2 = i16(L0 - ((H0 + H1) / 2));
      int X1 = i16(((X0 + X2) / 2) + (2 * H0));
      coefSet(px, X0); px += dstStep;
      coefSet(px, X1); px += dstStep;
      X0 = X2;
      H0 = H1;
    }
    if (nLowCount <= nHighCount + 1u) {
      if (nLowCount <= nHighCount) {
        coefSet(px, X2); px += dstStep;
        coefSet(px, i16(X2 + 2 * H0));
      } else {
        L0 = coefGet(pl);
        X0 = i16(L0 - H0);
        coefSet(px, X2); px += dstStep;
        coefSet(px, i16(((X0 + X2) / 2) + 2 * H0)); px += dstStep;
        coefSet(px, X0);
      }
    } else {
      L0 = coefGet(pl); pl += lowStep;
      X0 = i16(L0 - (H0 / 2));
      coefSet(px, X2); px += dstStep;
      coefSet(px, i16(((X0 + X2) / 2) + 2 * H0)); px += dstStep;
      coefSet(px, X0); px += dstStep;
      L0 = coefGet(pl);
      coefSet(px, i16((X0 + L0) / 2));
    }
  }
}

void progDwtBlock(uint base, uint temp, uint level) {
  uint nBandL = (64u >> level) + 1u;
  uint nBandH = (level == 1u) ? 31u : ((64u + (1u << (level - 1u))) >> level);
  uint off = 0u;
  uint hl = base + off; off += nBandH * nBandL;
  uint lh = base + off; off += nBandL * nBandH;
  uint hh = base + off; off += nBandH * nBandH;
  uint ll = base + off;
  uint dstStep = nBandL + nBandH;
  uint l = temp;
  uint h = temp + nBandL * dstStep;
  progIdwtX(ll, nBandL, hl, nBandH, l, dstStep, nBandL, nBandH, nBandL);
  progIdwtX(lh, nBandL, hh, nBandH, h, dstStep, nBandL, nBandH, nBandH);
  progIdwtY(l, dstStep, h, dstStep, base, dstStep, nBandL, nBandH, nBandL + nBandH);
}

void extrapolateDwt(uint base, uint temp) {
  progDwtBlock(base + 3807u, temp, 3u);
  progDwtBlock(base + 3007u, temp, 2u);
  progDwtBlock(base + 0u, temp, 1u);
}

// ---- kUpgrade: SRL/raw refinement (port of progressive_rfx_upgrade_*) ----
const uint kSubOff[10] = uint[10](0u, 1023u, 2046u, 3007u, 3279u, 3551u, 3807u, 3879u, 3951u, 4015u);
const uint kSubLen[10] = uint[10](1023u, 1023u, 961u, 272u, 272u, 256u, 72u, 72u, 64u, 81u);

int srlRead(inout int kp, inout int nz, inout int mode, uint numBits) {
  if (nz > 0) { nz--; return 0; }
  uint k = uint(kp) / 8u;
  if (mode == 0) {
    uint bit = ((srlPeek() & 0x80000000u) != 0u) ? 1u : 0u;
    srlShift(1);
    if (bit == 0u) {
      nz = int(1u << k);
      kp += 4; if (kp > 80) kp = 80;
      nz--;
      return 0;
    }
    nz = 0; mode = 1;
    if (k != 0u) {
      nz = int((srlPeek() >> (32u - k)) & ((1u << k) - 1u));
      srlShift(int(k));
    }
    if (nz != 0) { nz--; return 0; }
  }
  mode = 0;
  uint signBit = ((srlPeek() & 0x80000000u) != 0u) ? 1u : 0u;
  srlShift(1);
  if (kp < 6) kp = 0; else kp -= 6;
  if (numBits == 1u) return (signBit != 0u) ? -1 : 1;
  uint mag = 1u;
  uint maxMag = (1u << numBits) - 1u;
  while (mag < maxMag) {
    uint bit = ((srlPeek() & 0x80000000u) != 0u) ? 1u : 0u;
    srlShift(1);
    if (bit != 0u) break;
    mag++;
  }
  if (mag > 32767u) mag = 32767u;
  return (signBit != 0u) ? -int(mag) : int(mag);
}

void upgradeBlock(bool nonLL, inout int kp, inout int nz, inout int mode, uint stateBase, uint off,
                  uint len, uint shift, uint numBits) {
  if (numBits == 0u) return;
  if (!nonLL) {
    for (uint i = 0u; i < len; i++) {
      int inVal = int(brReadBits(int(numBits)));
      curSet(stateBase + off + i, i16(curGet(stateBase + off + i) + shl16(inVal, shift)));
    }
    return;
  }
  for (uint i = 0u; i < len; i++) {
    int s = signGet(stateBase + off + i);
    int inVal = 0;
    if (s > 0) { inVal = int(brReadBits(int(numBits))); }
    else if (s < 0) { inVal = -int(brReadBits(int(numBits))); }
    else { inVal = srlRead(kp, nz, mode, numBits); signSet(stateBase + off + i, i16(inVal)); }
    curSet(stateBase + off + i, i16(curGet(stateBase + off + i) + shl16(inVal, shift)));
  }
}

void upgradeComponent(uint compBase, uint tempBase, uint stateBase, uint tileStream, uint srlOff,
                      uint srlLen, uint rawOff, uint rawLen, const uint newBit[10],
                      const uint oldBit[10], const uint shift[10]) {
  srlInit(srlOff, srlLen);
  brInit(rawOff, rawLen);
  int kp = 8; int nz = 0; int mode = 0;
  for (int b = 0; b < 9; b++) {
    uint nb = newBit[b];
    uint ob = oldBit[b];
    uint numBits = (ob > nb) ? (ob - nb) : 0u;
    uint shv = (nb > 0u) ? (nb - 1u) : 0u;
    upgradeBlock(true, kp, nz, mode, stateBase, kSubOff[b], kSubLen[b], shv, numBits);
  }
  {
    uint nb = newBit[9];
    uint ob = oldBit[9];
    uint numBits = (ob > nb) ? (ob - nb) : 0u;
    uint shv = (nb > 0u) ? (nb - 1u) : 0u;
    upgradeBlock(false, kp, nz, mode, stateBase, kSubOff[9], kSubLen[9], shv, numBits);
  }
  for (uint i = 0u; i < 4096u; i++) coefSet(compBase + i, curGet(stateBase + i));
  extrapolateDwt(compBase, tempBase);
  for (int b = 0; b < 10; b++) bitPosSet(tileStream, uint(b), newBit[b]);
}

void main() {
  uint sid = gl_GlobalInvocationID.x;
  if (sid >= uNumStreams) return;
  uint mb = sid * 64u;
  uint type = metaByte(mb + 0u);
  uint flags = metaByte(mb + 1u);
  uint tileStream = metaU32(mb + 4u);
  uint payOff = metaU32(mb + 8u);
  uint payLen = metaU32(mb + 12u);
  uint srlOff = metaU32(mb + 16u);
  uint srlLen = metaU32(mb + 20u);
  uint rawOff = metaU32(mb + 24u);
  uint rawLen = metaU32(mb + 28u);
  uint compBase = uCompBase + sid * 4096u;
  uint stateBase = tileStream * 4096u;
  uint tempBase = uTempBase + sid * 4096u;

  uint sh[10];
  uint newBit[10];
  for (uint i = 0u; i < 10u; i++) {
    sh[i] = metaByte(mb + 32u + i);
    newBit[i] = metaByte(mb + 42u + i);
  }

  if (type == 0u) {
    rlgrDecode(payOff, payLen, compBase);
    for (uint i = 0u; i < 4096u; i++) signSet(stateBase + i, coefGet(compBase + i));
    dequantSub(compBase, 0u, 1023u, int(sh[0]));
    dequantSub(compBase, 1023u, 1023u, int(sh[1]));
    dequantSub(compBase, 2046u, 961u, int(sh[2]));
    dequantSub(compBase, 3007u, 272u, int(sh[3]));
    dequantSub(compBase, 3279u, 272u, int(sh[4]));
    dequantSub(compBase, 3551u, 256u, int(sh[5]));
    dequantSub(compBase, 3807u, 72u, int(sh[6]));
    dequantSub(compBase, 3879u, 72u, int(sh[7]));
    dequantSub(compBase, 3951u, 64u, int(sh[8]));
    diffDecode(compBase, 4015u, 81u);
    dequantSub(compBase, 4015u, 81u, int(sh[9]));
    if ((flags & 1u) != 0u) {
      for (uint i = 0u; i < 4096u; i++) {
        int v = coefGet(compBase + i) + curGet(stateBase + i);
        if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
        coefSet(compBase + i, v);
        curSet(stateBase + i, v);
      }
    } else {
      for (uint i = 0u; i < 4096u; i++) curSet(stateBase + i, coefGet(compBase + i));
    }
    for (uint b = 0u; b < 10u; b++) bitPosSet(tileStream, b, newBit[b]);
    extrapolateDwt(compBase, tempBase);
  } else {
    uint oldBit[10];
    for (uint b = 0u; b < 10u; b++) oldBit[b] = bitPosGet(tileStream, b);
    upgradeComponent(compBase, tempBase, stateBase, tileStream, srlOff, srlLen, rawOff, rawLen,
                     newBit, oldBit, sh);
  }
}
)GLSL";

const char* kComposeShader = R"GLSL(#version 310 es
precision highp int;
precision highp float;

layout(local_size_x = 64) in;

layout(std430, binding = 0) readonly buffer TileMeta { uint data[]; } tmeta;
layout(std430, binding = 1) readonly buffer Coef { uint data[]; } coef;
layout(std430, binding = 2) buffer Out { uint data[]; } outSurf;
layout(std430, binding = 3) readonly buffer Rects { uint data[]; } rcts;

uniform uint uNumTiles;
uniform uint uCompBase;
uniform int uSurfaceW;
uniform int uSurfaceH;
uniform int uKr;
uniform int uKcrG;
uniform int uKcbG;
uniform int uKcbB;

int i16lo(uint w) { return int(w << 16) >> 16; }
int i16hi(uint w) { return int(w) >> 16; }
int coefGet(uint idx) { uint w = coef.data[idx >> 1]; return ((idx & 1u) == 0u) ? i16lo(w) : i16hi(w); }

void surfPut(uint px, uint py, uint b, uint g, uint r) {
  // One BGRA pixel per 32-bit word: byte0=B, byte1=G, byte2=R, byte3=0xFF.
  // The desktop is not a whole number of tiles, so the right/bottom tiles are
  // partial: pixels outside the surface must be dropped, otherwise they wrap
  // into the next row (stride = surface width) and corrupt it.
  if (px >= uint(uSurfaceW) || py >= uint(uSurfaceH)) return;
  uint idx = py * uint(uSurfaceW) + px;
  outSurf.data[idx] = (b & 0xFFu) | ((g & 0xFFu) << 8) | ((r & 0xFFu) << 16) | (0xFFu << 24);
}

void main() {
  uint t = gl_GlobalInvocationID.x;
  if (t >= uNumTiles) return;
  uint px0 = tmeta.data[t * 4u];
  uint py0 = tmeta.data[t * 4u + 1u];
  uint rectOff = tmeta.data[t * 4u + 2u];
  uint rectCnt = tmeta.data[t * 4u + 3u];
  uint base0 = uCompBase + (t * 3u + 0u) * 4096u;
  uint base1 = uCompBase + (t * 3u + 1u) * 4096u;
  uint base2 = uCompBase + (t * 3u + 2u) * 4096u;
  for (uint row = 0u; row < 64u; row++) {
    for (uint col = 0u; col < 64u; col++) {
      uint i = row * 64u + col;
      int y = coefGet(base0 + i);
      int cb = coefGet(base1 + i);
      int cr = coefGet(base2 + i);
      int yv = int((uint(y) + 4096u) << 16);
      int sR = int(uint(cr) * uint(uKr) + uint(yv));
      int sG = int(uint(yv) - uint(cb) * uint(uKcbG) - uint(cr) * uint(uKcrG));
      int sB = int(uint(cb) * uint(uKcbB) + uint(yv));
      int r = clamp(sR >> 21, 0, 255);
      int g = clamp(sG >> 21, 0, 255);
      int b = clamp(sB >> 21, 0, 255);
      // FreeRDP composites a decoded tile only inside the region's clip rects;
      // the rest of the tile keeps the previous surface content.
      uint px = px0 + col;
      uint py = py0 + row;
      bool inside = (rectCnt == 0u);
      for (uint ri = 0u; !inside && ri < rectCnt; ri++) {
        uint a = rcts.data[(rectOff + ri) * 2u];
        uint c = rcts.data[(rectOff + ri) * 2u + 1u];
        uint rx = a & 0xFFFFu;
        uint ry = a >> 16;
        uint rw = c & 0xFFFFu;
        uint rh = c >> 16;
        if (px >= rx && px < rx + rw && py >= ry && py < ry + rh) inside = true;
      }
      if (inside) surfPut(px, py, uint(b), uint(g), uint(r));
    }
  }
}
)GLSL";

// B2: fill rects with a solid BGRA colour (one invocation per pixel).
const char* kFillShader = R"GLSL(#version 310 es
precision highp int;
layout(local_size_x = 64) in;
layout(std430, binding = 0) buffer Dst { uint data[]; } dst;
uniform uint uStride;   // bytes per row
uniform int uLeft;
uniform int uTop;
uniform int uWidth;
uniform int uHeight;
uniform uint uColor;    // B | G<<8 | R<<16 | 0xFF<<24
void main() {
  // 2D grid so a full-desktop rect can exceed the (typically 65535) per-axis
  // work-group limit; with gy == 1 this is the plain 1D index.
  uint i = gl_GlobalInvocationID.x +
           gl_GlobalInvocationID.y * (gl_NumWorkGroups.x * gl_WorkGroupSize.x);
  uint total = uint(uWidth) * uint(uHeight);
  if (i >= total) return;
  int x = uLeft + int(i % uint(uWidth));
  int y = uTop + int(i / uint(uWidth));
  dst.data[uint(y) * (uStride >> 2) + uint(x)] = uColor;
}
)GLSL";

// B2: copy a rect between two BGRA buffers (one invocation per pixel).
const char* kCopyShader = R"GLSL(#version 310 es
precision highp int;
layout(local_size_x = 64) in;
layout(std430, binding = 0) readonly buffer Src { uint data[]; } src;
layout(std430, binding = 1) buffer Dst { uint data[]; } dst;
uniform uint uSrcStride;
uniform uint uDstStride;
uniform int uSrcX;
uniform int uSrcY;
uniform int uDstX;
uniform int uDstY;
uniform int uWidth;
uniform int uHeight;
void main() {
  // 2D grid so a full-desktop rect can exceed the (typically 65535) per-axis
  // work-group limit; with gy == 1 this is the plain 1D index.
  uint i = gl_GlobalInvocationID.x +
           gl_GlobalInvocationID.y * (gl_NumWorkGroups.x * gl_WorkGroupSize.x);
  uint total = uint(uWidth) * uint(uHeight);
  if (i >= total) return;
  uint col = i % uint(uWidth);
  uint row = i / uint(uWidth);
  dst.data[(uint(uDstY) + row) * (uDstStride >> 2) + uint(uDstX) + col] =
      src.data[(uint(uSrcY) + row) * (uSrcStride >> 2) + uint(uSrcX) + col];
}
)GLSL";

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool CheckGl(const char* what) {
  const GLenum err = glGetError();
  if (err != GL_NO_ERROR) {
    HMRDP_LOGE("gpu rfx: %{public}s gl error 0x%{public}x", what, err);
    return false;
  }
  return true;
}

GLuint CompileProgram(const char* source, const char* name) {
  GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
  if (shader == 0) {
    return 0;
  }
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (ok != GL_TRUE) {
    char log[1024] = {0};
    glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
    HMRDP_LOGE("gpu rfx: %{public}s shader compile failed: %{public}s", name, log);
    glDeleteShader(shader);
    return 0;
  }
  GLuint prog = glCreateProgram();
  glAttachShader(prog, shader);
  glLinkProgram(prog);
  glDeleteShader(shader);
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (ok != GL_TRUE) {
    char log[1024] = {0};
    glGetProgramInfoLog(prog, sizeof(log) - 1, nullptr, log);
    HMRDP_LOGE("gpu rfx: %{public}s program link failed: %{public}s", name, log);
    glDeleteProgram(prog);
    return 0;
  }
  return prog;
}

EGLContext CreateOffscreen(EGLDisplay display, EGLConfig* outConfig, EGLSurface* outSurface) {
  const EGLint configAttribs[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
      EGL_NONE};
  EGLint numConfigs = 0;
  if (eglChooseConfig(display, configAttribs, outConfig, 1, &numConfigs) != EGL_TRUE ||
      numConfigs < 1) {
    return EGL_NO_CONTEXT;
  }
  const EGLint pbAttribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
  *outSurface = eglCreatePbufferSurface(display, *outConfig, pbAttribs);
  if (*outSurface == EGL_NO_SURFACE) {
    return EGL_NO_CONTEXT;
  }
  // Join the process-wide share group so the Renderer's window context can
  // sample the engine's screen texture (PERF-TODO §3.4 / §11.4).
  const EGLContext share = SharedEglAnchorContext();
  const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3,
                               EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
  EGLContext ctx = eglCreateContext(display, *outConfig, share, ctxAttribs);
  if (ctx != EGL_NO_CONTEXT) {
    return ctx;
  }
  const EGLint ctx3[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  return eglCreateContext(display, *outConfig, share, ctx3);
}

}  // namespace

// ---------------------------------------------------------------------------
// Capability probe
// ---------------------------------------------------------------------------

std::string GpuComputeInfo::Describe() const {
  char buf[320];
  std::snprintf(buf, sizeof(buf),
                "egl=%d compute=%d gl=%d.%d renderer=%s version=%s wg=%d shared=%d ssbo=%d tex=%d",
                egl ? 1 : 0, compute ? 1 : 0, glMajor, glMinor, renderer, version,
                maxWorkGroupInvocations, maxSharedMemory, maxSsboSize, maxTextureSize);
  return std::string(buf);
}

const GpuComputeInfo& GetGpuComputeInfo() {
  static std::once_flag once;
  static GpuComputeInfo info;
  std::call_once(once, []() {
    EGLDisplay display = SharedEglDisplay();
    if (display == EGL_NO_DISPLAY) {
      HMRDP_LOGE("gpu compute: egl init failed");
      return;
    }
    info.egl = true;
    EGLConfig config = nullptr;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = CreateOffscreen(display, &config, &surface);
    if (context == EGL_NO_CONTEXT ||
        eglMakeCurrent(display, surface, surface, context) != EGL_TRUE) {
      HMRDP_LOGE("gpu compute: offscreen context failed");
      return;
    }
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    if (version != nullptr) {
      std::snprintf(info.version, sizeof(info.version), "%s", version);
      int major = 0, minor = 0;
      if (std::sscanf(version, "OpenGL ES %d.%d", &major, &minor) == 2) {
        info.glMajor = major;
        info.glMinor = minor;
      }
    }
    if (renderer != nullptr) {
      std::snprintf(info.renderer, sizeof(info.renderer), "%s", renderer);
    }
    info.compute = (info.glMajor > 3) || (info.glMajor == 3 && info.glMinor >= 1);
    if (info.compute) {
      glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &info.maxWorkGroupInvocations);
      glGetIntegerv(GL_MAX_COMPUTE_SHARED_MEMORY_SIZE, &info.maxSharedMemory);
      glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &info.maxSsboSize);
      glGetIntegerv(GL_MAX_TEXTURE_SIZE, &info.maxTextureSize);
    }
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    HMRDP_LOGI("gpu compute: %{public}s", info.Describe().c_str());
  });
  return info;
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

struct GfxGpuDesktop::Impl {
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLSurface surface = EGL_NO_SURFACE;
  EGLContext context = EGL_NO_CONTEXT;
  bool current = false;
  GfxClearDecoder* clearDecoder = nullptr;

  GLuint decodeProg = 0;
  GLuint composeProg = 0;
  GLuint fillProg = 0;
  GLuint copyProg = 0;
  GLuint coefBuf = 0;

  // Rotating set of SSBOs for the buffers rewritten on every GFX command
  // (payload / per-chunk meta / compose rects). The CPU writes slot k+1 while
  // the GPU may still be reading slot k, so no implicit sync and no per-write
  // reallocation is needed - the textbook multi-buffering for UMA targets where
  // memory is shared and the hazard is ordering, not bandwidth.
  struct SsboRing {
    static constexpr int kSlots = 3;
    GLuint buf[kSlots] = {0, 0, 0};
    size_t cap[kSlots] = {0, 0, 0};
    int slot = 0;
    GLuint Next() {
      slot = (slot + 1) % kSlots;
      return buf[slot];
    }
    size_t& Cap() { return cap[slot]; }
  };
  SsboRing payloadRing;
  SsboRing metaRing;
  SsboRing tileMetaRing;
  SsboRing composeRectRing;

  GLuint decodeNumStreams = 0, decodeCompBase = 0, decodeTempBase = 0;
  GLuint composeNumTiles = 0, composeCompBase = 0;
  GLuint composeSurfaceW = 0, composeSurfaceH = 0;
  GLuint composeKr = 0, composeKcrG = 0, composeKcbG = 0, composeKcbB = 0;

  // One GPU surface: public metadata + the GL buffers that used to be single
  // instance members. The progressive state is per-surface so tile indices can
  // never leak across surfaces.
  struct SurfaceGpu {
    GpuSurface meta;
    GLuint outBuf = 0;
    GLuint stateCurBuf = 0;
    GLuint stateSignBuf = 0;
    GLuint stateBpBuf = 0;
    size_t outWords = 0;
  };
  std::map<uint16_t, SurfaceGpu> surfaces;

  SurfaceGpu* Find(uint16_t id) {
    const auto it = surfaces.find(id);
    return it == surfaces.end() ? nullptr : &it->second;
  }
  const SurfaceGpu* Find(uint16_t id) const {
    const auto it = surfaces.find(id);
    return it == surfaces.end() ? nullptr : &it->second;
  }

  // B2 surface commands.
  GLuint fillStride = 0, fillLeft = 0, fillTop = 0, fillWidth = 0, fillHeight = 0, fillColor = 0;
  GLuint copySrcStride = 0, copyDstStride = 0, copySrcX = 0, copySrcY = 0, copyDstX = 0,
         copyDstY = 0, copyWidth = 0, copyHeight = 0;
  struct CacheBuf {
    int width = 0;
    int height = 0;
    int stride = 0;
    GLuint buf = 0;
  };
  std::map<uint16_t, CacheBuf> cache;
  GLuint tempBuf = 0;
  size_t tempWords = 0;

  // Front (screen) buffer the output-mapped surfaces composite into (W4). The
  // buffer holds the pixels; `screenTex` mirrors it (GPU-to-GPU PBO upload) so
  // the Renderer can sample it from its own share-group context (W6).
  GLuint screenBuf = 0;
  GLuint screenTex = 0;
  size_t screenWords = 0;
  int screenW = 0;
  int screenH = 0;
  bool screenDirtyValid = false;
  int screenDirtyL = 0;
  int screenDirtyT = 0;
  int screenDirtyR = 0;
  int screenDirtyB = 0;

  size_t coefWords = 0;

  // Staging buffer for the ClearCodec batch: the union rectangle of a run is
  // packed here with a tight stride so the CPU only ever maps - and the driver
  // only ever has to cache-maintain/write back - the pixels the bands actually
  // need, instead of whole padded rows.
  GLuint clearStageBuf = 0;
  size_t clearStageBytes = 0;

  // Reusable CPU staging scratch, grown on demand: the per-chunk Progressive
  // meta blobs. Reallocating these per command is avoidable heap churn when the
  // stream carries thousands of commands per second.
  std::vector<uint8_t> metaScratch;
  std::vector<uint32_t> tileMetaScratch;

  // Queued ClearCodec commands sharing one GPU map (see FlushPendingClears).
  struct PendingClear {
    uint16_t surfaceId = 0;
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> payload;
  };
  std::vector<PendingClear> pendingClears;
  // Undecoded band bytes currently queued (memory safety bound, not a
  // performance knob).
  size_t pendingBytes = 0;
  // Union rectangle of the queued run, maintained as commands are appended so
  // the batch can be capped by area (see clearBatchAreaLimit).
  int queueRowMin = 0;
  int queueRowMax = 0;
  int queueColMin = 0;
  int queueColMax = 0;
  int clearBatchAreaLimit = 1 << 20;  // pixels; 0 = no batching
  // Dev instrumentation for the ClearCodec read-modify-write path.
  uint64_t clearFlushes = 0;
  uint64_t clearCmds = 0;
  uint64_t clearMapUs = 0;
  uint64_t clearDecodeUs = 0;
  uint64_t clearMapBytes = 0;
  // Union rectangle of the batches, to judge how much of the mapped rows/bytes
  // is actually needed by the bands.
  uint64_t clearUnionPixels = 0;
  uint64_t clearSpanPixels = 0;

  static void MarkSurfaceDirty(SurfaceGpu& s, int left, int top, int right, int bottom) {
    if (right <= left || bottom <= top) {
      return;
    }
    GpuSurface& m = s.meta;
    if (!m.dirtyValid) {
      m.dirtyValid = true;
      m.dirtyLeft = left;
      m.dirtyTop = top;
      m.dirtyRight = right;
      m.dirtyBottom = bottom;
      return;
    }
    if (left < m.dirtyLeft) m.dirtyLeft = left;
    if (top < m.dirtyTop) m.dirtyTop = top;
    if (right > m.dirtyRight) m.dirtyRight = right;
    if (bottom > m.dirtyBottom) m.dirtyBottom = bottom;
  }

  void MarkScreenDirty(int left, int top, int right, int bottom) {
    if (right <= left || bottom <= top) {
      return;
    }
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > screenW) right = screenW;
    if (bottom > screenH) bottom = screenH;
    if (right <= left || bottom <= top) {
      return;
    }
    if (!screenDirtyValid) {
      screenDirtyValid = true;
      screenDirtyL = left;
      screenDirtyT = top;
      screenDirtyR = right;
      screenDirtyB = bottom;
      return;
    }
    if (left < screenDirtyL) screenDirtyL = left;
    if (top < screenDirtyT) screenDirtyT = top;
    if (right > screenDirtyR) screenDirtyR = right;
    if (bottom > screenDirtyB) screenDirtyB = bottom;
  }

  bool MakeCurrent() {
    if (display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT) {
      return false;
    }
    // eglMakeCurrent is expensive (a backend round trip) and the engine
    // is invoked once per GFX command; the same thread usually already has the
    // context bound. The Renderer binds its own context on the same thread
    // between engine calls, so query the thread's current context instead of
    // trusting a cached flag.
    if (current && eglGetCurrentContext() == context) {
      return true;
    }
    if (eglMakeCurrent(display, surface, surface, context) != EGL_TRUE) {
      current = false;
      return false;
    }
    current = true;
    return true;
  }
};


GfxGpuDesktop::GfxGpuDesktop(GfxClearDecoder* clearDecoder) : impl_(new Impl()) {
  impl_->clearDecoder = clearDecoder;
}
GfxGpuDesktop::~GfxGpuDesktop() {
  Reset();
  delete impl_;
  impl_ = nullptr;
}

void GfxGpuDesktop::Reset() {
  if (impl_ == nullptr) {
    return;
  }
  if (impl_->context != EGL_NO_CONTEXT) {
    impl_->MakeCurrent();
    for (Impl::SsboRing* ring : {&impl_->payloadRing, &impl_->metaRing, &impl_->tileMetaRing,
                                 &impl_->composeRectRing}) {
      for (int i = 0; i < Impl::SsboRing::kSlots; ++i) {
        if (ring->buf[i] != 0) {
          glDeleteBuffers(1, &ring->buf[i]);
          ring->buf[i] = 0;
        }
        ring->cap[i] = 0;
      }
      ring->slot = 0;
    }
    if (impl_->coefBuf) glDeleteBuffers(1, &impl_->coefBuf);
    if (impl_->clearStageBuf) glDeleteBuffers(1, &impl_->clearStageBuf);
    if (impl_->tempBuf) glDeleteBuffers(1, &impl_->tempBuf);
    if (impl_->screenBuf) glDeleteBuffers(1, &impl_->screenBuf);
    if (impl_->screenTex) glDeleteTextures(1, &impl_->screenTex);
    for (auto& kv : impl_->surfaces) {
      Impl::SurfaceGpu& s = kv.second;
      if (s.outBuf) glDeleteBuffers(1, &s.outBuf);
      if (s.stateCurBuf) glDeleteBuffers(1, &s.stateCurBuf);
      if (s.stateSignBuf) glDeleteBuffers(1, &s.stateSignBuf);
      if (s.stateBpBuf) glDeleteBuffers(1, &s.stateBpBuf);
    }
    impl_->surfaces.clear();
    impl_->pendingClears.clear();
    impl_->pendingBytes = 0;
    for (auto& kv : impl_->cache) {
      if (kv.second.buf != 0) {
        glDeleteBuffers(1, &kv.second.buf);
      }
    }
    impl_->cache.clear();
    if (impl_->decodeProg) glDeleteProgram(impl_->decodeProg);
    if (impl_->composeProg) glDeleteProgram(impl_->composeProg);
    if (impl_->fillProg) glDeleteProgram(impl_->fillProg);
    if (impl_->copyProg) glDeleteProgram(impl_->copyProg);
    impl_->coefBuf = 0;
    impl_->clearStageBuf = 0;
    impl_->clearStageBytes = 0;
    impl_->tempBuf = 0;
    impl_->tempWords = 0;
    impl_->screenBuf = 0;
    impl_->screenTex = 0;
    impl_->screenWords = 0;
    impl_->screenW = 0;
    impl_->screenH = 0;
    impl_->screenDirtyValid = false;
    impl_->decodeProg = impl_->composeProg = impl_->fillProg = impl_->copyProg = 0;
    eglMakeCurrent(impl_->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(impl_->display, impl_->context);
    eglDestroySurface(impl_->display, impl_->surface);
    impl_->context = EGL_NO_CONTEXT;
    impl_->surface = EGL_NO_SURFACE;
  }
  impl_->display = EGL_NO_DISPLAY;
  screenW_ = 0;
  screenH_ = 0;
  ready_ = false;
}

bool GfxGpuDesktop::Init() {
  if (impl_ == nullptr) {
    return false;
  }
  Reset();

  const GpuComputeInfo& info = GetGpuComputeInfo();
  if (!info.compute) {
    HMRDP_LOGW("gpu rfx: compute unsupported (%{public}s)", info.Describe().c_str());
    return false;
  }

  impl_->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (impl_->display == EGL_NO_DISPLAY ||
      eglInitialize(impl_->display, nullptr, nullptr) != EGL_TRUE) {
    return false;
  }
  EGLConfig config = nullptr;
  EGLSurface surface = EGL_NO_SURFACE;
  impl_->context = CreateOffscreen(impl_->display, &config, &surface);
  impl_->surface = surface;
  if (impl_->context == EGL_NO_CONTEXT || !impl_->MakeCurrent()) {
    return false;
  }
  impl_->decodeProg = CompileProgram(kDecodeShader, "decode");
  impl_->composeProg = CompileProgram(kComposeShader, "compose");
  impl_->fillProg = CompileProgram(kFillShader, "fill");
  impl_->copyProg = CompileProgram(kCopyShader, "copy");
  if (impl_->decodeProg == 0 || impl_->composeProg == 0 || impl_->fillProg == 0 ||
      impl_->copyProg == 0) {
    return false;
  }

  // Program uniforms.
  impl_->decodeNumStreams = glGetUniformLocation(impl_->decodeProg, "uNumStreams");
  impl_->decodeCompBase = glGetUniformLocation(impl_->decodeProg, "uCompBase");
  impl_->decodeTempBase = glGetUniformLocation(impl_->decodeProg, "uTempBase");
  impl_->composeNumTiles = glGetUniformLocation(impl_->composeProg, "uNumTiles");
  impl_->composeCompBase = glGetUniformLocation(impl_->composeProg, "uCompBase");
  impl_->composeSurfaceW = glGetUniformLocation(impl_->composeProg, "uSurfaceW");
  impl_->composeSurfaceH = glGetUniformLocation(impl_->composeProg, "uSurfaceH");
  impl_->composeKr = glGetUniformLocation(impl_->composeProg, "uKr");
  impl_->composeKcrG = glGetUniformLocation(impl_->composeProg, "uKcrG");
  impl_->composeKcbG = glGetUniformLocation(impl_->composeProg, "uKcbG");
  impl_->composeKcbB = glGetUniformLocation(impl_->composeProg, "uKcbB");
  impl_->fillStride = glGetUniformLocation(impl_->fillProg, "uStride");
  impl_->fillLeft = glGetUniformLocation(impl_->fillProg, "uLeft");
  impl_->fillTop = glGetUniformLocation(impl_->fillProg, "uTop");
  impl_->fillWidth = glGetUniformLocation(impl_->fillProg, "uWidth");
  impl_->fillHeight = glGetUniformLocation(impl_->fillProg, "uHeight");
  impl_->fillColor = glGetUniformLocation(impl_->fillProg, "uColor");
  impl_->copySrcStride = glGetUniformLocation(impl_->copyProg, "uSrcStride");
  impl_->copyDstStride = glGetUniformLocation(impl_->copyProg, "uDstStride");
  impl_->copySrcX = glGetUniformLocation(impl_->copyProg, "uSrcX");
  impl_->copySrcY = glGetUniformLocation(impl_->copyProg, "uSrcY");
  impl_->copyDstX = glGetUniformLocation(impl_->copyProg, "uDstX");
  impl_->copyDstY = glGetUniformLocation(impl_->copyProg, "uDstY");
  impl_->copyWidth = glGetUniformLocation(impl_->copyProg, "uWidth");
  impl_->copyHeight = glGetUniformLocation(impl_->copyProg, "uHeight");

  // Per-chunk scratch: comp then temp, each chunkStreams*4096 int16.
  const uint32_t chunkStreams = kChunkTiles * 3;
  impl_->coefWords = static_cast<size_t>(chunkStreams) * 4096 / 2 * 2;  // two regions
  glGenBuffers(1, &impl_->coefBuf);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, impl_->coefBuf);
  glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(impl_->coefWords * 4), nullptr,
               GL_DYNAMIC_DRAW);

  const uint32_t chunkTiles = kChunkTiles;
  // Every ring slot is pre-allocated so a rotation never reallocates; capacities
  // only grow for the payload ring (sized to the largest GFX message seen).
  auto initRing = [](Impl::SsboRing* ring, size_t bytes) {
    for (int i = 0; i < Impl::SsboRing::kSlots; ++i) {
      glGenBuffers(1, &ring->buf[i]);
      glBindBuffer(GL_SHADER_STORAGE_BUFFER, ring->buf[i]);
      glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(bytes), nullptr,
                   GL_DYNAMIC_DRAW);
      ring->cap[i] = bytes;
    }
  };
  initRing(&impl_->metaRing, static_cast<size_t>(chunkStreams) * kMetaStride);
  initRing(&impl_->tileMetaRing, static_cast<size_t>(chunkTiles) * 16);
  initRing(&impl_->composeRectRing, static_cast<size_t>(chunkTiles) * 8 * sizeof(uint32_t));
  initRing(&impl_->payloadRing, 1u << 20);  // grown on demand

  if (!CheckGl("init")) {
    return false;
  }
  ready_ = true;
  HMRDP_LOGI("gpu gfx desktop: engine ready (%{public}s)", info.Describe().c_str());
  return true;
}

// ---------------------------------------------------------------------------
// Surface lifecycle
// ---------------------------------------------------------------------------

namespace {

// Rounds up to a multiple of `alignment` (mirrors gfx_align_scanline).
int GpuAlign(int value, int alignment) {
  const int pad = alignment - (value % alignment);
  return (pad == alignment) ? value : value + pad;
}

}  // namespace

bool GfxGpuDesktop::CreateSurface(uint16_t surfaceId, int width, int height, uint32_t format) {
  if (!ready_ || impl_ == nullptr || width <= 0 || height <= 0) {
    return false;
  }
  if (!impl_->MakeCurrent()) {
    return false;
  }
  DeleteSurface(surfaceId);

  Impl::SurfaceGpu surface;
  surface.meta.id = surfaceId;
  surface.meta.width = GpuAlign(width, 16);
  surface.meta.height = GpuAlign(height, 16);
  surface.meta.stride = GpuAlign(surface.meta.width * 4, 16);
  // FreeRDP maps the wire format 0x20 -> BGRX32, 0x21 -> BGRA32; anything else
  // is treated as BGRA32.
  surface.meta.format =
      (format == 0x20u) ? kPixelFormatBgrx32 : kPixelFormatBgra32;
  surface.meta.gridW = (surface.meta.width + 63) / 64;
  surface.meta.gridH = (surface.meta.height + 63) / 64;
  surface.meta.mappedWidth = width;
  surface.meta.mappedHeight = height;

  surface.outWords = static_cast<size_t>(surface.meta.stride) * surface.meta.height / 4;
  glGenBuffers(1, &surface.outBuf);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, surface.outBuf);
  glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(surface.outWords * 4), nullptr,
               GL_DYNAMIC_DRAW);
  {
    void* p = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
                               static_cast<GLsizeiptr>(surface.outWords * 4),
                               GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    if (p != nullptr) {
      std::memset(p, 0xFF, surface.outWords * 4);
      glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    }
  }

  // Persistent per-(tile,component) progressive state (current/sign int16 +
  // 10 nibble-ish bytes of bit positions), zero-initialised for this grid.
  const size_t gridStreams = static_cast<size_t>(surface.meta.gridW) * surface.meta.gridH * 3;
  const size_t stateCoefBytes = gridStreams * 4096 * 2;
  const size_t stateBpBytes = ((gridStreams * 10 + 3) / 4) * 4;
  auto makeZeroed = [](GLuint* buf, size_t bytes) {
    glGenBuffers(1, buf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, *buf);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(bytes), nullptr,
                 GL_DYNAMIC_DRAW);
    void* p = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(bytes),
                               GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    if (p != nullptr) {
      std::memset(p, 0, bytes);
      glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  };
  makeZeroed(&surface.stateCurBuf, stateCoefBytes);
  makeZeroed(&surface.stateSignBuf, stateCoefBytes);
  makeZeroed(&surface.stateBpBuf, stateBpBytes);

  glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  impl_->surfaces[surfaceId] = surface;
  if (!CheckGl("create surface")) {
    return false;
  }
  HMRDP_LOGI("gpu gfx desktop: surface %{public}u %{public}dx%{public}d stride=%{public}d",
             surfaceId, surface.meta.width, surface.meta.height, surface.meta.stride);
  return true;
}

void GfxGpuDesktop::DeleteSurface(uint16_t surfaceId) {
  if (impl_ == nullptr) {
    return;
  }
  FlushPendingClears();
  const auto it = impl_->surfaces.find(surfaceId);
  if (it == impl_->surfaces.end()) {
    return;
  }
  if (impl_->context != EGL_NO_CONTEXT) {
    impl_->MakeCurrent();
    Impl::SurfaceGpu& s = it->second;
    if (s.outBuf) glDeleteBuffers(1, &s.outBuf);
    if (s.stateCurBuf) glDeleteBuffers(1, &s.stateCurBuf);
    if (s.stateSignBuf) glDeleteBuffers(1, &s.stateSignBuf);
    if (s.stateBpBuf) glDeleteBuffers(1, &s.stateBpBuf);
  }
  impl_->surfaces.erase(it);
}

const GpuSurface* GfxGpuDesktop::FindSurface(uint16_t surfaceId) const {
  const Impl::SurfaceGpu* s = impl_ != nullptr ? impl_->Find(surfaceId) : nullptr;
  return s != nullptr ? &s->meta : nullptr;
}

void GfxGpuDesktop::MapSurfaceToOutput(uint16_t surfaceId, uint32_t outputOriginX,
                                       uint32_t outputOriginY) {
  Impl::SurfaceGpu* s = impl_ != nullptr ? impl_->Find(surfaceId) : nullptr;
  if (s != nullptr) {
    s->meta.mapped = true;
    s->meta.outputX = outputOriginX;
    s->meta.outputY = outputOriginY;
    // gdi_MapSurfaceToOutput clears the surface's invalid region.
    s->meta.dirtyValid = false;
  }
}

// ---------------------------------------------------------------------------
// Command dispatch (FreeRDP GFX command semantics)
// ---------------------------------------------------------------------------

namespace {

uint32_t GpuRd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t GpuRd16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

}  // namespace

void GfxGpuDesktop::ApplyCommand(uint16_t cmdId, uint32_t surfaceId, const uint32_t scalars[4],
                                 const uint8_t* params, uint32_t paramsLen, const uint8_t* payload,
                                 uint32_t payloadLen) {
  // Every command that is not itself a queued ClearCodec must see the surface
  // with all earlier ClearCodec writes applied, so the pending run is flushed
  // first. (A run is contiguous by construction, so this preserves order.)
  const bool isClear = (cmdId == kGpuCmdWireToSurface && scalars != nullptr &&
                        scalars[0] == kGpuCodecClearCodec);
  if (!isClear) {
    FlushPendingClears();
  }
  switch (cmdId) {
    case kGpuCmdCreateSurface:
      if (scalars != nullptr) {
        CreateSurface(static_cast<uint16_t>(surfaceId), static_cast<int>(scalars[0]),
                      static_cast<int>(scalars[1]), scalars[2]);
      }
      break;
    case kGpuCmdDeleteSurface:
      DeleteSurface(static_cast<uint16_t>(surfaceId));
      break;
    case kGpuCmdSolidFill:
      if (scalars != nullptr && params != nullptr) {
        // FreeRDP always uses alpha 0xFF regardless of the PDU's XA byte.
        SolidFill(static_cast<uint16_t>(surfaceId), (scalars[0] & 0x00FFFFFFu) | 0xFF000000u,
                  reinterpret_cast<const uint16_t*>(params), scalars[1]);
      }
      break;
    case kGpuCmdSurfaceToSurface: {
      if (scalars == nullptr || params == nullptr || paramsLen < 8) {
        break;
      }
      const int sx = GpuRd16(params);
      const int sy = GpuRd16(params + 2);
      const int w = GpuRd16(params + 4) - sx;
      const int h = GpuRd16(params + 6) - sy;
      const uint32_t count = scalars[1];
      const uint32_t avail = (paramsLen - 8) / 4;
      const uint32_t n = count < avail ? count : avail;
      for (uint32_t i = 0; i < n; ++i) {
        const int px = GpuRd16(params + 8 + static_cast<size_t>(i) * 4);
        const int py = GpuRd16(params + 8 + static_cast<size_t>(i) * 4 + 2);
        SurfaceToSurface(static_cast<uint16_t>(scalars[0]), sx, sy, w, h,
                         static_cast<uint16_t>(surfaceId), px, py);
      }
      break;
    }
    case kGpuCmdSurfaceToCache: {
      if (scalars == nullptr || params == nullptr || paramsLen < 16) {
        break;
      }
      const int sx = GpuRd16(params + 8);
      const int sy = GpuRd16(params + 10);
      const int w = GpuRd16(params + 12) - sx;
      const int h = GpuRd16(params + 14) - sy;
      SurfaceToCache(static_cast<uint16_t>(surfaceId), static_cast<uint16_t>(scalars[0]), sx, sy,
                     w, h);
      break;
    }
    case kGpuCmdCacheToSurface: {
      if (scalars == nullptr || params == nullptr) {
        break;
      }
      const uint32_t count = scalars[1];
      const uint32_t avail = paramsLen / 4;
      const uint32_t n = count < avail ? count : avail;
      for (uint32_t i = 0; i < n; ++i) {
        const int px = GpuRd16(params + static_cast<size_t>(i) * 4);
        const int py = GpuRd16(params + static_cast<size_t>(i) * 4 + 2);
        CacheToSurface(static_cast<uint16_t>(surfaceId), static_cast<uint16_t>(scalars[0]), px, py);
      }
      break;
    }
    case kGpuCmdEvictCacheEntry:
      if (scalars != nullptr) {
        EvictCache(static_cast<uint16_t>(scalars[0]));
      }
      break;
    case kGpuCmdWireToSurface: {
      const uint16_t sid = static_cast<uint16_t>(surfaceId);
      if (params == nullptr || paramsLen < 32 || scalars == nullptr || impl_ == nullptr) {
        break;
      }
      Impl::SurfaceGpu* surface = impl_->Find(sid);
      if (surface == nullptr) {
        break;
      }
      const uint32_t codecId = scalars[0];
      const uint32_t format = GpuRd32(params + 4);
      const int left = static_cast<int>(GpuRd32(params + 8));
      const int top = static_cast<int>(GpuRd32(params + 12));
      const int width = static_cast<int>(GpuRd32(params + 24));
      const int height = static_cast<int>(GpuRd32(params + 28));
      if (codecId == kGpuCodecCaprogressive || codecId == kGpuCodecCaprogressiveV2) {
        DecodeMessage(sid, payload, payloadLen);
      } else if (codecId == kGpuCodecClearCodec) {
        // Queued, not decoded here: ClearCodec needs a GPU->CPU->GPU
        // read-modify-write and the stream carries long runs of them, so
        // consecutive same-surface commands are served by one shared map (see
        // FlushPendingClears). The read-modify-write itself is unavoidable
        // because ClearCodec is not self-contained: pixels its bands do not
        // overwrite keep the current surface value.
        if (impl_->clearDecoder != nullptr && payload != nullptr && width > 0 && height > 0) {
          const int top0 = top < 0 ? 0 : top;
          if (top0 + height <= surface->meta.height) {
            QueueClear(sid, payload, payloadLen, left, top0, width, height);
          }
        }
      } else if (codecId == kGpuCodecUncompressed) {
        const uint32_t bpp = format >> 24;
        if (width > 0 && height > 0 && payload != nullptr &&
            (bpp == 24 || bpp == 32) &&
            static_cast<uint64_t>(bpp / 8) * width * height <= payloadLen) {
          if (bpp == 32) {
            UploadBgra(sid, left, top, width, height, payload, width * 4);
          } else {
            std::vector<uint8_t> tmp(static_cast<size_t>(width) * height * 4);
            for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
              tmp[i * 4] = payload[i * 3];
              tmp[i * 4 + 1] = payload[i * 3 + 1];
              tmp[i * 4 + 2] = payload[i * 3 + 2];
              tmp[i * 4 + 3] = 0xFF;
            }
            UploadBgra(sid, left, top, width, height, tmp.data(), width * 4);
          }
          Impl::MarkSurfaceDirty(*surface, left, top, left + width, top + height);
        }
      }
      break;
    }
    case kGpuCmdMapSurfaceToOutput:
      if (scalars != nullptr) {
        MapSurfaceToOutput(static_cast<uint16_t>(surfaceId), scalars[0], scalars[1]);
      }
      break;
    case kGpuCmdMapSurfaceToScaledOutput:
      // Server-side scaling is unsupported (this build's FreeRDP has no
      // swscale/cairo, so gdi draws nothing). Unmap the surface so a stale 1:1
      // mapping is not reused for the scaled PDU (mirrors FreeRDP).
      if (impl_ != nullptr) {
        if (Impl::SurfaceGpu* s = impl_->Find(static_cast<uint16_t>(surfaceId))) {
          s->meta.mapped = false;
          s->meta.dirtyValid = false;
        }
      }
      break;
    case kGpuCmdResetGraphics:
      if (scalars != nullptr) {
        ResetGraphics(static_cast<int>(scalars[0]), static_cast<int>(scalars[1]));
      }
      break;
    default:
      break;
  }
}

namespace {

// uint8 nibbles [HL1 LH1 HH1 HL2 LH2 HH2 HL3 LH3 HH3 LL3] in the RfxQuant.
void QuantArray(const RfxQuant& q, uint8_t out[10]) {
  out[0] = q.HL1; out[1] = q.LH1; out[2] = q.HH1;
  out[3] = q.HL2; out[4] = q.LH2; out[5] = q.HH2;
  out[6] = q.HL3; out[7] = q.LH3; out[8] = q.HH3;
  out[9] = q.LL3;
}

struct StreamJob {
  uint32_t type = 0;   // 0 = kFirst, 2 = kUpgrade
  uint32_t flags = 0;  // bit0 = RFX_TILE_DIFFERENCE (kFirst)
  uint32_t tileStream = 0;
  uint32_t payloadOff = 0;
  uint32_t payloadLen = 0;
  uint32_t srlOff = 0;
  uint32_t srlLen = 0;
  uint32_t rawOff = 0;
  uint32_t rawLen = 0;
  uint8_t shift[10] = {0};
  uint8_t newBit[10] = {0};
};

}  // namespace

namespace {
// Defined with the other pixel-op helpers below; used by the ClearCodec staging
// pack/unpack.
inline void DispatchPixels(size_t pixels);
}  // namespace

void GfxGpuDesktop::QueueClear(uint16_t surfaceId, const uint8_t* payload, size_t payloadLen,
                               int left, int top, int width, int height) {
  if (impl_ == nullptr || payload == nullptr || payloadLen == 0) {
    return;
  }
  // A run never spans surfaces. Its length is bounded two ways: the union
  // rectangle may not exceed `clearBatchAreaLimit` pixels (this controls the
  // padding in the shared staging map), and the queued payloads may not exceed
  // the memory budget. Both are resource bounds; the area limit is the knob that
  // trades map/unmap round trips against mapped bytes.
  constexpr size_t kMaxPendingPayloadBytes = 4u << 20;  // 4 MB of undecoded bands
  const int top0 = top < 0 ? 0 : top;
  const int bottom0 = top0 + height;
  const int left0 = left < 0 ? 0 : left;
  const int right0 = left0 + width;
  const bool sameSurface =
      !impl_->pendingClears.empty() && impl_->pendingClears.back().surfaceId == surfaceId;
  bool fits = sameSurface;
  if (fits) {
    const int rmin = top0 < impl_->queueRowMin ? top0 : impl_->queueRowMin;
    const int rmax = bottom0 > impl_->queueRowMax ? bottom0 : impl_->queueRowMax;
    const int cmin = left0 < impl_->queueColMin ? left0 : impl_->queueColMin;
    const int cmax = right0 > impl_->queueColMax ? right0 : impl_->queueColMax;
    const long long area = static_cast<long long>(rmax - rmin) * static_cast<long long>(cmax - cmin);
    fits = impl_->clearBatchAreaLimit > 0 && area <= impl_->clearBatchAreaLimit;
  }
  if (!fits || impl_->pendingBytes + payloadLen > kMaxPendingPayloadBytes) {
    FlushPendingClears();
  }
  if (impl_->pendingClears.empty()) {
    impl_->queueRowMin = top0;
    impl_->queueRowMax = bottom0;
    impl_->queueColMin = left0;
    impl_->queueColMax = right0;
  } else {
    if (top0 < impl_->queueRowMin) impl_->queueRowMin = top0;
    if (bottom0 > impl_->queueRowMax) impl_->queueRowMax = bottom0;
    if (left0 < impl_->queueColMin) impl_->queueColMin = left0;
    if (right0 > impl_->queueColMax) impl_->queueColMax = right0;
  }
  Impl::PendingClear pc;
  pc.surfaceId = surfaceId;
  pc.left = left;
  pc.top = top;
  pc.width = width;
  pc.height = height;
  pc.payload.assign(payload, payload + payloadLen);
  impl_->pendingBytes += payloadLen;
  impl_->pendingClears.push_back(std::move(pc));
}

void GfxGpuDesktop::FlushPendingClears() {
  if (impl_ == nullptr || impl_->pendingClears.empty()) {
    return;
  }
  std::vector<Impl::PendingClear>& list = impl_->pendingClears;
  const uint16_t sid = list.front().surfaceId;
  Impl::SurfaceGpu* surface = impl_->Find(sid);
  if (surface == nullptr || impl_->clearDecoder == nullptr) {
    list.clear();
    impl_->pendingBytes = 0;
    return;
  }
  // The whole run is served by one map of its union rectangle. The rectangle is
  // packed into a tight-stride staging buffer first, so the range the CPU maps
  // (and the driver must make coherent / write back) is exactly the pixels the
  // bands need - not whole padded rows. This is a residency/cache-maintenance
  // win that matters on unified memory, where the cost of a read-modify-write is
  // the sync point and the touched range rather than raw transfer bandwidth.
  int rowMin = surface->meta.height;
  int rowMax = 0;
  int colMin = surface->meta.width;
  int colMax = 0;
  for (const Impl::PendingClear& pc : list) {
    const int t = pc.top < 0 ? 0 : pc.top;
    int b = pc.top + pc.height;
    if (b > surface->meta.height) {
      b = surface->meta.height;
    }
    if (t < rowMin) {
      rowMin = t;
    }
    if (b > rowMax) {
      rowMax = b;
    }
    const int l = pc.left < 0 ? 0 : pc.left;
    int r = pc.left + pc.width;
    if (r > surface->meta.width) {
      r = surface->meta.width;
    }
    if (l < colMin) {
      colMin = l;
    }
    if (r > colMax) {
      colMax = r;
    }
  }
  if (colMax > colMin) {
    impl_->clearUnionPixels +=
        static_cast<uint64_t>(rowMax - rowMin) * static_cast<uint64_t>(colMax - colMin);
    impl_->clearSpanPixels += static_cast<uint64_t>(rowMax - rowMin) *
                              static_cast<uint64_t>(surface->meta.width);
  }
  const int unionW = colMax - colMin;
  const int unionH = rowMax - rowMin;
  if (unionW <= 0 || unionH <= 0 || !impl_->MakeCurrent()) {
    list.clear();
    impl_->pendingBytes = 0;
    return;
  }
  const size_t stageStride = static_cast<size_t>(unionW) * 4;
  const size_t mapBytes = stageStride * static_cast<size_t>(unionH);

  // Grow the staging buffer to the batch's union rectangle (bounded by the
  // surface itself, so no arbitrary cap is needed).
  if (mapBytes > impl_->clearStageBytes) {
    impl_->clearStageBytes = mapBytes + (mapBytes >> 1);
    if (impl_->clearStageBuf == 0) {
      glGenBuffers(1, &impl_->clearStageBuf);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, impl_->clearStageBuf);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(impl_->clearStageBytes), nullptr,
                 GL_DYNAMIC_DRAW);
  }

  // Pack the union rectangle out of the surface (strided rect copy on the GPU).
  glUseProgram(impl_->copyProg);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, surface->outBuf);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, impl_->clearStageBuf);
  glUniform1ui(impl_->copySrcStride, static_cast<GLuint>(surface->meta.stride));
  glUniform1ui(impl_->copyDstStride, static_cast<GLuint>(stageStride));
  glUniform1i(impl_->copySrcX, colMin);
  glUniform1i(impl_->copySrcY, rowMin);
  glUniform1i(impl_->copyDstX, 0);
  glUniform1i(impl_->copyDstY, 0);
  glUniform1i(impl_->copyWidth, unionW);
  glUniform1i(impl_->copyHeight, unionH);
  DispatchPixels(static_cast<size_t>(unionW) * static_cast<size_t>(unionH));
  glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
  glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);

  const int64_t mapStart = NowUs();
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, impl_->clearStageBuf);
  void* mapped = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
                                  static_cast<GLsizeiptr>(mapBytes),
                                  GL_MAP_READ_BIT | GL_MAP_WRITE_BIT);
  impl_->clearMapUs += static_cast<uint64_t>(NowUs() - mapStart);
  if (mapped != nullptr) {
    uint8_t* base = static_cast<uint8_t*>(mapped);
    const int64_t decodeStart = NowUs();
    for (const Impl::PendingClear& pc : list) {
      const int top0 = pc.top < 0 ? 0 : pc.top;
      const int left0 = pc.left < 0 ? 0 : pc.left;
      // The decoder writes at its own band origin inside the packed rectangle;
      // clear_decompress only touches [0,pc.width)x[0,pc.height) of that origin,
      // so the tight stride and the bit-exact per-command call agree.
      uint8_t* dst = base + (static_cast<size_t>(top0 - rowMin) * static_cast<size_t>(unionW) +
                             static_cast<size_t>(left0 - colMin)) *
                               4;
      if (impl_->clearDecoder->Decode(pc.payload.data(), pc.payload.size(), pc.width, pc.height,
                                      surface->meta.format, dst, static_cast<int>(stageStride), 0, 0,
                                      unionW, unionH)) {
        Impl::MarkSurfaceDirty(*surface, left0, top0, left0 + pc.width, top0 + pc.height);
      }
    }
    impl_->clearDecodeUs += static_cast<uint64_t>(NowUs() - decodeStart);
    const int64_t unmapStart = NowUs();
    glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    impl_->clearMapUs += static_cast<uint64_t>(NowUs() - unmapStart);
  }

  // Unpack the decoded rectangle back onto the surface.
  glUseProgram(impl_->copyProg);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, impl_->clearStageBuf);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, surface->outBuf);
  glUniform1ui(impl_->copySrcStride, static_cast<GLuint>(stageStride));
  glUniform1ui(impl_->copyDstStride, static_cast<GLuint>(surface->meta.stride));
  glUniform1i(impl_->copySrcX, 0);
  glUniform1i(impl_->copySrcY, 0);
  glUniform1i(impl_->copyDstX, colMin);
  glUniform1i(impl_->copyDstY, rowMin);
  glUniform1i(impl_->copyWidth, unionW);
  glUniform1i(impl_->copyHeight, unionH);
  DispatchPixels(static_cast<size_t>(unionW) * static_cast<size_t>(unionH));
  glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

  glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  impl_->clearFlushes++;
  impl_->clearCmds += list.size();
  impl_->clearMapBytes += mapBytes;
  if ((impl_->clearFlushes % 512u) == 0u) {
    HMRDP_LOGI("gfx clear-traffic: flushes=%{public}llu cmds=%{public}llu map=%{public}llu ms "
               "decode=%{public}llu ms mapped=%{public}llu MB usedPct=%{public}llu",
               static_cast<unsigned long long>(impl_->clearFlushes),
               static_cast<unsigned long long>(impl_->clearCmds),
               static_cast<unsigned long long>(impl_->clearMapUs / 1000),
               static_cast<unsigned long long>(impl_->clearDecodeUs / 1000),
               static_cast<unsigned long long>(impl_->clearMapBytes / (1024 * 1024)),
               static_cast<unsigned long long>(
                   impl_->clearSpanPixels > 0
                        ? impl_->clearUnionPixels * 100u / impl_->clearSpanPixels
                        : 0u));
  }
  list.clear();
  impl_->pendingBytes = 0;
}

bool GfxGpuDesktop::DecodeMessage(uint16_t surfaceId, const uint8_t* payload, size_t size) {
  if (!ready_ || impl_ == nullptr || payload == nullptr || size == 0) {
    return false;
  }
  Impl::SurfaceGpu* const surface = impl_->Find(surfaceId);
  if (surface == nullptr) {
    return false;
  }
  if (!impl_->MakeCurrent()) {
    return false;
  }
  const int gridW = surface->meta.gridW;
  const int gridH = surface->meta.gridH;
  const int surfaceW = surface->meta.width;
  const int surfaceH = surface->meta.height;

  // Parse the container on the CPU (cheap) and resolve each tile component into
  // a GPU stream job: kFirst (absolute) and kUpgrade (SRL/raw refinement).
  struct TileJob {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t rectOffset = 0;  // index (rects) into rectPool
    uint32_t rectCount = 0;   // 0 = whole tile
    StreamJob streams[3];
    bool valid = false;
  };
  std::vector<TileJob> tiles;
  tiles.reserve(256);
  std::vector<uint32_t> rectPool;  // 2 words per rect: [x|y<<16, w|h<<16]
  rectPool.reserve(256);
  RfxParseStats stats;
  ParseRfxProgressive(
      payload, size,
      [&](const RfxTileRef& t) {
        if (t.quants == nullptr) {
          return;
        }
        const bool upgrade = (t.type == RfxTileType::kUpgrade);
        if (!upgrade && t.type != RfxTileType::kFirst) {
          return;  // kSimple not used by this server
        }
        if (t.xIdx >= static_cast<uint32_t>(gridW) ||
            t.yIdx >= static_cast<uint32_t>(gridH)) {
          return;  // tile outside this surface's grid
        }
        TileJob job;
        job.x = t.xIdx;
        job.y = t.yIdx;
        job.valid = true;
        job.rectOffset = static_cast<uint32_t>(rectPool.size() / 2);
        job.rectCount = t.numRects;
        for (uint16_t ri = 0; ri < t.numRects; ++ri) {
          const RfxRect& r = t.rects[ri];
          rectPool.push_back(static_cast<uint32_t>(r.x) | (static_cast<uint32_t>(r.y) << 16));
          rectPool.push_back(static_cast<uint32_t>(r.width) |
                             (static_cast<uint32_t>(r.height) << 16));
        }
        const RfxQuant* qv[3] = {&t.quants[t.quantIdxY], &t.quants[t.quantIdxCb],
                                 &t.quants[t.quantIdxCr]};
        RfxQuant prog[3];
        if (t.quality != 0xFF && t.progQuants != nullptr && t.quality < t.numProgQuant) {
          prog[0] = t.progQuants[t.quality].y;
          prog[1] = t.progQuants[t.quality].cb;
          prog[2] = t.progQuants[t.quality].cr;
        }
        const uint8_t* data[3] = {t.yData, t.cbData, t.crData};
        const uint16_t len[3] = {t.yLen, t.cbLen, t.crLen};
        const uint8_t* srl[3] = {t.ySrlData, t.cbSrlData, t.crSrlData};
        const uint16_t srlLen[3] = {t.ySrlLen, t.cbSrlLen, t.crSrlLen};
        const uint8_t* raw[3] = {t.yRawData, t.cbRawData, t.crRawData};
        const uint16_t rawLen[3] = {t.yRawLen, t.cbRawLen, t.crRawLen};
        const uint32_t tileIndex = static_cast<uint32_t>(t.yIdx) * static_cast<uint32_t>(gridW) +
                                   t.xIdx;
        for (int c = 0; c < 3; ++c) {
          uint8_t qa[10];
          uint8_t pa[10];
          QuantArray(*qv[c], qa);
          QuantArray(prog[c], pa);
          StreamJob& sj = job.streams[c];
          sj.type = upgrade ? 2u : 0u;
          sj.flags = t.flags & 1u;
          sj.tileStream = tileIndex * 3u + static_cast<uint32_t>(c);
          for (int i = 0; i < 10; ++i) {
            const int nb = static_cast<int>(qa[i]) + static_cast<int>(pa[i]);
            sj.newBit[i] = static_cast<uint8_t>(nb);
            const int sh = nb - 1;
            sj.shift[i] = static_cast<uint8_t>(sh < 0 ? 0 : sh);
          }
          if (upgrade) {
            sj.srlOff = static_cast<uint32_t>(srl[c] - payload);
            sj.srlLen = srlLen[c];
            sj.rawOff = static_cast<uint32_t>(raw[c] - payload);
            sj.rawLen = rawLen[c];
          } else {
            sj.payloadOff = static_cast<uint32_t>(data[c] - payload);
            sj.payloadLen = len[c];
          }
        }
        tiles.push_back(job);
      },
      &stats);
  if (tiles.empty()) {
    return true;
  }

  // Rotate to a buffer the GPU is not reading, then write it directly (no orphan
  // reallocation, no implicit sync).
  const GLuint payloadBuf = impl_->payloadRing.Next();
  if (size > impl_->payloadRing.Cap()) {
    impl_->payloadRing.Cap() = size + (size >> 1);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, payloadBuf);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(impl_->payloadRing.Cap()), nullptr, GL_DYNAMIC_DRAW);
  }
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, payloadBuf);
  glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(size), payload);

  GLuint composeRectBuf = 0;
  if (!rectPool.empty()) {
    composeRectBuf = impl_->composeRectRing.Next();
    const size_t needBytes = rectPool.size() * sizeof(uint32_t);
    if (needBytes > impl_->composeRectRing.Cap()) {
      impl_->composeRectRing.Cap() = needBytes + (needBytes >> 1) + 256;
      glBindBuffer(GL_SHADER_STORAGE_BUFFER, composeRectBuf);
      glBufferData(GL_SHADER_STORAGE_BUFFER,
                   static_cast<GLsizeiptr>(impl_->composeRectRing.Cap()), nullptr, GL_DYNAMIC_DRAW);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, composeRectBuf);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(needBytes),
                    rectPool.data());
  }

  const uint32_t chunkTiles = kChunkTiles;
  for (size_t start = 0; start < tiles.size(); start += chunkTiles) {
    const uint32_t count =
        static_cast<uint32_t>(std::min(chunkTiles, static_cast<uint32_t>(tiles.size() - start)));
    const uint32_t streams = count * 3;

    std::vector<uint8_t>& meta = impl_->metaScratch;
    std::vector<uint32_t>& tileMeta = impl_->tileMetaScratch;
    meta.assign(static_cast<size_t>(streams) * kMetaStride, 0);
    tileMeta.assign(static_cast<size_t>(count) * 4, 0);
    for (uint32_t t = 0; t < count; ++t) {
      const TileJob& job = tiles[start + t];
      tileMeta[t * 4] = job.x * 64;  // pixel origin of the tile
      tileMeta[t * 4 + 1] = job.y * 64;
      tileMeta[t * 4 + 2] = job.rectOffset;
      tileMeta[t * 4 + 3] = job.rectCount;
      // Mark the whole tile dirty (mirrors the CPU ApplyProgressive marking).
      Impl::MarkSurfaceDirty(*surface, static_cast<int>(job.x * 64),
                             static_cast<int>(job.y * 64), static_cast<int>(job.x * 64 + 64),
                             static_cast<int>(job.y * 64 + 64));
      for (int c = 0; c < 3; ++c) {
        uint8_t* rec = &meta[(static_cast<size_t>(t) * 3 + c) * kMetaStride];
        const StreamJob& sj = job.streams[c];
        rec[0] = static_cast<uint8_t>(sj.type);
        rec[1] = static_cast<uint8_t>(sj.flags);
        std::memcpy(rec + 4, &sj.tileStream, 4);
        std::memcpy(rec + 8, &sj.payloadOff, 4);
        std::memcpy(rec + 12, &sj.payloadLen, 4);
        std::memcpy(rec + 16, &sj.srlOff, 4);
        std::memcpy(rec + 20, &sj.srlLen, 4);
        std::memcpy(rec + 24, &sj.rawOff, 4);
        std::memcpy(rec + 28, &sj.rawLen, 4);
        std::memcpy(rec + 32, sj.shift, 10);
        std::memcpy(rec + 42, sj.newBit, 10);
      }
    }
    // Rotate to fresh slots: the previous chunk's dispatches may still be reading
    // the current ones.
    const GLuint metaBuf = impl_->metaRing.Next();
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, metaBuf);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(meta.size()), meta.data());
    const GLuint tileMetaBuf = impl_->tileMetaRing.Next();
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, tileMetaBuf);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                    static_cast<GLsizeiptr>(tileMeta.size() * 4), tileMeta.data());

    // decode: comp at word 0, temp at word chunkStreams*4096/2.
    const uint32_t compBase = 0;
    const uint32_t tempBase = streams * 4096;
    glUseProgram(impl_->decodeProg);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, payloadBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, metaBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, impl_->coefBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, surface->stateCurBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, surface->stateSignBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, surface->stateBpBuf);
    glUniform1ui(impl_->decodeNumStreams, streams);
    glUniform1ui(impl_->decodeCompBase, compBase);
    glUniform1ui(impl_->decodeTempBase, tempBase);
    glDispatchCompute((streams + 63) / 64, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    glUseProgram(impl_->composeProg);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, tileMetaBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, impl_->coefBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, surface->outBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, composeRectBuf);
    glUniform1ui(impl_->composeNumTiles, count);
    glUniform1ui(impl_->composeCompBase, compBase);
    glUniform1i(impl_->composeSurfaceW, surfaceW);
    glUniform1i(impl_->composeSurfaceH, surfaceH);
    glUniform1i(impl_->composeKr, kKr);
    glUniform1i(impl_->composeKcrG, kKcrG);
    glUniform1i(impl_->composeKcbG, kKcbG);
    glUniform1i(impl_->composeKcbB, kKcbB);
    glDispatchCompute((count + 63) / 64, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
  }
  // No glFinish here: it used to drain the whole GPU pipeline on every
  // Progressive message (the engine is fed once per GFX command), which
  // serialised CPU and GPU and dominated replay time. The SSBO barrier above
  // orders the compute writes for the following Compose/dispatch, and the few
  // paths that read back to the CPU (ReadScreen/ReadSurface and the ClearCodec
  // band map) issue their own barrier + finish right before mapping.
  return CheckGl("decode message");
}

bool GfxGpuDesktop::ReadSurface(uint16_t surfaceId, std::vector<uint8_t>* out) {
  if (!ready_ || impl_ == nullptr || out == nullptr) {
    return false;
  }
  const Impl::SurfaceGpu* const surface = impl_->Find(surfaceId);
  if (surface == nullptr) {
    return false;
  }
  if (!impl_->MakeCurrent()) {
    return false;
  }
  const GLsizeiptr bytes = static_cast<GLsizeiptr>(surface->outWords * 4);
  out->assign(surface->outWords * 4, 0);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, surface->outBuf);
  // GLES has no glGetBufferSubData; map the buffer for reading instead. The
  // barrier makes prior shader writes visible to the buffer-update path and the
  // finish guarantees they have completed (DecodeMessage no longer drains the
  // pipeline on every message).
  glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
  glFinish();
  void* mapped = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, bytes, GL_MAP_READ_BIT);
  if (mapped == nullptr) {
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    return CheckGl("map surface");
  }
  std::memcpy(out->data(), mapped, static_cast<size_t>(bytes));
  glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  return CheckGl("read surface");
}

// ---------------------------------------------------------------------------
// B2 surface commands (GPU)
// ---------------------------------------------------------------------------

namespace {

inline void DispatchPixels(size_t pixels) {
  // One invocation per pixel (64 per group). A full desktop needs > 65535 groups,
  // which is the common per-axis limit, so spread the groups over a 2D grid; the
  // kernels recover the linear index from gl_NumWorkGroups/gl_WorkGroupSize.
  const uint32_t groups = static_cast<uint32_t>((pixels + 63) / 64);
  const uint32_t gx = groups < 65535u ? (groups > 0u ? groups : 1u) : 65535u;
  const uint32_t gy = (groups + gx - 1) / gx;
  glDispatchCompute(gx, gy > 0u ? gy : 1u, 1);
}

// Clips [x,x+w) x [y,y+h) to [0,limitW) x [0,limitH); false when empty.
bool ClipRectGpu(int* x, int* y, int* w, int* h, int limitW, int limitH) {
  if (*x < 0) {
    *w += *x;
    *x = 0;
  }
  if (*y < 0) {
    *h += *y;
    *y = 0;
  }
  if (*x + *w > limitW) {
    *w = limitW - *x;
  }
  if (*y + *h > limitH) {
    *h = limitH - *y;
  }
  return *w > 0 && *h > 0;
}

}  // namespace

bool GfxGpuDesktop::SolidFill(uint16_t surfaceId, uint32_t bgraPixel, const uint16_t* rects,
                              uint32_t rectCount) {
  if (!ready_ || impl_ == nullptr || rects == nullptr || rectCount == 0) {
    return false;
  }
  Impl::SurfaceGpu* const surface = impl_->Find(surfaceId);
  if (surface == nullptr) {
    return false;
  }
  if (!impl_->MakeCurrent()) {
    return false;
  }
  const int surfaceW = surface->meta.width;
  const int surfaceH = surface->meta.height;
  glUseProgram(impl_->fillProg);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, surface->outBuf);
  glUniform1ui(impl_->fillStride, static_cast<GLuint>(surface->meta.stride));
  glUniform1ui(impl_->fillColor, bgraPixel);
  for (uint32_t i = 0; i < rectCount; ++i) {
    int left = rects[i * 4 + 0];
    int top = rects[i * 4 + 1];
    int right = rects[i * 4 + 2];
    int bottom = rects[i * 4 + 3];
    if (right > surfaceW) {
      right = surfaceW;
    }
    if (bottom > surfaceH) {
      bottom = surfaceH;
    }
    if (left < 0) {
      left = 0;
    }
    if (top < 0) {
      top = 0;
    }
    if (right <= left || bottom <= top) {
      continue;
    }
    glUniform1i(impl_->fillLeft, left);
    glUniform1i(impl_->fillTop, top);
    glUniform1i(impl_->fillWidth, right - left);
    glUniform1i(impl_->fillHeight, bottom - top);
    DispatchPixels(static_cast<size_t>(right - left) * static_cast<size_t>(bottom - top));
    Impl::MarkSurfaceDirty(*surface, left, top, right, bottom);
  }
  // One barrier for the whole command: the rects are disjoint writes into the
  // same buffer and nothing reads it in between, so a per-rect barrier was
  // needless (a fill PDU can carry hundreds of rects).
  glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
  return CheckGl("solid fill");
}

bool GfxGpuDesktop::UploadBgra(uint16_t surfaceId, int left, int top, int width, int height,
                               const uint8_t* bgra, int srcStride) {
  if (!ready_ || impl_ == nullptr || bgra == nullptr || width <= 0 || height <= 0) {
    return false;
  }
  const Impl::SurfaceGpu* const surface = impl_->Find(surfaceId);
  if (surface == nullptr) {
    return false;
  }
  const int surfaceW = surface->meta.width;
  const int surfaceH = surface->meta.height;
  const size_t pitch = static_cast<size_t>(surface->meta.stride);
  int sx = left < 0 ? 0 : left;
  int sy = top < 0 ? 0 : top;
  int ex = left + width;
  int ey = top + height;
  if (ex > surfaceW) {
    ex = surfaceW;
  }
  if (ey > surfaceH) {
    ey = surfaceH;
  }
  if (ex <= sx || ey <= sy) {
    return true;
  }
  if (!impl_->MakeCurrent()) {
    return false;
  }
  const int srcCol = sx - left;
  const int srcRow0 = sy - top;
  const int rows = ey - sy;
  const int cols = ex - sx;
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, surface->outBuf);
  // Map the whole touched range once and write the rows into it. Doing a
  // glBufferSubData per row is orders of magnitude slower (hundreds of GL calls
  // per ClearCodec command).
  const size_t offset0 = static_cast<size_t>(sy) * pitch + static_cast<size_t>(sx) * 4;
  const size_t offsetEnd = static_cast<size_t>(ey - 1) * pitch + static_cast<size_t>(ex) * 4;
  void* mapped = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, static_cast<GLintptr>(offset0),
                                  static_cast<GLsizeiptr>(offsetEnd - offset0),
                                  GL_MAP_WRITE_BIT);
  if (mapped == nullptr) {
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    return CheckGl("upload bgra map");
  }
  uint8_t* base = static_cast<uint8_t*>(mapped) - offset0;
  for (int row = 0; row < rows; ++row) {
    const uint8_t* srcRow = bgra + static_cast<size_t>(srcRow0 + row) * srcStride +
                            static_cast<size_t>(srcCol) * 4;
    std::memcpy(base + static_cast<size_t>(sy + row) * pitch + static_cast<size_t>(sx) * 4, srcRow,
                static_cast<size_t>(cols) * 4);
  }
  glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  return CheckGl("upload bgra");
}

bool GfxGpuDesktop::DownloadRows(uint16_t surfaceId, int top, int height, uint8_t* dst,
                                 int dstStride) {
  if (!ready_ || impl_ == nullptr || dst == nullptr || height <= 0) {
    return false;
  }
  const Impl::SurfaceGpu* const surface = impl_->Find(surfaceId);
  if (surface == nullptr) {
    return false;
  }
  const size_t pitch = static_cast<size_t>(surface->meta.stride);
  int y0 = top < 0 ? 0 : top;
  int y1 = top + height;
  if (y1 > surface->meta.height) {
    y1 = surface->meta.height;
  }
  if (y1 <= y0) {
    return true;
  }
  if (!impl_->MakeCurrent()) {
    return false;
  }
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, surface->outBuf);
  const size_t offset = static_cast<size_t>(y0) * pitch;
  // Prior GPU writes (shaders) must be complete and visible before the CPU
  // reads them here; this is the only place the ClearCodec path needs a stall.
  glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
  glFinish();
  void* mapped = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, static_cast<GLintptr>(offset),
                                  static_cast<GLsizeiptr>((y1 - y0)) * pitch, GL_MAP_READ_BIT);
  if (mapped == nullptr) {
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    return CheckGl("download rows map");
  }
  const uint8_t* src = static_cast<const uint8_t*>(mapped);
  for (int row = 0; row < y1 - y0; ++row) {
    std::memcpy(dst + static_cast<size_t>(row) * dstStride, src + static_cast<size_t>(row) * pitch,
                pitch);
  }
  glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  return CheckGl("download rows");
}

bool GfxGpuDesktop::UploadRows(uint16_t surfaceId, int top, int height, const uint8_t* src,
                               int srcStride) {
  if (!ready_ || impl_ == nullptr || src == nullptr || height <= 0) {
    return false;
  }
  const Impl::SurfaceGpu* const surface = impl_->Find(surfaceId);
  if (surface == nullptr) {
    return false;
  }
  const size_t pitch = static_cast<size_t>(surface->meta.stride);
  int y0 = top < 0 ? 0 : top;
  int y1 = top + height;
  if (y1 > surface->meta.height) {
    y1 = surface->meta.height;
  }
  if (y1 <= y0) {
    return true;
  }
  if (!impl_->MakeCurrent()) {
    return false;
  }
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, surface->outBuf);
  const size_t offset = static_cast<size_t>(y0) * pitch;
  void* mapped = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, static_cast<GLintptr>(offset),
                                  static_cast<GLsizeiptr>((y1 - y0)) * pitch, GL_MAP_WRITE_BIT);
  if (mapped == nullptr) {
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    return CheckGl("upload rows map");
  }
  uint8_t* dst = static_cast<uint8_t*>(mapped);
  for (int row = 0; row < y1 - y0; ++row) {
    std::memcpy(dst + static_cast<size_t>(row) * pitch,
                src + static_cast<size_t>(y0 - top + row) * srcStride, pitch);
  }
  glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  return CheckGl("upload rows");
}

bool GfxGpuDesktop::SurfaceToCache(uint16_t surfaceId, uint16_t slot, int x, int y, int width,
                                   int height) {
  if (!ready_ || impl_ == nullptr || width <= 0 || height <= 0) {
    return false;
  }
  const Impl::SurfaceGpu* const surface = impl_->Find(surfaceId);
  if (surface == nullptr) {
    return false;
  }
  if (x < 0 || y < 0 || x + width > surface->meta.width || y + height > surface->meta.height) {
    return false;
  }
  if (!impl_->MakeCurrent()) {
    return false;
  }
  Impl::CacheBuf& entry = impl_->cache[slot];
  entry.width = width;
  entry.height = height;
  entry.stride = ((width * 4 + 15) / 16) * 16;
  const size_t bytes = static_cast<size_t>(entry.stride) * height;
  if (entry.buf == 0) {
    glGenBuffers(1, &entry.buf);
  }
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, entry.buf);
  glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(bytes), nullptr, GL_DYNAMIC_DRAW);
  glUseProgram(impl_->copyProg);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, surface->outBuf);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, entry.buf);
  glUniform1ui(impl_->copySrcStride, static_cast<GLuint>(surface->meta.stride));
  glUniform1ui(impl_->copyDstStride, static_cast<GLuint>(entry.stride));
  glUniform1i(impl_->copySrcX, x);
  glUniform1i(impl_->copySrcY, y);
  glUniform1i(impl_->copyDstX, 0);
  glUniform1i(impl_->copyDstY, 0);
  glUniform1i(impl_->copyWidth, width);
  glUniform1i(impl_->copyHeight, height);
  DispatchPixels(static_cast<size_t>(width) * height);
  glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
  return CheckGl("surface to cache");
}

bool GfxGpuDesktop::CacheToSurface(uint16_t surfaceId, uint16_t slot, int dstX, int dstY) {
  if (!ready_ || impl_ == nullptr) {
    return false;
  }
  Impl::SurfaceGpu* const surface = impl_->Find(surfaceId);
  if (surface == nullptr) {
    return false;
  }
  const auto it = impl_->cache.find(slot);
  if (it == impl_->cache.end()) {
    return false;
  }
  const Impl::CacheBuf& entry = it->second;
  int dx = dstX;
  int dy = dstY;
  int w = entry.width;
  int h = entry.height;
  if (!ClipRectGpu(&dx, &dy, &w, &h, surface->meta.width, surface->meta.height)) {
    return true;  // fully outside the surface
  }
  const int srcX = dx - dstX;
  const int srcY = dy - dstY;
  if (!impl_->MakeCurrent()) {
    return false;
  }
  glUseProgram(impl_->copyProg);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, entry.buf);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, surface->outBuf);
  glUniform1ui(impl_->copySrcStride, static_cast<GLuint>(entry.stride));
  glUniform1ui(impl_->copyDstStride, static_cast<GLuint>(surface->meta.stride));
  glUniform1i(impl_->copySrcX, srcX);
  glUniform1i(impl_->copySrcY, srcY);
  glUniform1i(impl_->copyDstX, dx);
  glUniform1i(impl_->copyDstY, dy);
  glUniform1i(impl_->copyWidth, w);
  glUniform1i(impl_->copyHeight, h);
  DispatchPixels(static_cast<size_t>(w) * h);
  glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
  Impl::MarkSurfaceDirty(*surface, dx, dy, dx + w, dy + h);
  return CheckGl("cache to surface");
}

void GfxGpuDesktop::EvictCache(uint16_t slot) {
  if (impl_ == nullptr) {
    return;
  }
  const auto it = impl_->cache.find(slot);
  if (it == impl_->cache.end()) {
    return;
  }
  if (impl_->context != EGL_NO_CONTEXT) {
    impl_->MakeCurrent();
    if (it->second.buf != 0) {
      glDeleteBuffers(1, &it->second.buf);
    }
  }
  impl_->cache.erase(it);
}

bool GfxGpuDesktop::SurfaceToSurface(uint16_t srcSurfaceId, int srcX, int srcY, int width,
                                     int height, uint16_t dstSurfaceId, int dstX, int dstY) {
  if (!ready_ || impl_ == nullptr || width <= 0 || height <= 0) {
    return false;
  }
  const Impl::SurfaceGpu* const src = impl_->Find(srcSurfaceId);
  Impl::SurfaceGpu* const dst = impl_->Find(dstSurfaceId);
  if (src == nullptr || dst == nullptr) {
    return false;
  }
  if (!impl_->MakeCurrent()) {
    return false;
  }
  // Stage through a temporary buffer so overlapping same-surface copies are
  // safe (the GPU dispatch has no per-row ordering).
  const size_t words = static_cast<size_t>(width) * height;
  if (words > impl_->tempWords) {
    if (impl_->tempBuf != 0) {
      glDeleteBuffers(1, &impl_->tempBuf);
    }
    glGenBuffers(1, &impl_->tempBuf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, impl_->tempBuf);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(words * 4), nullptr,
                 GL_DYNAMIC_DRAW);
    impl_->tempWords = words;
  }
  int dx = dstX;
  int dy = dstY;
  int w = width;
  int h = height;
  if (!ClipRectGpu(&dx, &dy, &w, &h, dst->meta.width, dst->meta.height)) {
    return true;
  }
  const int sx = srcX + (dx - dstX);
  const int sy = srcY + (dy - dstY);
  const int srcW = src->meta.width;
  const int srcH = src->meta.height;
  if (sx < 0 || sy < 0 || sx + w > srcW || sy + h > srcH) {
    return false;
  }
  glUseProgram(impl_->copyProg);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, src->outBuf);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, impl_->tempBuf);
  glUniform1ui(impl_->copySrcStride, static_cast<GLuint>(src->meta.stride));
  glUniform1ui(impl_->copyDstStride, static_cast<GLuint>(w * 4));
  glUniform1i(impl_->copySrcX, sx);
  glUniform1i(impl_->copySrcY, sy);
  glUniform1i(impl_->copyDstX, 0);
  glUniform1i(impl_->copyDstY, 0);
  glUniform1i(impl_->copyWidth, w);
  glUniform1i(impl_->copyHeight, h);
  DispatchPixels(static_cast<size_t>(w) * h);
  glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, impl_->tempBuf);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, dst->outBuf);
  glUniform1ui(impl_->copySrcStride, static_cast<GLuint>(w * 4));
  glUniform1ui(impl_->copyDstStride, static_cast<GLuint>(dst->meta.stride));
  glUniform1i(impl_->copySrcX, 0);
  glUniform1i(impl_->copySrcY, 0);
  glUniform1i(impl_->copyDstX, dx);
  glUniform1i(impl_->copyDstY, dy);
  glUniform1i(impl_->copyWidth, w);
  glUniform1i(impl_->copyHeight, h);
  DispatchPixels(static_cast<size_t>(w) * h);
  glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
  Impl::MarkSurfaceDirty(*dst, dx, dy, dx + w, dy + h);
  return CheckGl("surface to surface");
}

// ---------------------------------------------------------------------------
// Screen compose (W4)
// ---------------------------------------------------------------------------

bool GfxGpuDesktop::ResetGraphics(int width, int height) {
  if (!ready_ || impl_ == nullptr) {
    return false;
  }
  FlushPendingClears();
  if (width <= 0 || height <= 0) {
    if (impl_->screenBuf != 0 || impl_->screenTex != 0) {
      impl_->MakeCurrent();
      if (impl_->screenBuf != 0) glDeleteBuffers(1, &impl_->screenBuf);
      if (impl_->screenTex != 0) glDeleteTextures(1, &impl_->screenTex);
    }
    impl_->screenBuf = 0;
    impl_->screenTex = 0;
    impl_->screenWords = 0;
    impl_->screenW = 0;
    impl_->screenH = 0;
    impl_->screenDirtyValid = false;
    screenW_ = 0;
    screenH_ = 0;
    return true;
  }
  if (!impl_->MakeCurrent()) {
    return false;
  }
  if (impl_->screenBuf == 0) {
    glGenBuffers(1, &impl_->screenBuf);
  }
  impl_->screenW = width;
  impl_->screenH = height;
  impl_->screenWords = static_cast<size_t>(width) * height;
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, impl_->screenBuf);
  glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(impl_->screenWords * 4), nullptr,
               GL_DYNAMIC_DRAW);
  void* p = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
                             static_cast<GLsizeiptr>(impl_->screenWords * 4),
                             GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
  if (p != nullptr) {
    std::memset(p, 0, impl_->screenWords * 4);
    glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
  }
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

  // Shared screen texture the Renderer samples (same EGL share group).
  if (impl_->screenTex != 0) {
    glDeleteTextures(1, &impl_->screenTex);
  }
  glGenTextures(1, &impl_->screenTex);
  glBindTexture(GL_TEXTURE_2D, impl_->screenTex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  // Initialise the texture from the (zeroed) screen buffer so it mirrors it.
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, impl_->screenBuf);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, width);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);

  impl_->screenDirtyValid = false;
  screenW_ = width;
  screenH_ = height;
  return CheckGl("reset graphics");
}

std::string GfxGpuDesktop::TrafficStats() const {
  if (impl_ == nullptr || impl_->clearFlushes == 0) {
    return std::string();
  }
  const unsigned long long used =
      impl_->clearSpanPixels > 0 ? impl_->clearUnionPixels * 100ull / impl_->clearSpanPixels : 0ull;
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "clear: flush=%llu cmd=%llu map=%llums dec=%llums mapMB=%llu use=%llu%%",
                static_cast<unsigned long long>(impl_->clearFlushes),
                static_cast<unsigned long long>(impl_->clearCmds),
                static_cast<unsigned long long>(impl_->clearMapUs / 1000),
                static_cast<unsigned long long>(impl_->clearDecodeUs / 1000),
                static_cast<unsigned long long>(impl_->clearMapBytes / (1024 * 1024)), used);
  return std::string(buf);
}

uint64_t GfxGpuDesktop::ClearWorkUs() const {
  if (impl_ == nullptr) {
    return 0;
  }
  return impl_->clearMapUs + impl_->clearDecodeUs;
}

void GfxGpuDesktop::SetClearBatchAreaLimit(int pixels) {
  if (impl_ != nullptr) {
    impl_->clearBatchAreaLimit = pixels;
  }
}

int GfxGpuDesktop::clearBatchAreaLimit() const {
  return impl_ != nullptr ? impl_->clearBatchAreaLimit : 0;
}

void GfxGpuDesktop::ClearScreenDirty() {
  if (impl_ != nullptr) {
    impl_->screenDirtyValid = false;
  }
}

bool GfxGpuDesktop::screenDirty() const {
  return impl_ != nullptr && impl_->screenDirtyValid;
}

uint32_t GfxGpuDesktop::screenTexture() const {
  return impl_ != nullptr ? static_cast<uint32_t>(impl_->screenTex) : 0u;
}

bool GfxGpuDesktop::Compose() {
  if (!ready_ || impl_ == nullptr || impl_->screenBuf == 0) {
    return false;
  }
  // A frame boundary: make sure the queued ClearCodec run is on the surfaces
  // before they are composited.
  FlushPendingClears();
  if (!impl_->MakeCurrent()) {
    return false;
  }
  const int scrW = impl_->screenW;
  const int scrH = impl_->screenH;
  glUseProgram(impl_->copyProg);
  for (auto& kv : impl_->surfaces) {
    Impl::SurfaceGpu& s = kv.second;
    GpuSurface& m = s.meta;
    if (!m.mapped || !m.dirtyValid) {
      continue;
    }
    int left = m.dirtyLeft;
    int top = m.dirtyTop;
    int right = m.dirtyRight;
    int bottom = m.dirtyBottom;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > m.mappedWidth) right = m.mappedWidth;
    if (bottom > m.mappedHeight) bottom = m.mappedHeight;
    if (right <= left || bottom <= top) {
      m.dirtyValid = false;
      continue;
    }
    // 1:1 output mapping (scaled PDUs unmap the surface); a rect copy.
    int dstX = static_cast<int>(m.outputX) + left;
    int dstY = static_cast<int>(m.outputY) + top;
    if (dstX < 0) dstX = 0;
    if (dstY < 0) dstY = 0;
    if (dstX >= scrW || dstY >= scrH) {
      m.dirtyValid = false;
      continue;
    }
    int dstW = right - left;
    int dstH = bottom - top;
    if (dstW > scrW - dstX) dstW = scrW - dstX;
    if (dstH > scrH - dstY) dstH = scrH - dstY;
    if (dstW <= 0 || dstH <= 0) {
      m.dirtyValid = false;
      continue;
    }
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, s.outBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, impl_->screenBuf);
    glUniform1ui(impl_->copySrcStride, static_cast<GLuint>(m.stride));
    glUniform1ui(impl_->copyDstStride, static_cast<GLuint>(scrW * 4));
    glUniform1i(impl_->copySrcX, left);
    glUniform1i(impl_->copySrcY, top);
    glUniform1i(impl_->copyDstX, dstX);
    glUniform1i(impl_->copyDstY, dstY);
    glUniform1i(impl_->copyWidth, dstW);
    glUniform1i(impl_->copyHeight, dstH);
    DispatchPixels(static_cast<size_t>(dstW) * static_cast<size_t>(dstH));
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    impl_->MarkScreenDirty(dstX, dstY, dstX + dstW, dstY + dstH);
    m.dirtyValid = false;
  }

  // Mirror the dirty screen region into the shared texture. The buffer is bound
  // as a pixel-unpack source, so this is a GPU-to-GPU copy with no CPU readback
  // (the buffer may otherwise still be in flight from the compute dispatch, so a
  // buffer-update barrier is required first).
  if (impl_->screenDirtyValid && impl_->screenTex != 0) {
    const int l = impl_->screenDirtyL;
    const int t = impl_->screenDirtyT;
    const int w = impl_->screenDirtyR - l;
    const int h = impl_->screenDirtyB - t;
    if (w > 0 && h > 0) {
      glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, impl_->screenBuf);
      glBindTexture(GL_TEXTURE_2D, impl_->screenTex);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      glPixelStorei(GL_UNPACK_ROW_LENGTH, scrW);
      const size_t offset = (static_cast<size_t>(t) * scrW + static_cast<size_t>(l)) * 4;
      glTexSubImage2D(GL_TEXTURE_2D, 0, l, t, w, h, GL_RGBA, GL_UNSIGNED_BYTE,
                      reinterpret_cast<const void*>(offset));
      glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
      glBindTexture(GL_TEXTURE_2D, 0);
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }
  }
  return CheckGl("compose") && impl_->screenDirtyValid;
}

bool GfxGpuDesktop::ReadScreen(std::vector<uint8_t>* out) {
  if (!ready_ || impl_ == nullptr || out == nullptr || impl_->screenBuf == 0) {
    return false;
  }
  if (!impl_->MakeCurrent()) {
    return false;
  }
  const GLsizeiptr bytes = static_cast<GLsizeiptr>(impl_->screenWords * 4);
  out->assign(impl_->screenWords * 4, 0);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, impl_->screenBuf);
  // The compose dispatches wrote this buffer; make them visible/complete before
  // the CPU readback (only used by diagnostics / shadow compare).
  glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
  glFinish();
  void* mapped = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, bytes, GL_MAP_READ_BIT);
  if (mapped == nullptr) {
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    HMRDP_LOGE("gpu gfx desktop: read screen map failed (0x%{public}x)", glGetError());
    return false;
  }
  std::memcpy(out->data(), mapped, static_cast<size_t>(bytes));
  glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  return CheckGl("read screen");
}

// ---------------------------------------------------------------------------
// §1 Progressive container parser
// ---------------------------------------------------------------------------

namespace {

// Progressive block types (libfreerdp/codec/rfx_constants.h).
constexpr uint16_t kWbtSync = 0xCCC0;
constexpr uint16_t kWbtFrameBegin = 0xCCC1;
constexpr uint16_t kWbtFrameEnd = 0xCCC2;
constexpr uint16_t kWbtContext = 0xCCC3;
constexpr uint16_t kWbtRegion = 0xCCC4;
constexpr uint16_t kWbtTileSimple = 0xCCC5;
constexpr uint16_t kWbtTileFirst = 0xCCC6;
constexpr uint16_t kWbtTileUpgrade = 0xCCC7;

inline uint16_t ReadU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t ReadU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool ParseTile(const uint8_t* data, size_t size, RfxTileRef* tile, RfxParseStats* stats) {
  if (size < 6) {
    return false;
  }
  const uint16_t blockType = ReadU16(data);
  const uint32_t blockLen = ReadU32(data + 2);
  if (blockLen < 6 || static_cast<size_t>(blockLen) > size) {
    return false;
  }
  const uint8_t* p = data + 6;
  const size_t len = static_cast<size_t>(blockLen) - 6;

  if (blockType == kWbtTileFirst || blockType == kWbtTileSimple) {
    const bool simple = (blockType == kWbtTileSimple);
    const size_t headerLen = simple ? 16 : 17;
    if (len < headerLen) {
      return false;
    }
    tile->type = simple ? RfxTileType::kSimple : RfxTileType::kFirst;
    tile->quantIdxY = p[0];
    tile->quantIdxCb = p[1];
    tile->quantIdxCr = p[2];
    tile->xIdx = ReadU16(p + 3);
    tile->yIdx = ReadU16(p + 5);
    tile->flags = p[7];
    size_t h = 8;
    if (!simple) {
      tile->quality = p[h];
      h += 1;
    } else {
      tile->quality = 0xFF;
    }
    tile->yLen = ReadU16(p + h);
    tile->cbLen = ReadU16(p + h + 2);
    tile->crLen = ReadU16(p + h + 4);
    tile->tailLen = ReadU16(p + h + 6);
    h += 8;
    const size_t need = h + static_cast<size_t>(tile->yLen) + tile->cbLen + tile->crLen +
                        tile->tailLen;
    if (need > len) {
      return false;
    }
    tile->yData = p + h;
    h += tile->yLen;
    tile->cbData = p + h;
    h += tile->cbLen;
    tile->crData = p + h;
    h += tile->crLen;
    tile->tailData = p + h;
  } else if (blockType == kWbtTileUpgrade) {
    if (len < 20) {
      return false;
    }
    tile->type = RfxTileType::kUpgrade;
    tile->quantIdxY = p[0];
    tile->quantIdxCb = p[1];
    tile->quantIdxCr = p[2];
    tile->xIdx = ReadU16(p + 3);
    tile->yIdx = ReadU16(p + 5);
    tile->quality = p[7];
    tile->ySrlLen = ReadU16(p + 8);
    tile->yRawLen = ReadU16(p + 10);
    tile->cbSrlLen = ReadU16(p + 12);
    tile->cbRawLen = ReadU16(p + 14);
    tile->crSrlLen = ReadU16(p + 16);
    tile->crRawLen = ReadU16(p + 18);
    size_t h = 20;
    const size_t need = h + static_cast<size_t>(tile->ySrlLen) + tile->yRawLen + tile->cbSrlLen +
                        tile->cbRawLen + tile->crSrlLen + tile->crRawLen;
    if (need > len) {
      return false;
    }
    tile->ySrlData = p + h;
    h += tile->ySrlLen;
    tile->yRawData = p + h;
    h += tile->yRawLen;
    tile->cbSrlData = p + h;
    h += tile->cbSrlLen;
    tile->cbRawData = p + h;
    h += tile->cbRawLen;
    tile->crSrlData = p + h;
    h += tile->crSrlLen;
    tile->crRawData = p + h;
  } else {
    return false;
  }

  if (stats != nullptr) {
    stats->tiles++;
    if (tile->type == RfxTileType::kFirst) {
      stats->firstTiles++;
    } else if (tile->type == RfxTileType::kSimple) {
      stats->simpleTiles++;
    } else {
      stats->upgradeTiles++;
    }
    if (tile->xIdx > stats->maxTileX) {
      stats->maxTileX = tile->xIdx;
    }
    if (tile->yIdx > stats->maxTileY) {
      stats->maxTileY = tile->yIdx;
    }
    if ((tile->flags & 0x01u) != 0) {
      stats->diffTiles++;
    }
  }
  return true;
}

void ReadQuantNibbles(const uint8_t* b, RfxQuant* q) {
  q->LL3 = b[0] & 0x0F;
  q->HL3 = b[0] >> 4;
  q->LH3 = b[1] & 0x0F;
  q->HH3 = b[1] >> 4;
  q->HL2 = b[2] & 0x0F;
  q->LH2 = b[2] >> 4;
  q->HH2 = b[3] & 0x0F;
  q->HL1 = b[3] >> 4;
  q->LH1 = b[4] & 0x0F;
  q->HH1 = b[4] >> 4;
}

bool ParseRegion(const uint8_t* data, size_t size, const RfxTileCallback& onTile,
                 RfxParseStats* stats) {
  if (size < 12) {
    return false;
  }
  const uint8_t tileSize = data[0];
  const uint16_t numRects = ReadU16(data + 1);
  const uint8_t numQuant = data[3];
  const uint8_t numProgQuant = data[4];
  const uint8_t regionFlags = data[5];
  const uint16_t numTiles = ReadU16(data + 6);
  const uint32_t tileDataSize = ReadU32(data + 8);
  if (tileSize != 64 || numQuant > 7) {
    return false;
  }
  constexpr uint16_t kMaxRects = 1024;
  if (numRects > kMaxRects) {
    return false;
  }
  size_t p = 12;
  // rects (8 bytes each): x,y,width,height all little-endian u16.
  RfxRect rects[kMaxRects];
  if (p + static_cast<size_t>(numRects) * 8 > size) {
    return false;
  }
  for (uint16_t i = 0; i < numRects; ++i) {
    rects[i].x = ReadU16(data + p);
    rects[i].y = ReadU16(data + p + 2);
    rects[i].width = ReadU16(data + p + 4);
    rects[i].height = ReadU16(data + p + 6);
    p += 8;
  }
  // Component quant tables: 5 bytes -> 10 x 4-bit shifts each.
  RfxQuant quants[7];
  if (p + static_cast<size_t>(numQuant) * 5 > size) {
    return false;
  }
  for (uint8_t q = 0; q < numQuant; ++q) {
    ReadQuantNibbles(data + p, &quants[q]);
    p += 5;
  }
  // Progressive quant tables: 16 bytes each (quality + Y/Cb/Cr quant).
  RfxProgQuant progQuants[16];
  if (numProgQuant > 16 || p + static_cast<size_t>(numProgQuant) * 16 > size) {
    return false;
  }
  for (uint8_t q = 0; q < numProgQuant; ++q) {
    const uint8_t* b = data + p;
    progQuants[q].quality = b[0];
    ReadQuantNibbles(b + 1, &progQuants[q].y);
    ReadQuantNibbles(b + 6, &progQuants[q].cb);
    ReadQuantNibbles(b + 11, &progQuants[q].cr);
    p += 16;
  }
  if (p + tileDataSize > size) {
    return false;
  }
  const size_t tileEnd = p + tileDataSize;

  if (stats != nullptr) {
    stats->regions++;
    stats->rects += numRects;
    if ((regionFlags & 0x01u) != 0) {
      stats->extrapolateRegions++;
    }
  }

  uint32_t count = 0;
  while (p + 6 <= tileEnd && count < numTiles) {
    const uint32_t blockLen = ReadU32(data + p + 2);
    if (blockLen < 6 || p + blockLen > tileEnd) {
      return false;
    }
    RfxTileRef tile;
    if (!ParseTile(data + p, blockLen, &tile, stats)) {
      return false;
    }
    tile.quants = quants;
    tile.numQuant = numQuant;
    tile.progQuants = progQuants;
    tile.numProgQuant = numProgQuant;
    tile.regionFlags = regionFlags;
    tile.rects = rects;
    tile.numRects = numRects;
    if (onTile) {
      onTile(tile);
    }
    p += blockLen;
    count++;
  }
  return count == numTiles;
}

}  // namespace

bool ParseRfxProgressive(const uint8_t* data, size_t size, const RfxTileCallback& onTile,
                         RfxParseStats* stats) {
  if (data == nullptr || size < 6) {
    if (stats != nullptr) {
      stats->errors++;
    }
    return false;
  }
  if (stats != nullptr) {
    stats->messages++;
  }

  bool ok = true;
  size_t pos = 0;
  while (pos + 6 <= size) {
    const uint16_t blockType = ReadU16(data + pos);
    const uint32_t blockLen = ReadU32(data + pos + 2);
    if (blockLen < 6 || pos + blockLen > size) {
      ok = false;
      break;
    }
    const uint8_t* body = data + pos + 6;
    const size_t bodyLen = static_cast<size_t>(blockLen) - 6;
    switch (blockType) {
      case kWbtRegion:
        if (!ParseRegion(body, bodyLen, onTile, stats)) {
          ok = false;
        }
        break;
      case kWbtSync:
      case kWbtFrameBegin:
      case kWbtFrameEnd:
      case kWbtContext:
        break;
      default:
        ok = false;
        break;
    }
    if (!ok) {
      break;
    }
    pos += blockLen;
  }
  if (!ok && stats != nullptr) {
    stats->errors++;
  }
  return ok;
}

// ---------------------------------------------------------------------------
// ClearCodec glue: FreeRDP clear_decompress (the one codec not done in GLES)
// ---------------------------------------------------------------------------

namespace {

class FreeRdpClearDecoder : public GfxClearDecoder {
 public:
  FreeRdpClearDecoder() : clear_(clear_context_new(0 /* compressor = FALSE */)) {}
  ~FreeRdpClearDecoder() override {
    if (clear_ != nullptr) {
      clear_context_free(clear_);
      clear_ = nullptr;
    }
  }

  FreeRdpClearDecoder(const FreeRdpClearDecoder&) = delete;
  FreeRdpClearDecoder& operator=(const FreeRdpClearDecoder&) = delete;

  bool Decode(const uint8_t* src, size_t size, int width, int height, uint32_t dstFormat,
              uint8_t* dst, int stride, int xDst, int yDst, int dstW, int dstH) override {
    if (clear_ == nullptr || src == nullptr || dst == nullptr) {
      return false;
    }
    const INT32 rc =
        clear_decompress(clear_, src, static_cast<UINT32>(size), static_cast<UINT32>(width),
                         static_cast<UINT32>(height), dst, dstFormat, static_cast<UINT32>(stride),
                         static_cast<UINT32>(xDst), static_cast<UINT32>(yDst),
                         static_cast<UINT32>(dstW), static_cast<UINT32>(dstH), nullptr);
    return rc >= 0;
  }

  void Reset() override {
    if (clear_ != nullptr) {
      clear_context_reset(clear_);
    }
  }

 private:
  CLEAR_CONTEXT* clear_ = nullptr;
};

}  // namespace

std::unique_ptr<GfxClearDecoder> CreateFreeRdpClearDecoder() {
  return std::unique_ptr<GfxClearDecoder>(new FreeRdpClearDecoder());
}

// ---------------------------------------------------------------------------
// Shared command/present pipeline (session + replay harness)
// ---------------------------------------------------------------------------

bool GpuPresentComposed(GfxGpuDesktop* engine, Renderer* renderer) {
  if (engine == nullptr || renderer == nullptr) {
    return false;
  }
  if (!engine->Compose()) {
    return false;  // static frame: nothing dirty, no present (FPS stays 0)
  }
  const bool presented = renderer->PresentTexture(engine->screenTexture(), engine->screenWidth(),
                                                  engine->screenHeight());
  if (presented) {
    engine->ClearScreenDirty();
  }
  return presented;
}
}  // namespace hmrdp
