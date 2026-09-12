/*
 * HmRdp - GPU RemoteFX/Progressive decoder (see hmrdp_rfx_gpu.h).
 *
 * Covers the whole Progressive tile pipeline in GLES 3.1 compute: kFirst
 * (RLGR1 -> dequant -> extrapolate inverse DWT) and kUpgrade (SRL/raw
 * refinement), with the per-(tile,component) current/sign/bit-positions state
 * kept resident in GPU buffers across messages, plus YCbCr->BGRA composition.
 * It is a line-for-line mirror of the validated CPU reference in
 * hmrdp_rfx.cpp; the offline self-test replays a captured stream and compares
 * the GPU surface with the FreeRDP reference surface pixel by pixel.
 */
#include "hmrdp_rfx_gpu.h"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>

#include "hmrdp_log.h"

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
// Persistent per-(tile,component) progressive state (mirrors RfxTileState).
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

// ---- RLGR1 (port of RlgrDecode(RLGR1)) -----------------------------------
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
  uint px0 = tmeta.data[t * 2u];
  uint py0 = tmeta.data[t * 2u + 1u];
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
      surfPut(px0 + col, py0 + row, uint(b), uint(g), uint(r));
    }
  }
}
)GLSL";

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
  const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3,
                               EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
  EGLContext ctx = eglCreateContext(display, *outConfig, EGL_NO_CONTEXT, ctxAttribs);
  if (ctx != EGL_NO_CONTEXT) {
    return ctx;
  }
  const EGLint ctx3[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  return eglCreateContext(display, *outConfig, EGL_NO_CONTEXT, ctx3);
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
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || eglInitialize(display, nullptr, nullptr) != EGL_TRUE) {
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

struct RfxGpuDecoder::Impl {
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLSurface surface = EGL_NO_SURFACE;
  EGLContext context = EGL_NO_CONTEXT;
  bool current = false;

  GLuint decodeProg = 0;
  GLuint composeProg = 0;
  GLuint payloadBuf = 0;
  GLuint metaBuf = 0;
  GLuint coefBuf = 0;
  GLuint tileMetaBuf = 0;
  GLuint outBuf = 0;
  GLuint stateCurBuf = 0;
  GLuint stateSignBuf = 0;
  GLuint stateBpBuf = 0;

  GLuint decodeNumStreams = 0, decodeCompBase = 0, decodeTempBase = 0;
  GLuint composeNumTiles = 0, composeCompBase = 0;
  GLuint composeSurfaceW = 0, composeSurfaceH = 0;
  GLuint composeKr = 0, composeKcrG = 0, composeKcbG = 0, composeKcbB = 0;

  size_t coefWords = 0;
  size_t outWords = 0;
  size_t payloadCapacity = 0;

  bool MakeCurrent() {
    if (display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT) {
      return false;
    }
    if (eglMakeCurrent(display, surface, surface, context) != EGL_TRUE) {
      return false;
    }
    current = true;
    return true;
  }
};

RfxGpuDecoder::RfxGpuDecoder() : impl_(new Impl()) {}
RfxGpuDecoder::~RfxGpuDecoder() {
  Reset();
  delete impl_;
  impl_ = nullptr;
}

void RfxGpuDecoder::Reset() {
  if (impl_ == nullptr) {
    return;
  }
  if (impl_->context != EGL_NO_CONTEXT) {
    impl_->MakeCurrent();
    if (impl_->payloadBuf) glDeleteBuffers(1, &impl_->payloadBuf);
    if (impl_->metaBuf) glDeleteBuffers(1, &impl_->metaBuf);
    if (impl_->coefBuf) glDeleteBuffers(1, &impl_->coefBuf);
    if (impl_->tileMetaBuf) glDeleteBuffers(1, &impl_->tileMetaBuf);
    if (impl_->outBuf) glDeleteBuffers(1, &impl_->outBuf);
    if (impl_->stateCurBuf) glDeleteBuffers(1, &impl_->stateCurBuf);
    if (impl_->stateSignBuf) glDeleteBuffers(1, &impl_->stateSignBuf);
    if (impl_->stateBpBuf) glDeleteBuffers(1, &impl_->stateBpBuf);
    if (impl_->decodeProg) glDeleteProgram(impl_->decodeProg);
    if (impl_->composeProg) glDeleteProgram(impl_->composeProg);
    impl_->payloadBuf = impl_->metaBuf = impl_->coefBuf = impl_->tileMetaBuf = impl_->outBuf = 0;
    impl_->stateCurBuf = impl_->stateSignBuf = impl_->stateBpBuf = 0;
    impl_->decodeProg = impl_->composeProg = 0;
    eglMakeCurrent(impl_->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(impl_->display, impl_->context);
    eglDestroySurface(impl_->display, impl_->surface);
    impl_->context = EGL_NO_CONTEXT;
    impl_->surface = EGL_NO_SURFACE;
  }
  impl_->display = EGL_NO_DISPLAY;
  ready_ = false;
}

bool RfxGpuDecoder::Init(int gridW, int gridH, int surfaceW, int surfaceH) {
  if (impl_ == nullptr || gridW <= 0 || gridH <= 0 || surfaceW <= 0 || surfaceH <= 0) {
    return false;
  }
  Reset();
  gridW_ = gridW;
  gridH_ = gridH;
  surfaceW_ = surfaceW;
  surfaceH_ = surfaceH;

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
  if (impl_->decodeProg == 0 || impl_->composeProg == 0) {
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

  // Per-chunk scratch: comp then temp, each chunkStreams*4096 int16.
  const uint32_t chunkStreams = kChunkTiles * 3;
  impl_->coefWords = static_cast<size_t>(chunkStreams) * 4096 / 2 * 2;  // two regions
  glGenBuffers(1, &impl_->coefBuf);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, impl_->coefBuf);
  glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(impl_->coefWords * 4), nullptr,
               GL_DYNAMIC_DRAW);

  const uint32_t chunkTiles = kChunkTiles;
  glGenBuffers(1, &impl_->metaBuf);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, impl_->metaBuf);
  glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(chunkStreams * kMetaStride),
               nullptr, GL_DYNAMIC_DRAW);
  glGenBuffers(1, &impl_->tileMetaBuf);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, impl_->tileMetaBuf);
  glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(chunkTiles * 8), nullptr,
               GL_DYNAMIC_DRAW);

  glGenBuffers(1, &impl_->payloadBuf);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, impl_->payloadBuf);
  impl_->payloadCapacity = 1u << 20;  // grown on demand
  glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(impl_->payloadCapacity), nullptr,
               GL_DYNAMIC_DRAW);

  impl_->outWords = static_cast<size_t>(surfaceW) * surfaceH * 4 / 4;
  glGenBuffers(1, &impl_->outBuf);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, impl_->outBuf);
  glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(impl_->outWords * 4), nullptr,
               GL_DYNAMIC_DRAW);
  {
    void* p = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
                               static_cast<GLsizeiptr>(impl_->outWords * 4),
                               GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    if (p != nullptr) {
      std::memset(p, 0, impl_->outWords * 4);
      glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    }
  }

  // Persistent per-(tile,component) progressive state (current/sign int16 +
  // 10 nibble-ish bytes of bit positions), zero-initialised for the whole grid.
  const size_t gridStreams = static_cast<size_t>(gridW) * gridH * 3;
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
  makeZeroed(&impl_->stateCurBuf, stateCoefBytes);
  makeZeroed(&impl_->stateSignBuf, stateCoefBytes);
  makeZeroed(&impl_->stateBpBuf, stateBpBytes);

  if (!CheckGl("init")) {
    return false;
  }
  ready_ = true;
  HMRDP_LOGI("gpu rfx: decoder ready %{public}dx%{public}d grid %{public}dx%{public}d",
             surfaceW, surfaceH, gridW, gridH);
  return true;
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

