/*
 * HmRdp - typed handle registry for the Node-API boundary.
 *
 * Every native object that ArkTS holds is represented by a plain integer handle
 * (a JS number). The object itself never crosses the boundary: it stays native,
 * owned by a shared_ptr in one of these registries, and ArkTS only keeps the
 * handle. This is what lets the boundary be a thin translation layer over
 * opaque references with no GC-managed native payload (doc_agent/architecture.md).
 *
 * Each HandleRegistry<T> has its own id space and its own object type, so a
 * handle issued by one registry can never resolve in another: a 0x1 from the
 * session registry is not the replay registered as 0x1. Handles are monotonic
 * and never reused, so a stale handle misses instead of aliasing a new object.
 *
 * Find()/Snapshot() hand out shared_ptr copies, so a caller keeps the object
 * alive for as long as it needs it even if the entry is removed concurrently
 * (destroySession / stopReplay). That is what makes lookup race-free without a
 * caller-visible lease.
 */
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

  // Registers an object and returns its new handle (always > 0).
  int64_t Add(std::shared_ptr<T> object) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t handle = nextId_++;
    objects_[handle] = std::move(object);
    return handle;
  }

  // Returns the object for `handle`, or nullptr when it is unknown. The
  // returned shared_ptr keeps it alive past removal.
  std::shared_ptr<T> Find(int64_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = objects_.find(handle);
    return it == objects_.end() ? nullptr : it->second;
  }

  // Removes and returns the object, or nullptr when the handle is unknown.
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

  // Removes every object and returns them, in handle order.
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

  // Handle/object pairs without removing them, for iteration.
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
