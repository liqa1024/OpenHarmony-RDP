#ifndef HMRDP_VIDEO_FRAME_H
#define HMRDP_VIDEO_FRAME_H

namespace hmrdp {

// One decoded picture's layout, as the codec reports it in buffer mode (output description /
// onStreamChanged): `stride` is the byte distance between two consecutive luma rows, `sliceHeight`
// the rows allocated for a plane (>= the visible height), and the interleaved chroma plane starts at
// `stride * sliceHeight`. `visible*`/`origin*` describe the picture after cropping.
//
// Zero means "not reported": the importer then falls back to the buffer's own config, which is the
// only reading available in surface mode.
struct VideoFrameGeometry {
  int stride = 0;
  int sliceHeight = 0;
  int visibleWidth = 0;
  int visibleHeight = 0;
  int originX = 0;
  int originY = 0;
  // The codec's pixel format (OH_AVPixelFormat, 0 = not reported). The byte importer only accepts
  // the interleaved 4:2:0 layouts it can address; `cbFirst` says whether the first chroma byte is Cb
  // (NV12) or Cr (NV21).
  int pixelFormat = 0;
  bool cbFirst = true;
};

// Where a decode bridge's pictures go. Implemented by the session's presenter owner (the Moonlight
// video bridge), which holds the presenter under its own lock.
//
// The call runs on the decoder's hand-off thread, never on a codec callback thread. A picture is
// free to return to the codec when PresentVideoFrame returns -1, or after WaitVideoFrame(token) for
// a non-negative token: the GPU may still be reading the codec buffer until then. Keeping two
// pictures in flight lets decode overlap the presenter's GPU work.
class VideoFrameSink {
 public:
  virtual ~VideoFrameSink() = default;
  virtual int PresentVideoFrame(const void* nativeBuffer, const VideoFrameGeometry& geometry) = 0;
  virtual void WaitVideoFrame(int token) = 0;
};

}

#endif
