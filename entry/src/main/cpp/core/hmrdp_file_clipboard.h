/*
 * HmRdp - file-clipboard domain (native side).
 *
 * This unit owns the file-clipboard wire-format ids/names and the small pure
 * helpers the native *upload* serve needs (size lookup, progress payload). The
 * format knowledge itself lives in ArkTS (FileTransferCodec), as does the whole
 * download state machine; native only serves blocks the server pulls (a
 * synchronous cliprdr callback, so it stays native, see doc_agent/
 * architecture.md §3.1).
 */
#ifndef HMRDP_FILE_CLIPBOARD_H
#define HMRDP_FILE_CLIPBOARD_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace hmrdp::fileclip {

// Client-side ids and names of the file clipboard formats. With long format
// names negotiated (CB_USE_LONG_FORMAT_NAMES) the server maps them by name, so
// these ids only have to stay unique and stable within the session.
inline constexpr uint32_t kFileGroupDescriptorId = 0xC001;
inline constexpr char kFileGroupDescriptorName[] = "FileGroupDescriptorW";
inline constexpr uint32_t kFileContentsId = 0xC002;
inline constexpr char kFileContentsName[] = "FileContents";

// Upload progress event payload:
// "<direction>|<doneBytes>|<totalBytes>|<fileIndex>|<fileCount>".
// `fileIndex` is 1-based, 0 while nothing has started.
std::string BuildTransferProgress(int direction, uint64_t done, uint64_t total,
                                  uint32_t fileIndex, uint32_t fileCount);

bool FileSizeOfPath(const std::string& path, uint64_t* size);

}  // namespace hmrdp::fileclip

#endif  // HMRDP_FILE_CLIPBOARD_H
