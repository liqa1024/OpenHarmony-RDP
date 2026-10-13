#ifndef HMRDP_FILE_CLIPBOARD_H
#define HMRDP_FILE_CLIPBOARD_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace hmrdp::fileclip {

// With long names negotiated the server maps formats by name; these ids need only be unique
// within the session.
inline constexpr uint32_t kFileGroupDescriptorId = 0xC001;
inline constexpr char kFileGroupDescriptorName[] = "FileGroupDescriptorW";
inline constexpr uint32_t kFileContentsId = 0xC002;
inline constexpr char kFileContentsName[] = "FileContents";

bool FileSizeOfPath(const std::string& path, uint64_t* size);

}

#endif