bool RfxGpuDecoder::DecodeMessage(const uint8_t* payload, size_t size) {
  if (!ready_ || impl_ == nullptr || payload == nullptr || size == 0) {
    return false;
  }
  if (!impl_->MakeCurrent()) {
    return false;
  }

  // Parse the container on the CPU (cheap) and resolve each tile component into
  // a GPU stream job: kFirst (absolute) and kUpgrade (SRL/raw refinement).
  struct TileJob {
    uint32_t x = 0;
    uint32_t y = 0;
    StreamJob streams[3];
    bool valid = false;
  };
  std::vector<TileJob> tiles;
  tiles.reserve(256);
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
        TileJob job;
        job.x = t.xIdx;
        job.y = t.yIdx;
        job.valid = true;
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
        const uint32_t tileIndex = static_cast<uint32_t>(t.yIdx) * static_cast<uint32_t>(gridW_) +
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

  // Upload payload (grow the buffer if needed).
  if (size > impl_->payloadCapacity) {
    impl_->payloadCapacity = size + (size >> 1);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, impl_->payloadBuf);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(impl_->payloadCapacity),
                 nullptr, GL_DYNAMIC_DRAW);
  }
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, impl_->payloadBuf);
  glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(size), payload);

  const uint32_t chunkTiles = kChunkTiles;
  for (size_t start = 0; start < tiles.size(); start += chunkTiles) {
    const uint32_t count =
        static_cast<uint32_t>(std::min(chunkTiles, static_cast<uint32_t>(tiles.size() - start)));
    const uint32_t streams = count * 3;

    std::vector<uint8_t> meta(static_cast<size_t>(streams) * kMetaStride, 0);
    std::vector<uint32_t> tileMeta(static_cast<size_t>(count) * 2, 0);
    for (uint32_t t = 0; t < count; ++t) {
      const TileJob& job = tiles[start + t];
      tileMeta[t * 2] = job.x * 64;  // pixel origin of the tile
      tileMeta[t * 2 + 1] = job.y * 64;
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
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, impl_->metaBuf);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(meta.size()), meta.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, impl_->tileMetaBuf);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                    static_cast<GLsizeiptr>(tileMeta.size() * 4), tileMeta.data());

    // decode: comp at word 0, temp at word chunkStreams*4096/2.
    const uint32_t compBase = 0;
    const uint32_t tempBase = streams * 4096;
    glUseProgram(impl_->decodeProg);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, impl_->payloadBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, impl_->metaBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, impl_->coefBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, impl_->stateCurBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, impl_->stateSignBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, impl_->stateBpBuf);
    glUniform1ui(impl_->decodeNumStreams, streams);
    glUniform1ui(impl_->decodeCompBase, compBase);
    glUniform1ui(impl_->decodeTempBase, tempBase);
    glDispatchCompute((streams + 63) / 64, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    glUseProgram(impl_->composeProg);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, impl_->tileMetaBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, impl_->coefBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, impl_->outBuf);
    glUniform1ui(impl_->composeNumTiles, count);
    glUniform1ui(impl_->composeCompBase, compBase);
    glUniform1i(impl_->composeSurfaceW, surfaceW_);
    glUniform1i(impl_->composeSurfaceH, surfaceH_);
    glUniform1i(impl_->composeKr, kKr);
    glUniform1i(impl_->composeKcrG, kKcrG);
    glUniform1i(impl_->composeKcbG, kKcbG);
    glUniform1i(impl_->composeKcbB, kKcbB);
    glDispatchCompute((count + 63) / 64, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
  }
  glFinish();
  return CheckGl("decode message");
}

