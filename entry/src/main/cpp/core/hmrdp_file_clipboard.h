#ifndef HMRDP_FILE_CLIPBOARD_H
#define HMRDP_FILE_CLIPBOARD_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace hmrdp::fileclip {

// With long format names negotiated the server maps formats by name, so these
// ids only need to stay unique and stable within the session.
inline constexpr uint32_t kFileGroupDescriptorId = 0xC001;
inline constexpr char kFileGroupDescriptorName[] = "FileGroupDescriptorW";
inline constexpr uint32_t kFileContentsId = 0xC002;
inline constexpr char kFileContentsName[] = "FileContents";

bool FileSizeOfPath(const std::string& path, uint64_t* size);

}  // namespace hmrdp::fileclip

#endif  // HMRDP_FILE_CLIPBOARD_H
