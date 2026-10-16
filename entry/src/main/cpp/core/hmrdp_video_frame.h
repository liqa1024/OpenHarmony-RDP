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
};

// Where a decode bridge's pictures go. Implemented by the session's presenter owner (the Moonlight
// video bridge), which holds the presenter under its own lock.
//
// The call runs on the decoder's hand-off thread, never on a codec callback thread, and it may block
// (the presenter waits its GPU fence inside). The buffer stays owned by the decoder and is returned
// to the codec as soon as the call returns, so the receiver must be done with it by then.
class VideoFrameSink {
 public:
  virtual ~VideoFrameSink() = default;
  virtual void PresentVideoFrame(const void* nativeBuffer, const VideoFrameGeometry& geometry) = 0;
};

}

#endif
