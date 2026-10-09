#include "hmrdp_file_clipboard.h"

#include <sys/stat.h>

namespace hmrdp::fileclip {

bool FileSizeOfPath(const std::string& path, uint64_t* size) {
  struct stat st = {};
  if (stat(path.c_str(), &st) != 0 || size == nullptr) {
    return false;
  }
  *size = static_cast<uint64_t>(st.st_size);
  return true;
}

}  // namespace hmrdp::fileclip
