#include "shared_object_pool.h"
#include "utils.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <list>
#include <mutex>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <zstd.h>

namespace detsim::snapshot {
namespace {
constexpr uint64_t magic = 0x314c4f4f50535344ULL;
constexpr size_t cache_limit = size_t{64} << 20;
constexpr size_t cache_entry_limit = 16'384;
enum class Kind : uint32_t { Blob = 1, Ready = 2 };
struct Header {
    uint64_t magic;
    uint32_t version, kind;
    uint64_t key, raw_size, checksum, stored_size;
    uint32_t codec, reserved;
};
static_assert(sizeof(Header) == 56);
struct Fd {
    int value = -1;
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() { if (value >= 0) close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};
struct Conflict : std::runtime_error { using std::runtime_error::runtime_error; };
[[noreturn]] void io_error(const char* action) {
    throw std::runtime_error(std::string("SharedObjectPool ") + action + ": " + std::strerror(errno));
}
void require(bool ok, const char* action) { if (!ok) throw std::runtime_error(std::string("SharedObjectPool ") + action); }
void exact(int fd, void* bytes, size_t size, bool writing) {
    auto* next = static_cast<uint8_t*>(bytes);
    while (size) {
        const ssize_t count = writing ? write(fd, next, size) : read(fd, next, size);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) io_error(writing ? "write" : "read");
        require(count > 0, "short object I/O");
        next += count; size -= size_t(count);
    }
}
struct FileLock {
    int fd;
    FileLock(int descriptor, int mode) : fd(descriptor) {
        int result; do { result = flock(fd, mode); } while (result && errno == EINTR);
        if (result) io_error("lock");
    }
    ~FileLock() { flock(fd, LOCK_UN); }
};
std::string filename(hash_type key) {
    char text[24]; std::snprintf(text, sizeof(text), "%016lx.obj", static_cast<unsigned long>(key)); return text;
}

}
struct SharedObjectPool::Impl {
    std::string directory;
    bool read_only;
    Fd root, blobs, ready, barrier;
    std::mutex mutex;
    std::exception_ptr failure;
    struct CacheKey {
        Kind kind;
        hash_type key;
        bool operator==(const CacheKey&) const = default;
    };
    struct CacheHash {
        size_t operator()(const CacheKey& value) const noexcept {
            return std::hash<hash_type>{}(value.key) ^ (size_t(value.kind) << 1);
        }
    };
    struct Entry { Buffer bytes; std::list<CacheKey>::iterator position; };
    std::unordered_map<CacheKey, Entry, CacheHash> cache;
    std::list<CacheKey> lru;
    size_t cache_bytes = 0;
    Impl(std::string path, bool create, bool readonly)
        : directory(std::filesystem::absolute(path).lexically_normal().string()), read_only(readonly),
          root(open_root(create)), blobs(open_directory("blobs", create)), ready(open_directory("ready", create)),
          barrier(openat(root.value, ".pool.lock", O_CLOEXEC | O_NOFOLLOW | ((readonly ? O_RDONLY : O_RDWR) | (create && !readonly ? O_CREAT : 0)), 0600)) {
        if (barrier.value < 0) io_error("open barrier");
        struct stat status{};
        require(!fstat(barrier.value, &status) && S_ISREG(status.st_mode), "invalid barrier file");
    }
    int open_root(bool create) {
        require(!directory.empty(), "empty directory");
        if (create && !read_only) std::filesystem::create_directories(directory);
        const int fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) io_error("open root directory");
        return fd;
    }
    int open_directory(const char* name, bool create) {
        if (create && !read_only && mkdirat(root.value, name, 0700) && errno != EEXIST) io_error("create object directory");
        const int fd = openat(root.value, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) io_error("open object directory");
        return fd;
    }
    void check() const { if (failure) std::rethrow_exception(failure); }
    int dir(Kind kind) const { return kind == Kind::Blob ? blobs.value : ready.value; }
    void remember(CacheKey key, Buffer bytes) {
        if (bytes->size() > cache_limit || cache.contains(key)) return;
        while (cache_bytes > cache_limit - bytes->size() || cache.size() >= cache_entry_limit) {
            const auto found = cache.find(lru.back()); cache_bytes -= found->second.bytes->size();
            cache.erase(found); lru.pop_back();
        }
        lru.push_front(key);
        try { cache.emplace(key, Entry{bytes, lru.begin()}); } catch (...) { lru.pop_front(); throw; }
        cache_bytes += bytes->size();
    }
    Buffer load(Kind kind, hash_type key, bool cached = true) {
        if (!key) return {};
        const CacheKey id{kind, key};
        if (cached) {
            const auto found = cache.find(id);
            if (found != cache.end()) { lru.splice(lru.begin(), lru, found->second.position); return found->second.bytes; }
        }
        const auto name = filename(key);
        Fd fd(openat(dir(kind), name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        if (fd.value < 0) { if (errno == ENOENT) return {}; io_error("open object"); }
        struct stat status{};
        require(!fstat(fd.value, &status) && S_ISREG(status.st_mode) && status.st_size >= off_t(sizeof(Header)), "invalid object file");
        Header h{}; exact(fd.value, &h, sizeof(h), false);
        require(h.magic == magic && h.version == 1 && h.kind == uint32_t(kind) && h.key == key && !h.reserved &&
            h.raw_size && h.raw_size <= UINT32_MAX && h.stored_size && h.stored_size <= ZSTD_compressBound(size_t(h.raw_size)) &&
            h.stored_size == uint64_t(status.st_size)-sizeof(Header) && h.codec <= 1, "invalid typed object header/size");
        std::vector<uint8_t> encoded;
        if (h.codec) {
            encoded.resize(size_t(h.stored_size)); exact(fd.value, encoded.data(), encoded.size(), false);
            require(ZSTD_getFrameContentSize(encoded.data(), encoded.size()) == h.raw_size,
                    "compressed object declared size mismatch");
        } else require(h.stored_size == h.raw_size, "invalid raw object size");
        auto data = std::make_shared<std::vector<uint8_t>>(size_t(h.raw_size));
        if (!h.codec) exact(fd.value, data->data(), data->size(), false);
        else {
            const auto size = ZSTD_decompress(data->data(), data->size(), encoded.data(), encoded.size());
            require(!ZSTD_isError(size) && size == data->size(), "invalid compressed object");
        }
        const auto checksum = XXHash64::hash(data->data(), data->size());
        require(checksum == h.checksum && (kind != Kind::Blob || checksum == key), "object checksum/key mismatch");
        remember(id, data); return data;
    }
    void publish(Kind kind, hash_type key, const void* bytes, size_t size) {
        // Private-pool lifetime contract: published objects are never modified
        // or removed externally. Only successful publication/cold validation
        // admits immutable owners to this bounded, pool-instance-local cache.
        const CacheKey id{kind,key};
        const auto known = cache.find(id);
        if (known != cache.end()) {
            if (known->second.bytes->size() != size || std::memcmp(known->second.bytes->data(),bytes,size))
                throw Conflict("SharedObjectPool conflicting cached object");
            lru.splice(lru.begin(),lru,known->second.position);
            return;
        }
        if (auto prior = load(kind, key, false)) {
            if (prior->size() != size || std::memcmp(prior->data(), bytes, size)) throw Conflict("SharedObjectPool conflicting same-key object");
            return;
        }
        FileLock exclusion(barrier.value, LOCK_SH);
        std::vector<uint8_t> encoded;
        uint32_t codec = 0;
        if (size >= 4096) {
            encoded.resize(ZSTD_compressBound(size));
            const auto compressed = ZSTD_compress(encoded.data(), encoded.size(), bytes, size, 1);
            require(!ZSTD_isError(compressed), "compression failed");
            if (compressed < size) { encoded.resize(compressed); codec = 1; }
            else encoded.clear();
        }
        const void* payload = codec ? encoded.data() : bytes;
        const size_t stored = codec ? encoded.size() : size;
        Header header{magic,1,uint32_t(kind),key,size,XXHash64::hash(bytes,size),stored,codec,0};
        const auto temporary = ".tmp-" + std::to_string(getpid()) + "-XXXXXX";
        std::string full = directory + (kind == Kind::Blob ? "/blobs/" : "/ready/") + temporary;
        Fd fd(mkostemp(full.data(), O_CLOEXEC));
        if (fd.value < 0) io_error("create temporary object");
        struct Cleanup { std::string path; ~Cleanup() { if (!path.empty()) unlink(path.c_str()); } } cleanup{full};
        exact(fd.value, &header, sizeof(header), true);
        exact(fd.value, const_cast<void*>(payload), stored, true);
        if (fchmod(fd.value, 0444)) io_error("seal object permissions");
        const auto name = filename(key);
        if (linkat(AT_FDCWD, full.c_str(), dir(kind), name.c_str(), 0)) {
            if (errno != EEXIST) io_error("publish object");
            const auto prior = load(kind,key,false);
            require(bool(prior), "concurrent publication disappeared");
            if (prior->size() != size || std::memcmp(prior->data(),bytes,size)) throw Conflict("SharedObjectPool conflicting concurrent publication");
        }
        if (unlink(full.c_str())) io_error("remove temporary object");
        cleanup.path.clear();
        const int written = fd.value;
        fd.value = -1; // Linux close errors must not cause a retry on a reused fd.
        if (close(written)) io_error("close published object");
        if (size <= cache_limit)
            remember({kind,key}, std::make_shared<const std::vector<uint8_t>>(
                static_cast<const uint8_t*>(bytes), static_cast<const uint8_t*>(bytes)+size));
    }
};
SharedObjectPool::SharedObjectPool(std::string dir, bool create, bool read_only) {
    if (dir.empty()) throw std::invalid_argument("SharedObjectPool empty directory");
    impl_ = std::make_unique<Impl>(std::move(dir),create,read_only);
}
SharedObjectPool::~SharedObjectPool() = default;
const std::string& SharedObjectPool::path() const { return impl_->directory; }
bool SharedObjectPool::is_read_only() const noexcept { return impl_->read_only; }
void SharedObjectPool::publish_blob(hash_type key, const void* bytes, size_t size) {
    if (!key || !bytes || !size || size > UINT32_MAX || XXHash64::hash(bytes,size) != key)
        throw std::invalid_argument("SharedObjectPool invalid blob key/bytes");
    auto& p=*impl_; std::lock_guard lock(p.mutex); p.check();
    if (p.read_only) throw std::runtime_error("SharedObjectPool is read-only");
    try { p.publish(Kind::Blob,key,bytes,size); }
    catch (const Conflict&) { throw; }
    catch (...) { p.failure=std::current_exception(); throw; }
}
void SharedObjectPool::publish_ready(hash_type key, const std::vector<uint8_t>& bytes) {
    if (!key || bytes.empty() || bytes.size() > UINT32_MAX) throw std::invalid_argument("SharedObjectPool invalid ready record");
    auto& p=*impl_; std::lock_guard lock(p.mutex); p.check();
    if (p.read_only) throw std::runtime_error("SharedObjectPool is read-only");
    try { p.publish(Kind::Ready,key,bytes.data(),bytes.size()); }
    catch (const Conflict&) { throw; }
    catch (...) { p.failure=std::current_exception(); throw; }
}
SharedObjectPool::Buffer SharedObjectPool::load_blob(hash_type key) {
    auto& p=*impl_; std::lock_guard lock(p.mutex); p.check();
    try { return p.load(Kind::Blob,key); } catch (...) { p.failure=std::current_exception(); throw; }
}
SharedObjectPool::Buffer SharedObjectPool::load_ready(hash_type key) {
    auto& p=*impl_; std::lock_guard lock(p.mutex); p.check();
    try { return p.load(Kind::Ready,key); } catch (...) { p.failure=std::current_exception(); throw; }
}
void SharedObjectPool::flush() {
    auto& p=*impl_; std::lock_guard lock(p.mutex); p.check();
    try {
        FileLock exclusion(p.barrier.value, LOCK_EX);
        // Linux syncfs waits for filesystem data/metadata writeback. Since
        // Linux 5.8 it also reports inode writeback failures; ENOSYS and all
        // other errors fail this barrier. Errors can include unrelated files on
        // the same filesystem. Root/blobs/ready may be separate mounts.
        const std::array<int,3> descriptors{p.root.value,p.blobs.value,p.ready.value};
        std::array<dev_t,3> devices{};
        size_t count=0;
        for (const int descriptor : descriptors) {
            struct stat status{};
            if (fstat(descriptor,&status)) io_error("stat pool filesystem");
            require(S_ISDIR(status.st_mode),"invalid pool directory");
            if (std::find(devices.begin(),devices.begin()+count,status.st_dev) != devices.begin()+count) continue;
            if (syncfs(descriptor)) io_error("sync filesystem");
            devices[count++]=status.st_dev;
        }
        // create_directories may have created the pool and path prefixes. Sync
        // their parent entries as well; syncing only the pool inode is not a
        // durability guarantee for a newly linked directory tree.
        auto parent = std::filesystem::path(p.directory).parent_path();
        for (;;) {
            Fd fd(open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
            if (fd.value < 0) io_error("open parent directory");
            if (fsync(fd.value)) io_error("sync parent directory");
            const auto next = parent.parent_path();
            if (next == parent) break;
            parent = next;
        }
    } catch (...) { p.failure=std::current_exception(); throw; }
}
}
