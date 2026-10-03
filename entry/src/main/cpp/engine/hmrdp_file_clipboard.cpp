/*
 * HmRdp - file-clipboard domain (native side). See hmrdp_file_clipboard.h.
 */
#include "hmrdp_file_clipboard.h"

#include <sys/stat.h>

#include <sstream>

namespace hmrdp::fileclip {

std::string SanitizeClipboardFileName(const std::string& name) {
  std::string out;
  out.reserve(name.size());
  for (char c : name) {
    if (c == '/' || c == '\\' || c == ':' || c == '\0') {
      out.push_back('_');
    } else {
      out.push_back(c);
    }
  }
  if (out.empty() || out == "." || out == "..") {
    out = "file";
  }
  return out;
}

std::string BuildTransferProgress(int direction, uint64_t done, uint64_t total,
                                  uint32_t fileIndex, uint32_t fileCount) {
  std::ostringstream out;
  out << direction << '|' << done << '|' << total << '|' << fileIndex << '|'
      << fileCount;
  return out.str();
}

std::string JoinSandboxPath(const std::string& dir, const std::string& name) {
  if (dir.empty()) {
    return name;
  }
  return dir.back() == '/' ? dir + name : dir + "/" + name;
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
