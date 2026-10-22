#include "shared_object_pool.h"
#include "utils.h"
#include <atomic>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <sys/file.h>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
using detsim::snapshot::SharedObjectPool;
static std::atomic<bool> fail_sync{false};
static std::atomic<int> fail_write{0};
static std::atomic<uint64_t> read_calls{0},flock_calls{0},syncfs_calls{0},fstat_calls{0},fsync_calls{0};
static std::atomic<int> mock_devices{0},fail_syncfs{0};
static std::atomic<bool> fail_stat{false},sync_without_exclusive{false};
static thread_local bool exclusive_held=false;
static std::mutex device_mutex;
static std::vector<dev_t> synced_devices;
static dev_t visible_device(int fd,dev_t actual){
    const int mode=mock_devices;
    if(!mode)return actual;
    char path[4096],name[64];snprintf(name,sizeof(name),"/proc/self/fd/%d",fd);
    const auto size=readlink(name,path,sizeof(path)-1);
    if(size<0)return actual;
    path[size]=0;const auto leaf=std::filesystem::path(path).filename().string();
    if(leaf=="blobs")return mode==1?101:202;
    if(leaf=="ready")return mode==3?303:mode==2?202:101;
    return 101;
}
extern "C" int __real_fstat(int,struct stat*);
extern "C" int __wrap_fstat(int fd,struct stat* status){
    ++fstat_calls;const int result=__real_fstat(fd,status);
    if(!result&&S_ISDIR(status->st_mode)){
        if(fail_stat){errno=EIO;return -1;}
        status->st_dev=visible_device(fd,status->st_dev);
    }
    return result;
}
extern "C" int __real_syncfs(int);
extern "C" int __wrap_syncfs(int fd){
    ++syncfs_calls;if(!exclusive_held)sync_without_exclusive=true;
    struct stat status{};
    if(__real_fstat(fd,&status))return -1;
    {std::lock_guard lock(device_mutex);synced_devices.push_back(visible_device(fd,status.st_dev));}
    if(const int error=fail_syncfs){errno=error;return -1;}
    return mock_devices?0:__real_syncfs(fd);
}
extern "C" ssize_t __real_read(int,void*,size_t);
extern "C" int __real_flock(int,int);
extern "C" ssize_t __wrap_read(int fd,void* data,size_t size){++read_calls;return __real_read(fd,data,size);}
extern "C" int __wrap_flock(int fd,int mode){
    ++flock_calls;const int result=__real_flock(fd,mode);
    if(!result){if(mode&LOCK_EX)exclusive_held=true;else if(mode&LOCK_UN)exclusive_held=false;}
    return result;
}
extern "C" int __real_fsync(int);
extern "C" ssize_t __real_write(int,const void*,size_t);
extern "C" int __wrap_fsync(int fd) {
    ++fsync_calls;
    if (fail_sync) { errno=EIO; return -1; } return __real_fsync(fd);
}
extern "C" ssize_t __wrap_write(int fd,const void* data,size_t size) {
    if (fail_write == 1) { fail_write=2; return __real_write(fd,data,size); }
    if (fail_write == 2) { fail_write=3; return __real_write(fd,data,size/2); }
    if (fail_write == 3) { errno=EIO; return -1; }
    return __real_write(fd,data,size);
}
static void require(bool ok,const char* why) { if (!ok) throw std::runtime_error(why); }
template<class F> static void rejects(F fn) { bool bad=false; try { fn(); } catch(const std::exception&) {bad=true;} require(bad,"expected rejection"); }
static std::filesystem::path object(const std::filesystem::path& dir,const char* type,hash_type key) {
    char name[24]; snprintf(name,sizeof(name),"%016lx.obj",static_cast<unsigned long>(key)); return dir/type/name;
}
static void corrupt(const std::filesystem::path& path,size_t offset) {
    chmod(path.c_str(),0600);
    std::fstream file(path,std::ios::in|std::ios::out|std::ios::binary); require(bool(file),"open corrupt fixture");
    file.seekg(offset); char byte=0;file.read(&byte,1);require(bool(file),"read corrupt fixture");
    byte^=0x55;file.seekp(offset);file.write(&byte,1);require(bool(file),"write corrupt fixture");
}
static void filesystem_barriers(const std::filesystem::path& root){
    const std::vector<uint8_t> bytes(64,'s');const auto key=XXHash64::hash(bytes.data(),bytes.size());
    SharedObjectPool pool((root/"barrier-count").string());pool.publish_blob(key,bytes.data(),bytes.size());
    const auto calls=syncfs_calls.load(),stats=fstat_calls.load(),syncs=fsync_calls.load();
    pool.flush();
    const auto first=syncfs_calls.load()-calls,first_stats=fstat_calls.load()-stats,ancestors=fsync_calls.load()-syncs;
    require(first==1&&first_stats==3&&ancestors>0,"single-filesystem barrier did not use one syncfs and ancestor fsync");
    for(uint64_t i=1;i<=256;++i)pool.publish_ready(i,bytes);
    const auto more_calls=syncfs_calls.load(),more_stats=fstat_calls.load(),more_syncs=fsync_calls.load();
    pool.flush();
    require(syncfs_calls.load()-more_calls==first&&fstat_calls.load()-more_stats==first_stats&&
            fsync_calls.load()-more_syncs==ancestors,"barrier syscall count grew with object count");
    // Simulate separate mounts without modifying the host mount namespace.
    // syncfs records the actual descriptor's mocked device, so calling the root
    // descriptor repeatedly cannot satisfy coverage of distinct devices.
    for(int mode:{1,2,3,4}){
        SharedObjectPool view((root/"barrier-count").string(),false,true);
        {std::lock_guard lock(device_mutex);synced_devices.clear();}
        mock_devices=mode;view.flush();mock_devices=0;
        std::lock_guard lock(device_mutex);
        const std::set<dev_t> expected=mode==1?std::set<dev_t>{101}:mode==3?std::set<dev_t>{101,202,303}:std::set<dev_t>{101,202};
        require(synced_devices.size()==expected.size()&&std::set<dev_t>(synced_devices.begin(),synced_devices.end())==expected,
                "distinct backing filesystem coverage/dedup failed");
    }
    for(int error:{EIO,ENOSPC,ENOSYS}){
        SharedObjectPool broken((root/("syncfs-error-"+std::to_string(error))).string());broken.publish_blob(key,bytes.data(),bytes.size());
        fail_syncfs=error;rejects([&]{broken.flush();});fail_syncfs=0;
        rejects([&]{broken.load_blob(key);});
        const auto before=syncfs_calls.load();rejects([&]{broken.flush();});require(syncfs_calls==before,"poisoned barrier retried syncfs");
    }
    {
        SharedObjectPool broken((root/"stat-error").string());fail_stat=true;rejects([&]{broken.flush();});fail_stat=false;
        rejects([&]{broken.load_ready(1);});
    }
    require(!sync_without_exclusive,"syncfs ran without the exclusive publication barrier");
}
int main() {
    char tmp[]="/tmp/detsim-shared-pool-XXXXXX"; require(mkdtemp(tmp),"mkdtemp");
    const std::filesystem::path root(tmp);
    try {
        const auto dir=root/"concurrent"; SharedObjectPool pool(dir.string());
        const std::vector<uint8_t> bytes(8192,'p');const auto key=XXHash64::hash(bytes.data(),bytes.size());
        std::atomic<int> success=0,conflict=0;std::vector<std::thread> threads;
        std::atomic<bool> read_done=false;std::exception_ptr read_failure;
        std::thread reader([&] {
            try { SharedObjectPool view(dir.string(),false,true);
                while(!read_done){auto data=view.load_blob(key);if(data)require(*data==bytes,"partial publication observed");std::this_thread::yield();}
            } catch(...){read_failure=std::current_exception();}
        });
        for(int i=0;i<8;++i)threads.emplace_back([&]{SharedObjectPool writer(dir.string(),false);writer.publish_blob(key,bytes.data(),bytes.size());++success;});
        for(auto& t:threads)t.join();
        read_done=true;reader.join();if(read_failure)std::rethrow_exception(read_failure);
        require(success==8 && *pool.load_blob(key)==bytes,"concurrent same-key dedup failed");
        const auto reads=read_calls.load(),locks=flock_calls.load();
        pool.publish_blob(key,bytes.data(),bytes.size());
        require(read_calls==reads&&flock_calls==locks,"validated immutable cache hit touched disk/global lock");
        threads.clear();success=0;
        for(int i=0;i<8;++i)threads.emplace_back([&,i]{SharedObjectPool writer(dir.string(),false);std::vector<uint8_t> ready(32768,uint8_t(i));
            try{writer.publish_ready(77,ready);++success;}catch(const std::runtime_error&){++conflict;}});
        for(auto& t:threads)t.join();
        require(success==1&&conflict==7,"concurrent conflict did not reject");
        auto ready=pool.load_ready(77);require(ready&&ready->size()==32768,"ready missing");
        require(std::all_of(ready->begin(),ready->end(),[&](uint8_t c){return c==(*ready)[0];}),"mixed ready payload");
        rejects([&]{pool.publish_blob(key+1,bytes.data(),bytes.size());});require(*pool.load_blob(key)==bytes,"caller error poisoned valid pool");
        // Same numeric IDs in different typed namespaces must never alias,
        // including warmed cache hits, re-publication and cold reopen.
        {
            const auto typed_dir=root/"typed-cache";
            const std::vector<uint8_t> metadata{'r','e','a','d','y'};
            const std::vector<uint8_t> other{'o','t','h','e','r'};
            SharedObjectPool typed(typed_dir.string());
            typed.publish_blob(key,bytes.data(),bytes.size());
            typed.publish_ready(key,metadata);
            for(int i=0;i<3;++i) {
                const auto before_load_reads=read_calls.load(),before_load_locks=flock_calls.load();
                require(*typed.load_blob(key)==bytes,"typed cached blob alias");
                require(*typed.load_ready(key)==metadata,"typed cached ready alias");
                require(read_calls==before_load_reads&&flock_calls==before_load_locks,"typed cached load touched disk");
                const auto before_reads=read_calls.load(),before_locks=flock_calls.load();
                typed.publish_ready(key,metadata);
                typed.publish_blob(key,bytes.data(),bytes.size());
                require(read_calls==before_reads&&flock_calls==before_locks,"typed cache hit touched disk");
            }
            rejects([&]{typed.publish_blob(key,other.data(),other.size());});
            rejects([&]{typed.publish_ready(key,other);});
            require(*typed.load_blob(key)==bytes&&*typed.load_ready(key)==metadata,"invalid blob poisoned typed cache");
            SharedObjectPool different((root/"typed-other-pool").string());
            different.publish_ready(key,other);
            require(*different.load_ready(key)==other&&*typed.load_ready(key)==metadata,"cache escaped pool instance");
            typed.flush();
            SharedObjectPool cold(typed_dir.string(),false,true);
            require(*cold.load_ready(key)==metadata&&*cold.load_blob(key)==bytes,"cold typed namespace alias");
            rejects([&]{cold.publish_blob(key,bytes.data(),bytes.size());});
            rejects([&]{cold.publish_ready(key,metadata);});
        }
        {std::ofstream partial(dir/"blobs"/".tmp-partial");partial<<"bad";}
        require(!pool.load_blob(99),"temporary object visible");
        SharedObjectPool readonly(dir.string(),false,true);
        require(*readonly.load_blob(key)==bytes,"read-only load failed");rejects([&]{readonly.publish_ready(99,bytes);});readonly.flush();
        // A partially written temporary file must never become a final object.
        {
            SharedObjectPool writer((root/"partial").string());fail_write=1;
            rejects([&]{writer.publish_blob(key,bytes.data(),bytes.size());});fail_write=0;
            rejects([&]{writer.load_blob(key);});SharedObjectPool fresh((root/"partial").string(),false,true);
            require(!fresh.load_blob(key),"failed write exposed partial final object");
            for(const auto& f:std::filesystem::directory_iterator(root/"partial"/"blobs"))require(f.path().filename().string().find(".tmp-")!=0,"temporary file leaked");
        }
        // Each corruption starts with a fresh reader so a valid immutable cache
        // owner cannot accidentally hide a malformed persistent fixture.
        for(size_t offset:{size_t(0),size_t(12),size_t(16),size_t(24),size_t(32),size_t(40),size_t(48),size_t(56)}) {
            const auto location=root/("corrupt-"+std::to_string(offset));
            {SharedObjectPool writer(location.string());writer.publish_blob(key,bytes.data(),bytes.size());}
            corrupt(object(location,"blobs",key),offset);SharedObjectPool view(location.string(),false,true);
            rejects([&]{view.load_blob(key);});rejects([&]{view.load_ready(77);});
        }
        // Two 34 MiB buffers exceed the fixed 64 MiB cache, even while a caller
        // keeps an old immutable owner alive. Cold reload must validate again.
        {
            const auto location=root/"eviction";SharedObjectPool bounded(location.string());
            std::vector<uint8_t> big(size_t(34)<<20,'a');auto a=XXHash64::hash(big.data(),big.size());
            bounded.publish_blob(a,big.data(),big.size());auto retained=bounded.load_blob(a);
            std::fill(big.begin(),big.end(),'b');auto b=XXHash64::hash(big.data(),big.size());bounded.publish_blob(b,big.data(),big.size());
            require(*bounded.load_blob(b)==big,"recent cache content changed");
            corrupt(object(location,"blobs",a),0);rejects([&]{bounded.load_blob(a);});
            require(retained->front()=='a'&&retained->back()=='a',"eviction mutated retained owner");
            require(std::filesystem::file_size(object(location,"blobs",b))<big.size()/10,"large compressible object stored raw");
        }
        filesystem_barriers(root);
        pool.flush(); fail_sync=true;rejects([&]{pool.flush();});fail_sync=false;rejects([&]{pool.load_blob(key);});
        std::filesystem::remove_all(root);
        std::cout<<"PASS shared pool: atomic concurrent publication, conflicts, partial/temp, typed/hash corruption, bounded immutable cache, compression, readonly and constant filesystem barriers, device coverage and poison\n";
        return 0;
    }catch(const std::exception& error){fail_sync=false;fail_write=0;mock_devices=0;fail_syncfs=0;fail_stat=false;std::filesystem::remove_all(root);std::cerr<<error.what()<<'\n';return 1;}
}
