/*
 * HmRdp - file-clipboard domain (native side).
 *
 * This unit owns everything specific to the file clipboard: the descriptor POD,
 * the wire-format ids/names, and the small pure helpers (name sanitising, path
 * joining, size lookup, progress payload). The file-transfer state machine and
 * the cliprdr synchronous serve stay in the session (they are the adapter that
 * must answer on the RDP thread); the format knowledge itself lives in ArkTS
 * (FileTransferCodec). Keeping the file domain here is what lets the session be
 * an ordinary wire-up rather than a grab-bag.
 */
#ifndef HMRDP_FILE_CLIPBOARD_H
#define HMRDP_FILE_CLIPBOARD_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hmrdp::fileclip {

// One file on the clipboard: either offered locally or parsed by ArkTS from the
// server's FILEGROUPDESCRIPTORW. Index order is the FILECONTENTS listIndex, so
// it must be preserved verbatim.
struct FileInfo {
  std::string name;
  uint64_t size = 0;
  bool sizeKnown = false;
  bool isDirectory = false;
};

// Client-side ids and names of the file clipboard formats. With long format
// names negotiated (CB_USE_LONG_FORMAT_NAMES) the server maps them by name, so
// these ids only have to stay unique and stable within the session.
inline constexpr uint32_t kFileGroupDescriptorId = 0xC001;
inline constexpr char kFileGroupDescriptorName[] = "FileGroupDescriptorW";
inline constexpr uint32_t kFileContentsId = 0xC002;
inline constexpr char kFileContentsName[] = "FileContents";

// A FILECONTENTS_RANGE response carries at most this many bytes; the server
// answers each request in a single message, so this only bounds the per-message
// buffer, not correctness.
inline constexpr uint32_t kFileChunkBytes = 256 * 1024;

// A clipboard file name comes from the peer; never let one escape the target
// directory. (ArkTS sanitises too; this is the native defense-in-depth guard.)
std::string SanitizeClipboardFileName(const std::string& name);

// File-transfer progress event payload:
// "<direction>|<doneBytes>|<totalBytes>|<fileIndex>|<fileCount>".
// `fileIndex` is 1-based, 0 while nothing has started.
std::string BuildTransferProgress(int direction, uint64_t done, uint64_t total,
                                  uint32_t fileIndex, uint32_t fileCount);

std::string JoinSandboxPath(const std::string& dir, const std::string& name);

bool FileSizeOfPath(const std::string& path, uint64_t* size);

}  // namespace hmrdp::fileclip

#endif  // HMRDP_FILE_CLIPBOARD_H
