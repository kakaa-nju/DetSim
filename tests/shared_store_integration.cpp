#include "state_store.h"
#include "state_store_packed.h"
#include "sysstate_store.h"
#include "shared_object_pool.h"
#include "utils.h"
#include "ncurses_ui.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <unistd.h>
#include <sys/stat.h>
extern "C" detsim::ui::NCursesUI* get_ncurses_ui() { return nullptr; }
static std::atomic<bool> fail_pool_sync{false};
static std::mutex gate_mutex;
static std::condition_variable gate_cv;
static bool hold_compression=false,compression_entered=false;
extern "C" std::vector<uint8_t> real_compress(StateStorePacked*,const std::vector<uint8_t>&)
    asm("__real__ZN16StateStorePacked8compressERKSt6vectorIhSaIhEE");
extern "C" std::vector<uint8_t> wrap_compress(StateStorePacked*,const std::vector<uint8_t>&)
    asm("__wrap__ZN16StateStorePacked8compressERKSt6vectorIhSaIhEE");
extern "C" std::vector<uint8_t> wrap_compress(StateStorePacked* store,const std::vector<uint8_t>& bytes){
    std::unique_lock lock(gate_mutex);compression_entered=true;gate_cv.notify_all();
    gate_cv.wait(lock,[]{return !hold_compression;});lock.unlock();return real_compress(store,bytes);
}
static void release_compression(){std::lock_guard lock(gate_mutex);hold_compression=false;gate_cv.notify_all();}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
    char path[4096],descriptor[64];snprintf(descriptor,sizeof(descriptor),"/proc/self/fd/%d",fd);
    const auto size=readlink(descriptor,path,sizeof(path)-1);
    if(size>=0){path[size]=0;if(fail_pool_sync&&std::string(path).find("shared-objects")!=std::string::npos){errno=EIO;return -1;}}
    return __real_fsync(fd);
}
extern "C" int __real_syncfs(int);
extern "C" int __wrap_syncfs(int fd) {
    char path[4096],descriptor[64];snprintf(descriptor,sizeof(descriptor),"/proc/self/fd/%d",fd);
    const auto size=readlink(descriptor,path,sizeof(path)-1);
    if(size>=0){path[size]=0;if(fail_pool_sync&&std::string(path).find("shared-objects")!=std::string::npos){errno=EIO;return -1;}}
    return __real_syncfs(fd);
}
static void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
template<class F>static void rejects(F fn){bool failed=false;try{fn();}catch(const std::exception&){failed=true;}require(failed,"expected rejection");}
static StateStore::Config config(const std::filesystem::path& root,bool readonly=false){
    StateStore::Config value;value.packed_storage_path=root.string();value.read_only=readonly;
    value.hot_cache_size=65536;value.warm_cache_size=65536;value.prefetch_threads=1;return value;
}
static std::filesystem::path blob(const std::filesystem::path& root,hash_type key){char name[24];snprintf(name,sizeof(name),"%016lx.obj",static_cast<unsigned long>(key));return root/"blobs"/name;}
int main(){
    char temporary[]="/tmp/detsim-shared-store-XXXXXX";require(mkdtemp(temporary),"mkdtemp");
    const auto original=std::filesystem::current_path();
    const std::filesystem::path root(temporary),memory=root/"memory",pool=memory/"shared-objects";
    std::filesystem::current_path(root);
    SysStateStore::instance().init();
    auto& store=StateStore::instance();
    try{
        const std::vector<uint8_t> a(8192,'a'),b(8192,'b');
        store.init(config(memory));require(!store.shared_pool(),"ordinary store unexpectedly shared");
        {std::lock_guard lock(gate_mutex);hold_compression=true;}
        const auto first=store.save(a.data(),a.size());const auto generation=store.generation();
        {std::unique_lock lock(gate_mutex);require(gate_cv.wait_for(lock,std::chrono::seconds(3),[]{return compression_entered;}),"pending writer did not reach gate");}
        auto attaching=std::async(std::launch::async,[&]{store.attach_shared_pool(pool.string(),true);});
        const auto blocked=attaching.wait_for(std::chrono::milliseconds(20));
        release_compression();
        require(blocked==std::future_status::timeout,"attach returned before an accepted pending write finished");
        attaching.get();
        require(store.generation()>generation&&store.get_io_queue_size()==0,"attach did not drain and invalidate");
        require(StateStorePacked::instance().exists(first),"attach dropped local pending write");
        require(store.storage_path()==memory.string(),"storage path differs");
        require(!store.shared_pool()->load_blob(first),"attach must not pretend existing private bytes were published");
        require(store.save(a.data(),a.size(),first)==first,"reference-hint shared save differs");
        require(*store.shared_pool()->load_blob(first)==a,"hint hit failed global publication");
        const auto second=store.save(b.data(),b.size());
        require(!StateStorePacked::instance().exists(second),"shared write leaked into local packed store");
        require(store.exists(second)&&*store.load_shared(second)==b,"shared-only load/exists inconsistent");
        store.enable_prefetch(4);store.queue_push_back(second);store.queue_push_back(second);store.queue_pop_front();
        require(!store.in_preflight(second)&&store.get_stats().prefetch_issued==0,"shared write queued unsupported packed prefetch");
        store.wait_for_completion();
        const auto previous_generation=store.generation();
        store.attach_shared_pool((root/"other-pool").string(),true);
        require(store.generation()>previous_generation&&!store.shared_pool()->load_blob(second),"new pool inherited an invalid publication proof");
        require(store.save(b.data(),b.size(),second)==second&&*store.shared_pool()->load_blob(second)==b,
                "old cache/reference hit was not published in the newly attached pool");
        store.wait_for_completion();store.shutdown();
        auto cold=config(memory,true);cold.hot_cache_size=0;
        store.init(cold);require(bool(store.shared_pool()),"cold reopen did not find shared fallback");
        require(store.exists(second)&&*store.load_shared(second)==b,"cold shared-only read failed");
        store.enable_prefetch(4);store.queue_push_back(second);store.queue_push_back(second);store.queue_pop_front();
        require(!store.in_preflight(second)&&store.get_stats().prefetch_issued==0,"fallback-only key incorrectly prefetched from packed store");
        rejects([&]{store.save(b.data(),b.size(),second);});rejects([&]{store.attach_shared_pool(pool.string(),true);});
        require(*store.load_shared(second)==b,"readonly rejection poisoned legal reads");
        store.wait_for_completion();store.shutdown();
        // Both explicit barriers and shutdown include shared objects, even when
        // no local packed async writes were accepted.
        for(bool shutdown:{false,true}){
            const auto path=root/(shutdown?"shutdown":"barrier");store.init(config(path));
            store.attach_shared_pool((path/"shared-objects").string(),true);store.save(b.data(),b.size());
            fail_pool_sync=true;
            if(shutdown)rejects([&]{store.shutdown();});
            else{rejects([&]{store.wait_for_completion();});rejects([&]{store.load_shared(first);});}
            fail_pool_sync=false;
            if(!shutdown)rejects([&]{store.shutdown();});
        }
        // A cold malformed shared object poisons subsequent local/cache reads.
        chmod(blob(pool,second).c_str(),0600);
        {std::fstream file(blob(pool,second),std::ios::in|std::ios::out|std::ios::binary);char bad='!';file.write(&bad,1);}
        store.init(cold);rejects([&]{store.exists(second);});rejects([&]{store.load_shared(first);});rejects([&]{store.shutdown();});
        SysStateStore::instance().shutdown();std::filesystem::current_path(original);std::filesystem::remove_all(root);
        std::cout<<"PASS shared StateStore: pending attach, global hint publication, shared-only cold fallback, readonly, prefetch and durable barrier/shutdown poison\n";return 0;
    }catch(const std::exception& error){fail_pool_sync=false;release_compression();try{store.shutdown();}catch(...){}try{SysStateStore::instance().shutdown();}catch(...){}std::filesystem::current_path(original);std::filesystem::remove_all(root);std::cerr<<error.what()<<'\n';return 1;}
}
