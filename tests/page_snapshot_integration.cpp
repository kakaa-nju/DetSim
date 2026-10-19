#include "page_snapshot.h"
#include "state_store_packed.h"
#include "debug.h"
#include "ncurses_ui.h"
#include <cstdio>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/wait.h>
static std::atomic<bool> fail_sync{false};
extern "C" int __real_fsync(int fd);
extern "C" int __wrap_fsync(int fd) {
  if (fail_sync.load()) { errno = EIO; return -1; }
  return __real_fsync(fd);
}
extern "C" detsim::ui::NCursesUI* get_ncurses_ui() { return nullptr; }
static void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
template<class F> static void rejects(F f) {
  bool rejected = false;
  try { f(); } catch (const std::exception&) { rejected = true; }
  require(rejected, "Expected explicit rejection");
}
static RegionInfo region(uint64_t start, uint64_t end, uint64_t offset) {
  RegionInfo r{}; r.start = start; r.end = end; r.offset = offset;
  r.prot = PROT_READ | PROT_WRITE; memcpy(r.flags, "rw-p", 5); return r;
}
int main(int argc, char** argv) {
  if (argc == 4 && !strcmp(argv[1], "--read")) {
    try {
      StateStore::Config cfg; cfg.read_only=true; cfg.packed_storage_path=argv[2];
      cfg.hot_cache_size=1; cfg.warm_cache_size=1;
      StateStore::instance().init(cfg);
      const auto root=std::stoull(argv[3],nullptr,16);
      auto view=SnapshotView::open(root);
      std::vector<uint8_t> output(8192);
      view->read_into(0x1003,output.data(),output.size());
      for (size_t i=0;i<output.size();++i)
        require(output[i]==uint8_t(i*37),"Fresh-process page bytes");
      StateStore::instance().shutdown();
      return 0;
    } catch (const std::exception& error) {
      fprintf(stderr,"fresh page reader: %s\n",error.what()); return 1;
    }
  }
  char directory[] = "/tmp/detsim-pages-XXXXXX";
  require(mkdtemp(directory) != nullptr, "mkdtemp");
  auto& store = StateStore::instance();
  try {
    StateStore::Config cfg; cfg.packed_storage_path = std::string(directory) + "/packed";
    cfg.hot_cache_size = 1; cfg.warm_cache_size = 1; cfg.prefetch_threads = 0;
    store.init(cfg);
    std::vector<uint8_t> bytes(8192); for (size_t i=0; i<bytes.size(); ++i) bytes[i] = i * 37;
    auto save = [&](const std::vector<uint8_t>& data) { return store.save(data.data(), data.size()); };
    // Unaligned first region, followed by adjacent region: chunks stop at each boundary.
    std::vector<RegionInfo> regions{region(0x1003, 0x2800, 0), region(0x2800, 0x3003, 2)};
    std::vector<PageRef> pages;
    uint64_t cursor = 0;
    for (auto& r : regions) {
      r.offset = pages.size();
      for (uint64_t address = r.start; address < r.end;) {
        uint32_t size = std::min<uint64_t>(r.end-address, 4096-address%4096);
        auto key = store.save(bytes.data()+cursor, size);
        pages.push_back({address,size,key}); address += size; cursor += size;
      }
    }
    std::vector<uint8_t> metadata{1,2,3}; user_regs_struct regs{}; regs.rip = 42;
    auto manifest = encode_page_manifest(regions,metadata,"maps",regs,pages);
    auto key = save(manifest); store.wait_for_completion();
    for (uint32_t unsupported : {5u,6u,7u,8u,FLAT_SNAPSHOT_VERSION-2,PAGE_SNAPSHOT_VERSION-2}) {
      auto older=manifest;
      reinterpret_cast<StateDataHeader*>(older.data())->version=unsupported;
      const auto invalid=save(older);
      rejects([&]{SnapshotView::open(invalid);});
      for(int attempt=0;attempt<2;++attempt) rejects([&]{SnapshotView::load_metadata(invalid);});
    }
    char root_argument[32]; snprintf(root_argument,sizeof(root_argument),"%llx",(unsigned long long)key);
    const auto reader=fork(); require(reader>=0,"fork fresh reader");
    if (!reader) {
      execl("/proc/self/exe","page-reader","--read",cfg.packed_storage_path.c_str(),root_argument,nullptr);
      _exit(127);
    }
    int reader_status;
    require(waitpid(reader,&reader_status,0)==reader && WIFEXITED(reader_status) &&
        WEXITSTATUS(reader_status)==0,"Fresh-process read while writer alive");
    auto view = SnapshotView::open(key);
    require(view->format_version()==PAGE_SNAPSHOT_VERSION && view->main_regs().rip==42 && view->maps_text()=="maps", "Manifest metadata");
    require(view->metadata().size()==3 && view->page_count(0)==2 && view->page_count(1)==2, "Manifest tables");
    require(*SnapshotView::load_metadata(key)==metadata,"Metadata API preserves exact saved manifest bytes");
    std::vector<uint8_t> output(bytes.size()); view->read_into(0x1003, output.data(), output.size());
    require(output==bytes, "Cross-page and adjacent-region read");
    // Pointer-page A / pointee-page B alternation uses public SnapshotView only.
    std::vector<std::vector<uint8_t>> cache_bytes(6, std::vector<uint8_t>(4096));
    std::vector<PageRef> cache_pages;
    for (size_t i=0;i<cache_bytes.size();++i) {
      for (size_t j=0;j<4096;++j) cache_bytes[i][j]=uint8_t(i*29+j*7);
      cache_pages.push_back({0x10000+i*4096,4096,save(cache_bytes[i])});
    }
    const std::vector<RegionInfo> cache_regions{region(0x10000,0x16000,0)};
    const auto cache_root=save(encode_page_manifest(cache_regions,metadata,"cache4",regs,cache_pages));
    store.wait_for_completion();
    auto cache_view=SnapshotView::open(cache_root);
    store.reset_stats();
    std::vector<uint8_t> cache_output(4096);
    for (int repeat=0;repeat<20;++repeat) {
      for (size_t index : {size_t(0),size_t(1)}) {
        cache_view->read_into(0x10000+index*4096,cache_output.data(),cache_output.size());
        require(cache_output==cache_bytes[index],"Alternating page byte equality");
      }
    }
    const auto alternating_loads=store.get_stats().load_requests;
    require(alternating_loads==2,"Alternation load_requests");
    cache_view->page(0,2); cache_view->page(0,3);
    const auto cache_pin=cache_view->page(0,0); // A hit must not change FIFO order.
    store.reset_stats();
    cache_view->page(0,4); cache_view->page(0,0); cache_view->page(0,1);
    require(store.get_stats().load_requests==3,"FIFO eviction and reload load_requests");
    require(cache_pin.length==4096 && !memcmp(cache_pin.data,cache_bytes[0].data(),4096),
        "OwnedSpan survives cache eviction");
    for (size_t i=0;i<cache_bytes.size();++i) {
      cache_view->read_into(0x10000+i*4096,cache_output.data(),cache_output.size());
      require(cache_output==cache_bytes[i],"More-than-cache-capacity byte equality");
    }
    // Reusing one key with a shorter manifest reference must fail even on hit.
    const std::vector<RegionInfo> mismatch_regions{region(0x20000,0x21000,0),region(0x22000,0x22001,1)};
    const std::vector<PageRef> mismatch_pages{{0x20000,4096,cache_pages[0].key},{0x22000,1,cache_pages[0].key}};
    auto mismatch_view=SnapshotView::open(save(encode_page_manifest(mismatch_regions,metadata,"length",regs,mismatch_pages)));
    mismatch_view->page(0,0);
    rejects([&]{mismatch_view->page(1,0);});
    printf("page-cache alternation load_requests=%llu slots=4\n",(unsigned long long)alternating_loads);

    auto pinned = view->page(0,0); view->page(1,1);
    require(pinned.data[0]==bytes[0], "Pinned page ownership");
    rejects([&]{view->read_into(0x3003,output.data(),1);});
    rejects([&]{view->page(0,2);});
    auto bad = manifest; reinterpret_cast<PageManifestHeader*>(bad.data())->page_size=8192;
    rejects([&]{SnapshotView::open(save(bad));});
    bad=manifest; reinterpret_cast<PageManifestHeader*>(bad.data())->common.tstate_size=0;
    rejects([&]{SnapshotView::open(save(bad));});
    bad=manifest; reinterpret_cast<PageManifestHeader*>(bad.data())->common.regs_offset--;
    rejects([&]{SnapshotView::open(save(bad));});
    auto wrong=pages; wrong[0].address++;
    rejects([&]{SnapshotView::open(save(encode_page_manifest(regions,metadata,"maps",regs,wrong)));});
    wrong=pages; wrong[0].key=UINT64_MAX;
    const auto missing_root=save(encode_page_manifest(regions,metadata,"maps",regs,wrong));
    auto missing=SnapshotView::open(missing_root);
    rejects([&]{missing->page(0,0);});
    require(*SnapshotView::load_metadata(missing_root)==metadata,"Metadata validates root while child pages retain lazy validation");
    wrong=pages; wrong[0].key=store.save(metadata.data(),metadata.size());
    auto shortpage=SnapshotView::open(save(encode_page_manifest(regions,metadata,"maps",regs,wrong)));
    rejects([&]{shortpage->page(0,0);});
    // Deliberately publish internally checksummed records under false content keys.
    // The view must reject both a corrupt semantic manifest key and a page key.
    store.wait_for_completion();
    auto& packed = StateStorePacked::instance();
    constexpr hash_type false_manifest = 0x123456789abcdef0ULL;
    packed.save(manifest.data(), manifest.size(), false_manifest);
    rejects([&]{SnapshotView::open(false_manifest);});
    for(int attempt=0;attempt<2;++attempt) rejects([&]{SnapshotView::load_metadata(false_manifest);});
    constexpr hash_type false_page = 0x123456789abcdef1ULL;
    packed.save(bytes.data(), pages[0].length, false_page);
    wrong=pages; wrong[0].key=false_page;
    auto corrupt=SnapshotView::open(save(encode_page_manifest(regions,metadata,"maps",regs,wrong)));
    rejects([&]{corrupt->page(0,0);});
    // Existing flat files expose the same decoded bytes without conversion.
    StateDataHeader h{}; h.magic=0x4453544D; h.version=FLAT_SNAPSHOT_VERSION; h.num_regions=regions.size();
    h.tstate_offset=sizeof(h)+regions.size()*sizeof(RegionInfo); h.tstate_size=metadata.size();
    h.maps_offset=h.tstate_offset+h.tstate_size; h.maps_size=4; h.regs_offset=h.maps_offset+4+bytes.size();
    std::vector<uint8_t> legacy(h.regs_offset+sizeof(regs)); memcpy(legacy.data(),&h,sizeof(h));
    uint64_t offset=h.maps_offset+h.maps_size;
    for(size_t i=0;i<regions.size();++i) { auto r=regions[i]; r.offset=offset; offset+=r.end-r.start; memcpy(legacy.data()+sizeof(h)+i*sizeof(r),&r,sizeof(r)); }
    memcpy(legacy.data()+h.tstate_offset,metadata.data(),metadata.size()); memcpy(legacy.data()+h.maps_offset,"maps",4);
    memcpy(legacy.data()+h.maps_offset+4,bytes.data(),bytes.size()); memcpy(legacy.data()+h.regs_offset,&regs,sizeof(regs));
    auto old=SnapshotView::open(save(legacy)); old->read_into(0x1003,output.data(),output.size());
    require(output==bytes && old->format_version()==FLAT_SNAPSHOT_VERSION,"Legacy adapter");
    store.wait_for_completion(); store.shutdown();
    rejects([&]{view->page(0,0);});
    rejects([&]{cache_view->page(0,0);});
    cfg.read_only=true; store.init(cfg); auto reopened=SnapshotView::open(key);
    reopened->read_into(0x1003,output.data(),output.size()); require(output==bytes,"Cold reopen");
    auto cold_cache_view=SnapshotView::open(cache_root);
    for (size_t i=0;i<cache_bytes.size();++i) {
      cold_cache_view->read_into(0x10000+i*4096,cache_output.data(),cache_output.size());
      require(cache_output==cache_bytes[i],"Cold reopened cache bytes");
    }
    rejects([&]{cache_view->page(0,0);});
    rejects([&]{view->metadata();}); store.shutdown();
    rejects([&]{SnapshotView::load_metadata(key);});
    // Cached flat bytes and pages must not bypass a poisoned store.
    cfg.read_only=false; cfg.hot_cache_size=65536; cfg.warm_cache_size=65536;
    store.init(cfg);
    auto large=legacy; const size_t extra=5*1024*1024;
    auto large_header=h; large_header.regs_offset+=extra;
    RegionInfo large_last; const size_t last_offset=sizeof(h)+(regions.size()-1)*sizeof(RegionInfo);
    memcpy(&large_last,large.data()+last_offset,sizeof(large_last)); large_last.end+=extra;
    large.resize(large.size()+extra);
    memcpy(large.data(),&large_header,sizeof(large_header)); memcpy(large.data()+last_offset,&large_last,sizeof(large_last));
    memcpy(large.data()+large_header.regs_offset,&regs,sizeof(regs));
    const auto large_key=save(large); store.wait_for_completion();
    const auto compact=SnapshotView::load_metadata(large_key);
    require(*compact==metadata && SnapshotView::load_metadata(large_key)==compact,"Large flat root caches an owned metadata-only slice");
    const auto legacy_key=save(legacy);
    auto warm_old=SnapshotView::open(legacy_key);
    auto warm_new=SnapshotView::open(key);
    warm_old->page(0,0); warm_new->page(0,0);
    require(SnapshotView::open(key)==warm_new, "Parsed cache not warmed");
    auto warm_cache=SnapshotView::open(cache_root);
    for (size_t i=0;i<4;++i) warm_cache->page(0,i);
    fail_sync=true;
    rejects([&]{store.wait_for_completion();});
    fail_sync=false;
    for (size_t i=0;i<4;++i) rejects([&]{warm_cache->page(0,i);});
    for (const auto& poisoned : {warm_old,warm_new}) {
      rejects([&]{poisoned->read_into(0x1003,output.data(),1);});
      rejects([&]{poisoned->page(0,0);});
      rejects([&]{poisoned->metadata();});
      rejects([&]{poisoned->maps_text();});
      rejects([&]{poisoned->regions();});
      rejects([&]{poisoned->main_regs();});
      rejects([&]{poisoned->format_version();});
      rejects([&]{poisoned->page_count(0);});
    }
    rejects([&]{SnapshotView::open(key);});
    rejects([&]{SnapshotView::open(legacy_key);});
    for(auto poisoned_key:{key,legacy_key,large_key}) rejects([&]{SnapshotView::load_metadata(poisoned_key);});
    rejects([&]{store.shutdown();});
    std::filesystem::remove_all(directory);
    puts("page_snapshot_integration: PASS (cold/tiny, flat/pages, coverage, bounds, missing/length/key integrity, lifecycle/poison)");
    return 0;
  } catch (const std::exception& e) {
    fprintf(stderr,"page_snapshot_integration: %s\n",e.what());
    try {store.shutdown();} catch (...) {}
    std::filesystem::remove_all(directory); return 1;
  }
}