bool RfxGpuDecoder::ReadSurface(std::vector<uint8_t>* out) {
  if (!ready_ || impl_ == nullptr || out == nullptr) {
    return false;
  }
  if (!impl_->MakeCurrent()) {
    return false;
  }
  const GLsizeiptr bytes = static_cast<GLsizeiptr>(impl_->outWords * 4);
  out->assign(impl_->outWords * 4, 0);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, impl_->outBuf);
  // GLES has no glGetBufferSubData; map the buffer for reading instead.
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
// Offline self-test
// ---------------------------------------------------------------------------

namespace {

std::vector<uint8_t> ReadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return {};
  }
  f.seekg(0, std::ios::end);
  const std::streamoff n = f.tellg();
  f.seekg(0, std::ios::beg);
  std::vector<uint8_t> b(static_cast<size_t>(n));
  f.read(reinterpret_cast<char*>(b.data()), n);
  return b;
}

uint32_t Rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

struct RefSurface {
  uint32_t w = 0;
  uint32_t h = 0;
  uint32_t stride = 0;
  std::vector<uint8_t> data;
};

}  // namespace

RfxGpuSelfTestResult RunRfxGpuSelfTest(const std::string& rfxPath,
                                       const std::string& surfacePath) {
  RfxGpuSelfTestResult res;
  // Dev convenience: make sure the sample slots exist and are world-writable so
  // `hdc file send` can overwrite them (the shell user cannot create files in
  // the app sandbox). Empty placeholders make the self-test return immediately.
  for (const std::string* p : {&rfxPath, &surfacePath}) {
    if (p->empty()) {
      continue;
    }
    FILE* f = std::fopen(p->c_str(), "ab");
    if (f != nullptr) {
      std::fclose(f);
    }
    ::chmod(p->c_str(), 0666);
  }
  const std::vector<uint8_t> cb = ReadFile(rfxPath);
  const std::vector<uint8_t> sb = ReadFile(surfacePath);
  if (cb.empty() || sb.empty()) {
    res.log = "cannot read capture files";
    return res;
  }

  std::vector<std::pair<size_t, size_t>> streams;  // (offset, length)
  size_t pos = 0;
  while (pos + 32 <= cb.size() && Rd32(&cb[pos]) == 0x31584652u) {
    const uint32_t len = Rd32(&cb[pos + 28]);
    if (pos + 32 + len > cb.size()) break;
    streams.emplace_back(pos + 32, len);
    pos += 32 + len;
  }
  std::map<uint32_t, RefSurface> surfaces;
  pos = 0;
  while (pos + 32 <= sb.size() && Rd32(&sb[pos]) == 0x31534653u) {
    RefSurface s;
    const uint32_t idx = Rd32(&sb[pos + 4]);
    s.w = Rd32(&sb[pos + 8]);
    s.h = Rd32(&sb[pos + 12]);
    s.stride = Rd32(&sb[pos + 16]);
    const size_t bytes = static_cast<size_t>(s.stride) * s.h;
    if (pos + 32 + bytes > sb.size()) break;
    s.data.assign(sb.begin() + pos + 32, sb.begin() + pos + 32 + bytes);
    surfaces[idx] = std::move(s);
    pos += 32 + bytes;
  }
  if (surfaces.empty()) {
    res.log = "no reference surfaces";
    return res;
  }
  const RefSurface& probe = surfaces.begin()->second;
  const int gridW = static_cast<int>((probe.w + 63) / 64);
  const int gridH = static_cast<int>((probe.h + 63) / 64);

  RfxGpuDecoder decoder;
  if (!decoder.Init(gridW, gridH, static_cast<int>(probe.w), static_cast<int>(probe.h))) {
    res.log = "gpu decoder init failed";
    return res;
  }
  res.ran = true;

  char line[256];
  for (size_t r = 1; r <= streams.size(); ++r) {
    const uint8_t* payload = cb.data() + streams[r - 1].first;
    const size_t len = streams[r - 1].second;
    // Collect the tile set and rects for this record (for clipped comparison).
    std::vector<uint32_t> tiles;
    std::vector<RfxRect> rects;
    RfxParseStats stats;
    ParseRfxProgressive(
        payload, len,
        [&](const RfxTileRef& t) {
          tiles.push_back(static_cast<uint32_t>(t.yIdx) * gridW + t.xIdx);
          for (uint16_t i = 0; i < t.numRects; ++i) rects.push_back(t.rects[i]);
        },
        &stats);
    res.tiles += stats.tiles;
    res.firstTiles += stats.firstTiles;
    res.upgradeTiles += stats.upgradeTiles;

    const bool ok = decoder.DecodeMessage(payload, len);
    if (ok) {
      res.decodedOk += stats.tiles;
    } else {
      res.decodedFail += stats.tiles;
    }
    auto it = surfaces.find(static_cast<uint32_t>(r));
    if (it == surfaces.end()) {
      continue;
    }
    const RefSurface& ref = it->second;
    std::vector<uint8_t> ours;
    if (!decoder.ReadSurface(&ours)) {
      continue;
    }

    size_t mismatch = 0, compared = 0;
    int64_t sumAbs = 0;
    for (uint32_t ti : tiles) {
      const int tx = static_cast<int>(ti) % gridW;
      const int ty = static_cast<int>(ti) / gridW;
      const int copyW = (tx * 64 + 64 <= static_cast<int>(ref.w)) ? 64 : (ref.w - tx * 64);
      const int copyH = (ty * 64 + 64 <= static_cast<int>(ref.h)) ? 64 : (ref.h - ty * 64);
      if (copyW <= 0 || copyH <= 0) continue;
      for (int row = 0; row < copyH; ++row) {
        const int py = ty * 64 + row;
        const uint8_t* a = ours.data() + static_cast<size_t>(py) * ref.stride + tx * 64 * 4;
        const uint8_t* b = ref.data.data() + static_cast<size_t>(py) * ref.stride + tx * 64 * 4;
        for (int x = 0; x < copyW; ++x) {
          const int px = tx * 64 + x;
          bool clipped = rects.empty();
          for (const RfxRect& rc : rects) {
            if (px >= rc.x && px < rc.x + rc.width && py >= rc.y && py < rc.y + rc.height) {
              clipped = true;
              break;
            }
          }
          if (!clipped) continue;
          for (int c = 0; c < 3; ++c) {
            int d = a[x * 4 + c] - b[x * 4 + c];
            if (d < 0) d = -d;
            sumAbs += d;
            if (d > 2) mismatch++;
            compared++;
          }

        }
      }
    }
    res.comparedRecords++;
    res.compared += compared;
    res.mismatch += mismatch;
    res.meanAbs += static_cast<double>(sumAbs);
    if (mismatch != 0) res.badRecords++;
    std::snprintf(line, sizeof(line),
                  "rec%zu first=%u up=%u cmp=%zu mism=%zu", r, stats.firstTiles,
                  stats.upgradeTiles, compared, mismatch);
    HMRDP_LOGI("gpu rfx rec %{public}s", line);
    res.log += line;
    res.log += "\n";
  }
  res.meanAbs = res.compared ? res.meanAbs / static_cast<double>(res.compared) : 0.0;
  res.ok = res.ran && res.badRecords == 0 && res.compared > 0;
  return res;
}

}  // namespace hmrdp
