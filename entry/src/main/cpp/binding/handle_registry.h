#ifndef HMRDP_BINDING_HANDLE_REGISTRY_H
#define HMRDP_BINDING_HANDLE_REGISTRY_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace hmrdp::binding {

template <typename T>
class HandleRegistry {
 public:
  HandleRegistry() = default;
  HandleRegistry(const HandleRegistry&) = delete;
  HandleRegistry& operator=(const HandleRegistry&) = delete;

  int64_t Add(std::shared_ptr<T> object) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t handle = nextId_++;
    objects_[handle] = std::move(object);
    return handle;
  }

  std::shared_ptr<T> Find(int64_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = objects_.find(handle);
    return it == objects_.end() ? nullptr : it->second;
  }

  std::shared_ptr<T> Take(int64_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = objects_.find(handle);
    if (it == objects_.end()) {
      return nullptr;
    }
    std::shared_ptr<T> object = std::move(it->second);
    objects_.erase(it);
    return object;
  }

  std::vector<std::shared_ptr<T>> TakeAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<T>> out;
    out.reserve(objects_.size());
    for (auto& entry : objects_) {
      out.push_back(std::move(entry.second));
    }
    objects_.clear();
    return out;
  }

  std::vector<std::pair<int64_t, std::shared_ptr<T>>> Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<int64_t, std::shared_ptr<T>>> out;
    out.reserve(objects_.size());
    for (const auto& entry : objects_) {
      out.emplace_back(entry.first, entry.second);
    }
    return out;
  }

  std::size_t Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return objects_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::map<int64_t, std::shared_ptr<T>> objects_;
  int64_t nextId_ = 1;
};

}  // namespace hmrdp::binding

#endif  // HMRDP_BINDING_HANDLE_REGISTRY_H
