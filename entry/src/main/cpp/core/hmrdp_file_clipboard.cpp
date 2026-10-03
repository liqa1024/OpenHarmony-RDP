/*
 * HmRdp - file-clipboard domain (native side). See hmrdp_file_clipboard.h.
 */
#include "hmrdp_file_clipboard.h"

#include <sys/stat.h>

#include <sstream>

namespace hmrdp::fileclip {

std::string BuildTransferProgress(int direction, uint64_t done, uint64_t total,
                                  uint32_t fileIndex, uint32_t fileCount) {
  std::ostringstream out;
  out << direction << '|' << done << '|' << total << '|' << fileIndex << '|'
      << fileCount;
  return out.str();
}

bool FileSizeOfPath(const std::string& path, uint64_t* size) {
  struct stat st = {};
  if (stat(path.c_str(), &st) != 0 || size == nullptr) {
    return false;
  }
  *size = static_cast<uint64_t>(st.st_size);
  return true;
}

}  // namespace hmrdp::fileclip
