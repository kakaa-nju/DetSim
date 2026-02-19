/*
 * fd_manager.cpp - Unified file descriptor manager
 */

#include "fd_manager.h"
#include <algorithm>
#include "cereal/archives/binary.hpp"
#include "cereal/types/unordered_set.hpp"

int FdManager::allocate_fd(FdType type) {
    int fd = next_fd_.fetch_add(1);
    allocated_fds_.insert(fd);
    return fd;
}

void FdManager::release_fd(int fd) {
    allocated_fds_.erase(fd);
}

FdType FdManager::get_fd_type(int fd) const {
    if (allocated_fds_.count(fd)) {
        /* For now, we don't track type separately */
        return FdType::UNKNOWN;
    }
    return FdType::UNKNOWN;
}

bool FdManager::is_allocated(int fd) const {
    return allocated_fds_.count(fd) > 0;
}

template <class Archive>
void FdManager::serialize(Archive &ar) {
    /* std::atomic<int> cannot be serialized directly, use temporary int */
    int next_fd_val = next_fd_.load();
    ar(next_fd_val, allocated_fds_);
    next_fd_ = next_fd_val;
}

/* Explicit instantiation */
template void FdManager::serialize<cereal::BinaryInputArchive>(cereal::BinaryInputArchive &);
template void FdManager::serialize<cereal::BinaryOutputArchive>(cereal::BinaryOutputArchive &);
