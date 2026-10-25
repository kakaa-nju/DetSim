#include "state/state_view.h"
#include "state/page_snapshot.h"
#include "state_store_packed.h"
#include <fstream>
#include <thread>
#include <barrier>
#include <atomic>
#include "monitor.h"
#include "proc_status.h"
#include "dwarf_info.h"
#include "cereal/archives/binary.hpp"
#include <sstream>
#include <nlohmann/json.hpp>
#include <cereal/types/map.hpp>
#include <cereal/types/deque.hpp>
#include <fcntl.h>
#include "ncurses_ui.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>
extern "C" detsim::ui::NCursesUI* get_ncurses_ui() { return nullptr; }
static void require(bool b,const char* m) { if(!b) throw std::runtime_error(m); }
template<class F> static void rejects(F f) { bool b=false; try { f(); } catch(const std::exception&) { b=true; } require(b,"expected rejection"); }
// tests/framework.mk wraps SnapshotView::open at this target's link so the saved
// observation tests can count how often a view asks the store for a root.
static std::atomic<size_t> snapshot_open_calls{0};
extern "C" std::shared_ptr<const SnapshotView> __real__ZN12SnapshotView4openEm(uint64_t);
extern "C" std::shared_ptr<const SnapshotView> __wrap__ZN12SnapshotView4openEm(uint64_t hash) {
 ++snapshot_open_calls; return __real__ZN12SnapshotView4openEm(hash);
}
static_assert(NP >= 2, "State observation fixture needs two logical nodes");
// Construct saved observations through both real snapshot layouts, without a live tracee.
static sys_state save_serialized_fixture(const std::vector<uint8_t>& metadata, bool pages, size_t memory_size=4096, uint64_t first_word=0) {
 auto& store=StateStore::instance();
 RegionInfo region{}; region.start=0x1000; region.end=region.start+memory_size; region.prot=PROT_READ;
 memcpy(region.flags,"r--p",5); std::vector<uint8_t> memory(memory_size,0);
 if (first_word) memcpy(memory.data(),&first_word,sizeof(first_word));
 user_regs_struct regs{};
 std::vector<uint8_t> bytes;
 if (pages) {
  require(memory_size && memory_size%4096==0,"fixture memory must span complete pages");
  const auto page=store.save(memory.data(),4096);
  std::vector<PageRef> pages;
  for(size_t offset=0;offset<memory_size;offset+=4096) pages.push_back({region.start+offset,4096,page});
  bytes=encode_page_manifest({region},metadata,"",regs,pages);
 } else {
  StateDataHeader header{}; header.magic=0x4453544D; header.version=FLAT_SNAPSHOT_VERSION; header.num_regions=1;
  header.tstate_offset=sizeof(header)+sizeof(region); header.tstate_size=metadata.size();
  header.maps_offset=header.tstate_offset+header.tstate_size;
  region.offset=header.maps_offset; header.regs_offset=region.offset+memory.size();
  bytes.resize(header.regs_offset+sizeof(regs));
  memcpy(bytes.data(),&header,sizeof(header)); memcpy(bytes.data()+sizeof(header),&region,sizeof(region));
  memcpy(bytes.data()+header.tstate_offset,metadata.data(),metadata.size());
  memcpy(bytes.data()+region.offset,memory.data(),memory.size()); memcpy(bytes.data()+header.regs_offset,&regs,sizeof(regs));
 }
 sys_state machine; machine.ts_hash[0]=store.save(bytes.data(),bytes.size()); return machine;
}
static std::vector<uint8_t> serialize_fixture(const tracee_state& saved) {
 std::ostringstream encoded; { cereal::BinaryOutputArchive archive(encoded); archive(saved); }
 const auto text=encoded.str(); return {text.begin(),text.end()};
}
static sys_state save_stdio_fixture(const tracee_state& saved,bool pages,size_t memory_size=4096) {
 return save_serialized_fixture(serialize_fixture(saved),pages,memory_size);
}
static constexpr const char* observation_cache_env="DETSIM_OBSERVATION_CACHE";
// Scoped environment mutation: every test restores the exact previous value,
// including the distinction between unset and set-to-empty.
class scoped_env {
 public:
  scoped_env(const char* name,const char* value):name_(name) {
   const char* previous=std::getenv(name_);
   if(previous) {had_=true;previous_=previous;}
   if(value) require(setenv(name_,value,1)==0,"setenv failed");
   else require(unsetenv(name_)==0,"unsetenv failed");
  }
  ~scoped_env() {
   if(had_) setenv(name_,previous_.c_str(),1);
   else unsetenv(name_);
  }
  scoped_env(const scoped_env&)=delete;
  scoped_env& operator=(const scoped_env&)=delete;
 private:
  const char* name_;
  bool had_=false;
  std::string previous_;
};
template<class F> static std::string rejection_message(F f) {
 try { f(); } catch(const std::exception& e) { return e.what(); }
 throw std::runtime_error("expected rejection");
}
// Saved observations for symbol tests can need both a low raw page and pages
// holding two different layouts of the same global. Group writes by page and
// emit a valid page manifest with the region -> first-page-index mapping.
static sys_state save_mapped_fixture(const std::vector<uint8_t>& metadata,
    const std::vector<std::pair<uint64_t,long>>& values) {
 auto& store=StateStore::instance();
 std::map<uint64_t,std::vector<uint8_t>> pages;
 for(const auto& [address,value]:values) {
  const auto base=address & ~uint64_t(4095);
  auto& page=pages[base];
  if(page.empty()) page.assign(4096,0);
  const auto offset=size_t(address-base);
  require(offset+sizeof(value)<=page.size(),"fixture value crosses a saved page");
  memcpy(page.data()+offset,&value,sizeof(value));
 }
 std::vector<RegionInfo> regions; std::vector<PageRef> references;
 for(auto& [base,page]:pages) {
  RegionInfo region{}; region.start=base; region.end=base+page.size(); region.prot=PROT_READ;
  memcpy(region.flags,"r--p",5); region.offset=references.size();
  const auto key=store.save(page.data(),page.size());
  regions.push_back(region); references.push_back({base,uint32_t(page.size()),key});
 }
 user_regs_struct regs{};
 const auto manifest=encode_page_manifest(regions,metadata,"",regs,references);
 sys_state machine; machine.ts_hash[0]=store.save(manifest.data(),manifest.size()); return machine;
}
static void captured_stdio_observation(StateStore& store, const StateStore::Config& config) {
 static_assert(DETSIM_BUFFER_STDIO==6 && DETSIM_PLUGIN_ABI_V2==2);
 const auto captured=DETSIM_BUFFER_STDIO;
 auto cold=[&](const sys_state& machine) {
  store.wait_for_completion(); store.shutdown(); store.init(config); return detsim::StateView(machine);
 };
 for (bool pages:{false,true}) {
  tracee_state saved;
  {
   auto empty_view=cold(save_stdio_fixture(saved,pages)); char untouched='?';
   for(int channel=0;channel<3;++channel)
    require(empty_view.read_buffer(0,captured,channel,0,0,0,&untouched,1)==0 && untouched=='?',"existing empty captured stream is valid EOF");
  }
  std::array<std::string,3> keys;
  for(int channel=0;channel<3;++channel) {
   keys[channel]=saved.fs_state.open_files.at(channel).path;
   auto& node=saved.fs_state.filesystem.at(keys[channel]);
   node.content={static_cast<char>('0'+channel),'\0','x'}; node.metadata.st_size=3;
  }
  auto machine=save_stdio_fixture(saved,pages);
  auto view=cold(machine); char result[16];
  for(int channel=0;channel<3;++channel) {
   memset(result,'?',sizeof(result));
   require(view.read_buffer(0,captured,channel,0,0,0,result,sizeof(result))==3 &&
           result[0]=='0'+channel && result[1]=='\0' && result[2]=='x' && result[3]=='?',"captured channel exact binary bytes");
   require(view.read_buffer(0,captured,channel,0,0,1,result,1)==1 && result[0]=='\0',"captured channel offset and bound");
   require(view.read_buffer(0,captured,channel,0,0,3,nullptr,0)==0,"captured channel EOF");
  }
  for(int channel:{-1,3,999}) rejects([&]{view.read_buffer(0,captured,channel,0,0,0,result,1);});
  rejects([&]{view.read_buffer(0,captured,1,1,0,0,result,1);});
  rejects([&]{view.read_buffer(0,captured,1,0,1,0,result,1);});
  rejects([&]{view.read_buffer(0,captured,1,0,0,4,result,0);});
  rejects([&]{view.read_buffer(0,captured,1,0,0,UINT64_MAX,result,1);});
  rejects([&]{view.read_buffer(0,captured,1,0,0,0,nullptr,1);});
  rejects([&]{view.read_buffer(NP,captured,1,0,0,0,result,1);});
  tracee_state closed=saved;
  auto descriptors=std::make_shared<FdManager>(closed.fd_manager_state); closed.fs_state.set_fd_manager(descriptors);
  for(int channel=0;channel<3;++channel) require(closed.fs_state.do_close(channel)==0,"close captured descriptor");
  closed.fd_manager_state=*descriptors;
  machine=save_stdio_fixture(closed,pages);
  for(bool exited:{false,true}) {
   machine.status[0]=exited?dstatus_exit(0):0;
   auto closed_view=cold(machine);
   for(int channel=0;channel<3;++channel) {
    require(closed_view.read_buffer(0,captured,channel,0,0,0,result,sizeof(result))==3 && result[0]=='0'+channel,"captured channel remains after close/exit");
    rejects([&]{closed_view.read_buffer(0,DETSIM_BUFFER_FILE,channel,0,0,0,result,1);});
   }
  }
  tracee_state redirected=saved;
  descriptors=std::make_shared<FdManager>(redirected.fd_manager_state); redirected.fs_state.set_fd_manager(descriptors);
  require(redirected.fs_state.do_close(1)==0,"close stdout before redirection");
  require(redirected.fs_state.do_open("/replacement",O_CREAT|O_RDWR,0600)==1,"reuse stdout descriptor");
  require(redirected.fs_state.do_write(1,"replacement",11)==11,"redirected file bytes");
  redirected.fd_manager_state=*descriptors;
  auto redirected_view=cold(save_stdio_fixture(redirected,pages));
  require(redirected_view.read_buffer(0,DETSIM_BUFFER_FILE,1,0,0,0,result,sizeof(result))==11 && std::string(result,11)=="replacement","FILE follows current fd");
  require(redirected_view.read_buffer(0,captured,1,0,0,0,result,sizeof(result))==3 && result[0]=='1' && result[1]=='\0',"captured channel independent of redirected fd");
  for(int corruption=0;corruption<4;++corruption) {
   tracee_state bad=closed;
   if(corruption==0) bad.fs_state.filesystem.erase(keys[1]);
   else if(corruption==1) bad.fs_state.filesystem.at(keys[1]).metadata.st_mode=S_IFREG|0600;
   else bad.fs_state.filesystem.at(keys[1]).metadata.st_size=corruption==2?-1:4;
   auto bad_view=cold(save_stdio_fixture(bad,pages));
   memset(result,'?',sizeof(result));
   rejects([&]{bad_view.read_buffer(0,captured,1,0,0,0,result,sizeof(result));});
   rejects([&]{bad_view.read_buffer(0,captured,1,0,0,0,nullptr,0);});
   require(result[0]=='?',"invalid captured channel must not copy fabricated bytes");
  }
  sys_state absent; auto absent_view=cold(absent);
  rejects([&]{absent_view.read_buffer(0,captured,1,0,0,0,result,1);});
 }
 puts("PASS captured stdio full/pages cold observation: close/exit, binary bytes, redirection, missing/type/size/selectors");
}
static void compact_metadata_constructor(StateStore& store,const StateStore::Config& config) {
 constexpr size_t image_size=5*1024*1024;
 tracee_state saved; saved.tv={123,456}; const auto expected=serialize_fixture(saved);
 auto reopen=[&](size_t budget=0) {
  store.wait_for_completion(); store.shutdown(); auto cold=config;
  cold.hot_cache_size=budget; cold.warm_cache_size=budget; store.init(cold); store.reset_stats();
 };
 for(bool pages:{false,true}) {
  const auto machine=save_stdio_fixture(saved,pages,image_size); const auto hash=machine.ts_hash[0];
  for(size_t budget:{size_t(0),size_t(1)}) {
   reopen(budget);
   tracee_state first(hash); first.tv={999,1}; first.fd_manager_state.release_fd(1);
   first.fs_state.filesystem.at(first.fs_state.open_files.at(1).path).content={'x'};
   const tracee_state second(hash); const auto reads=store.get_stats().disk_reads;
   fprintf(stderr,"compact metadata constructor pages=%d budget=%zu root_reads=%lu\n",pages,budget,reads);
   require(second.tv.tv_sec==123 && second.tv.tv_usec==456 && second.fd_manager_state.is_allocated(1) &&
           second.fs_state.filesystem.at(second.fs_state.open_files.at(1).path).content.empty(),"metadata branches decode independently");
   require(reads==1,"large flat metadata queries repeatedly decode discarded full roots");
   require(*SnapshotView::load_metadata(hash)==expected,"owned metadata bytes equal complete snapshot bytes");
   auto view=SnapshotView::open(hash);
   require(view!=nullptr && view->main_regs().rax==0,"metadata entry must not masquerade as memory view");
   uint8_t value=0xff; view->read_into(0x1000,&value,1); require(value==0,"metadata-first memory access");
   const auto after_memory=store.get_stats().disk_reads;
   require(after_memory==2,"memory view/page requires the independent full-root or page read");
   require(tracee_state(hash).tv.tv_sec==123 && store.get_stats().disk_reads==after_memory,"memory open preserves cached metadata");
   store.shutdown(); rejects([&]{SnapshotView::load_metadata(hash);}); rejects([&]{tracee_state state(hash);});
   auto cold=config; cold.hot_cache_size=0; cold.warm_cache_size=0; store.init(cold); store.reset_stats();
   require(tracee_state(hash).tv.tv_sec==123 && store.get_stats().disk_reads==1,"metadata cache epoch after close/reopen");
  }
  auto small=save_stdio_fixture(saved,pages); reopen();
  const auto view=SnapshotView::open(small.ts_hash[0]);
  require(*SnapshotView::load_metadata(small.ts_hash[0])==expected && SnapshotView::open(small.ts_hash[0])==view,"metadata lookup must retain a small cached view");
  // Structurally valid roots may still contain invalid serialized metadata.
  auto trailing=expected; trailing.push_back(0xff);
  tracee_state invalid_io=saved; invalid_io.fd_manager_state.release_fd(1);
  for(const auto& invalid:{std::vector<uint8_t>{0},trailing,serialize_fixture(invalid_io)}) {
   const auto bad=save_serialized_fixture(invalid,pages,image_size); reopen();
   for(int attempt=0;attempt<2;++attempt) rejects([&]{tracee_state state(bad.ts_hash[0]);});
   require(store.get_stats().disk_reads==1,"cached raw bytes still require repeated archive/trailing/I-O validation");
  }
  std::vector<hash_type> large_metadata;
  tracee_state large=saved; const auto path=large.fs_state.open_files.at(1).path;
  large.fs_state.filesystem.at(path).content.assign(1536*1024,'m');
  large.fs_state.filesystem.at(path).metadata.st_size=1536*1024;
  for(int i=0;i<3;++i) {large.tv.tv_usec=i;large_metadata.push_back(save_stdio_fixture(large,pages,image_size).ts_hash[0]);}
  reopen();
  for(size_t i=0;i<2;++i) SnapshotView::load_metadata(large_metadata[i]);
  for(size_t i=0;i<2;++i) SnapshotView::load_metadata(large_metadata[i]);
  require(store.get_stats().disk_reads==2,"two metadata/view entries fit combined 4MiB budget");
  SnapshotView::load_metadata(large_metadata.back());
  require(store.get_stats().disk_reads==3,"three cold byte-budget metadata roots");
  SnapshotView::load_metadata(large_metadata.back());
  require(store.get_stats().disk_reads==3,"most recent metadata survives byte pressure");
  SnapshotView::load_metadata(large_metadata.front());
  require(store.get_stats().disk_reads==4,"combined 4MiB budget evicts oldest metadata/view");
  large.fs_state.filesystem.at(path).content.assign(4*1024*1024+1024,'b');
  large.fs_state.filesystem.at(path).metadata.st_size=4*1024*1024+1024;
  const auto oversized=save_stdio_fixture(large,pages,image_size); reopen();
  for(int i=0;i<2;++i) require(tracee_state(oversized.ts_hash[0]).tv.tv_sec==123,"oversized metadata remains valid");
  require(store.get_stats().disk_reads==2,"oversized metadata bypasses bounded cache");
 }
 // Concurrent misses publish one owned slice, not duplicate LRU keys.
 const auto shared=save_stdio_fixture(saved,false,image_size).ts_hash[0]; reopen();
 std::array<std::shared_ptr<const std::vector<uint8_t>>,8> owners;
 std::array<std::exception_ptr,8> errors; std::barrier start(8); std::vector<std::thread> threads;
 for(size_t i=0;i<owners.size();++i) threads.emplace_back([&,i] {
  start.arrive_and_wait(); try {owners[i]=SnapshotView::load_metadata(shared);} catch(...) {errors[i]=std::current_exception();}
 });
 for(auto& thread:threads) thread.join();
 for(size_t i=0;i<owners.size();++i) {if(errors[i]) std::rethrow_exception(errors[i]);require(owners[i]==owners[0] && *owners[i]==expected,"same-key metadata misses share one immutable slice");}
 std::vector<hash_type> keys;
 for(int i=0;i<257;++i) {auto entry=saved;entry.tv.tv_usec=i;keys.push_back(save_stdio_fixture(entry,false,image_size).ts_hash[0]);}
 reopen();
 for(size_t i=0;i<256;++i) SnapshotView::load_metadata(keys[i]);
 SnapshotView::load_metadata(keys.front());
 require(store.get_stats().disk_reads==256,"256 small compact entries fit existing entry cap");
 SnapshotView::load_metadata(keys.back()); require(store.get_stats().disk_reads==257,"257th cold entry");
 SnapshotView::load_metadata(keys.back()); require(store.get_stats().disk_reads==257,"entry-cap most recent survives");
 SnapshotView::load_metadata(keys[1]); require(store.get_stats().disk_reads==258,"256-entry cap evicts oldest untouched compact entry");
 // keys[2] is now evicted. A warm slice remains valid, but a miss cannot use
 // that retained data or a live image when the actual root is missing/corrupt.
 std::ifstream index(config.packed_storage_path+"/global.idx",std::ios::binary); index.exceptions(std::ios::badbit|std::ios::failbit);
 uint32_t magic,version; uint64_t count; index.read(reinterpret_cast<char*>(&magic),4);index.read(reinterpret_cast<char*>(&version),4);index.read(reinterpret_cast<char*>(&count),8);
 require(magic==0x53544149 && version==1,"fixture packed index"); PackedIndexEntry entry{}; bool found=false;
 for(uint64_t i=0;i<count;++i) {hash_type key;PackedIndexEntry candidate;index.read(reinterpret_cast<char*>(&key),8);index.read(reinterpret_cast<char*>(&candidate),sizeof(candidate));if(key==keys[2]) {entry=candidate;found=true;break;}}
 require(found,"evicted fixture root indexed"); index.close();
 const auto segment=config.packed_storage_path+"/segments/"+std::to_string(entry.segment_id)+".dat";
 std::filesystem::rename(segment,segment+".held");
 require(!SnapshotView::load_metadata(keys.front())->empty(),"warm validated slice need not rescan missing disk root");
 for(int i=0;i<2;++i) rejects([&]{SnapshotView::load_metadata(keys[2]);});
 std::filesystem::rename(segment+".held",segment);
 std::fstream bytes(segment,std::ios::binary|std::ios::in|std::ios::out); bytes.exceptions(std::ios::badbit|std::ios::failbit);
 char original;bytes.seekg(entry.offset);bytes.read(&original,1);const char corrupt=original^0xff;bytes.seekp(entry.offset);bytes.write(&corrupt,1);bytes.flush();
 for(int i=0;i<2;++i) rejects([&]{SnapshotView::load_metadata(keys[2]);});
 bytes.seekp(entry.offset);bytes.write(&original,1);bytes.close();
 require(!SnapshotView::load_metadata(keys[2])->empty(),"restored root validates after failed misses");
 store.shutdown(); store.init(config);
 puts("PASS compact snapshot metadata: full/pages, tiny/zero/cold, fresh graph, bounded single LRU, concurrency, oversized bypass, archive/I-O/lifecycle/eviction integrity");
}
static void warm_saved_observation(StateStore& store,const StateStore::Config& config) {
 tracee_state saved; const auto metadata=serialize_fixture(saved);
 constexpr long first=0x1122334455667788L, branch_value=0x0102030405060708L;
 for(bool pages:{false,true}) {
  sys_state machine=save_serialized_fixture(metadata,pages,4096,uint64_t(first));
  machine.ts_hash[1]=machine.ts_hash[0];
  detsim::StateView view(machine); long value=0;
  snapshot_open_calls=0;
  for(int attempt=0;attempt<3;++attempt) {
   view.read(0,0x1000,&value,sizeof(value)); require(value==first,"saved cold/page+full bytes");
  }
  require(snapshot_open_calls==1,"repeated reads of one saved node reuse one opened root");
  detsim::StateView copy=view;
  copy.read(0,0x1000,&value,sizeof(value));
  require(value==first && snapshot_open_calls==1,"copied observation shares the immutable pinned root");
  view.read(1,0x1000,&value,sizeof(value));
  require(value==first && snapshot_open_calls==2,"same saved source on another node opens once for that node");
  copy.read(1,0x1000,&value,sizeof(value));
  require(value==first && snapshot_open_calls==3,"copy pins a root that was cold when it was copied");
  detsim::StateView late=view;
  late.read(1,0x1000,&value,sizeof(value));
  require(value==first && snapshot_open_calls==3,"copy of a warm view reuses every pinned node");
  const sys_state other=save_serialized_fixture(metadata,pages,4096,uint64_t(branch_value));
  detsim::StateView branch(other); snapshot_open_calls=0;
  branch.read(0,0x1000,&value,sizeof(value));
  require(value==branch_value && snapshot_open_calls==1,"branch different bytes open their own root");
  view.read(0,0x1000,&value,sizeof(value));
  require(value==first && snapshot_open_calls==1,"original view never adopts branch bytes");
  sys_state missing=machine; missing.ts_hash[0]=~missing.ts_hash[0];
  detsim::StateView absent(missing); long untouched=0x55; snapshot_open_calls=0;
  for(int attempt=0;attempt<2;++attempt)
   rejects([&]{absent.read(0,0x1000,&untouched,sizeof(untouched));});
  require(untouched==0x55 && snapshot_open_calls==2,"missing uncached root rejects without fabricating bytes");
  sys_state no_root; detsim::StateView disconnected(no_root);
  const auto without_root=snapshot_open_calls.load();
  rejects([&]{disconnected.read(0,0x1000,&untouched,sizeof(untouched));});
  require(snapshot_open_calls==without_root && untouched==0x55,"absent hash is rejected before opening a root");
  view.read(0,0x1000,&value,sizeof(value));
  store.wait_for_completion(); store.shutdown();
  const auto pinned=snapshot_open_calls.load();
  rejects([&]{view.read(0,0x1000,&value,sizeof(value));});
  rejects([&]{copy.read(0,0x1000,&value,sizeof(value));});
  require(snapshot_open_calls==pinned,"closed store rejects before using a pinned root");
  store.init(config);
  rejects([&]{view.read(0,0x1000,&value,sizeof(value));});
  require(snapshot_open_calls==pinned,"reopened store invalidates the old observation before its pinned root");
  detsim::StateView fresh(machine); snapshot_open_calls=0;
  fresh.read(0,0x1000,&value,sizeof(value));
  require(value==first && snapshot_open_calls==1,"fresh view after reopen opens the root once");
  // A retained root must not bypass store poisoning just because its bytes are
  // already warm. Attaching a missing read-only pool is a real failure path.
  rejects([&]{store.attach_shared_pool(config.packed_storage_path+"/absent-pool",false);});
  const auto before_poison_read=snapshot_open_calls.load();
  rejects([&]{fresh.read(0,0x1000,&value,sizeof(value));});
  require(snapshot_open_calls==before_poison_read,"poisoned view rejects before reusing its pinned root");
  rejects([&]{store.shutdown();});
  store.init(config);
 }
 puts("PASS warm saved observation: one open per node/view, copy isolation, branch bytes, missing roots, lifecycle invalidation");
}
// stopped_live reads must bypass SnapshotView entirely: the pid slot is faked to
// this test process only for the duration of the check and restored by RAII.
static void live_observation_reads_fresh() {
 scoped_env cached(observation_cache_env,"on");
 detsim::clear_observation_memo();
 struct restore_pid { int& slot; int saved; restore_pid(int& target):slot(target),saved(target){} ~restore_pid(){slot=saved;} };
 restore_pid restore(ptmc_state.pids[0]); ptmc_state.pids[0]=getpid();
 auto live=detsim::StateView::stopped_live();
 volatile long cell=111; long observed=0;
 const auto address=reinterpret_cast<uint64_t>(&cell);
 snapshot_open_calls=0;
 live.read(0,address,&observed,sizeof(observed));
 require(observed==111 && snapshot_open_calls.load()==0,"live read uses guest memory without opening a saved root");
 const auto expression="*(long*)"+std::to_string(address);
 require(live.evaluate(0,expression)==111,"live evaluate reads the caller's current memory");
 const auto live_stats=detsim::observation_memo_stats();
 require(live_stats.lookups==0 && live_stats.hits==0 && live_stats.roots==0,"live evaluate never consults the saved memo");
 cell=222;
 live.read(0,address,&observed,sizeof(observed));
 require(observed==222 && snapshot_open_calls.load()==0,"live read sees changed bytes without a cached root");
 require(live.evaluate(0,expression)==222,"live evaluate sees changed bytes and no retained observation");
 require(detsim::observation_memo_stats().lookups==0 && detsim::observation_memo_stats().hits==0,"live evaluate keeps memo counters untouched");
 {
  scoped_env invalid(observation_cache_env,"bogus");
  auto unaffected=detsim::StateView::stopped_live();
  require(unaffected.evaluate(0,expression)==222,"live observation ignores irrelevant memo configuration");
 }
 puts("PASS stopped_live reads/evaluations stay fresh, bypass the memo, and never open a saved root");
}
// Repeated observations of one saved node must reuse the validated value while
// fresh/copied views, branch roots, process qualifiers and a live evaluation all
// keep their distinct provenance. Values are checked, not only counters.
static void saved_observation_memo_raw(StateStore&, const StateStore::Config&, const char* mode) {
 scoped_env env(observation_cache_env,mode);
 require(std::getenv(observation_cache_env) && std::string(std::getenv(observation_cache_env))==mode,"explicit observation cache mode");
 auto stats=[] { return detsim::observation_memo_stats(); };
 const auto metadata=serialize_fixture(tracee_state{});
 constexpr long first=0x11223344L, other=0x01020304L, changed=0x11112222L;
 const std::string expression="*(int*)0x1000";
 detsim::clear_observation_memo();
 {
  // Default/unset mode keeps the original evaluator path, including no counters.
  scoped_env unset(observation_cache_env,nullptr);
  const auto off_root=save_serialized_fixture(metadata,true,4096,7777);
  detsim::StateView view(off_root);
  require(view.evaluate(0,expression)==7777,"default cache mode evaluates saved memory");
  const auto s=stats();
  require(s.lookups==0 && s.hits==0 && s.misses==0 && s.admitted==0 && s.roots==0,"default off mode does not memoize");
 }
 {
  scoped_env invalid(observation_cache_env,"bogus");
  const auto invalid_root=save_serialized_fixture(metadata,true,4096,8888);
  const auto error=rejection_message([&]{
   detsim::StateView view(invalid_root); view.evaluate(0,expression);
  });
  require(error=="DETSIM_OBSERVATION_CACHE must be off, on or verify","invalid cache mode has an explicit error");
 }
 detsim::clear_observation_memo();
 const auto root_a=save_serialized_fixture(metadata,true,4096,uint64_t(first));
 const auto root_b=save_serialized_fixture(metadata,false,4096,uint64_t(other));
 const auto root_b2=save_serialized_fixture(metadata,true,4096,uint64_t(changed));
 {
  scoped_env off(observation_cache_env,"off");
  detsim::StateView off_view(root_a);
  {
   scoped_env on(observation_cache_env,"on");
   detsim::clear_observation_memo();
   require(off_view.evaluate(0,expression)==first && stats().lookups==0,
           "existing view retains its off mode after environment change");
   detsim::StateView on_view(root_a);
   require(on_view.evaluate(0,expression)==first,"new view observes enabled memo mode");
   scoped_env off_again(observation_cache_env,"off");
   auto copy=on_view;
   require(copy.evaluate(0,expression)==first && stats().hits==1,
           "copied view retains the captured on mode");
   const auto lookups=stats().lookups;
   detsim::StateView new_off(root_a);
   require(new_off.evaluate(0,expression)==first && stats().lookups==lookups,
           "new off view does not inherit another view's mode");
  }
 }
 {
  detsim::clear_observation_memo();
  ExprCache::instance().clear();
  detsim::StateView view(root_a);
  std::string storage="prefix"+expression+" + 991";
  const std::string_view slice(storage.data()+6,expression.size());
  require(view.evaluate(0,slice)==first,"borrowed expression slice must not parse its suffix");
  const auto before=stats();
  storage.assign(storage.size(),'x');
  require(view.evaluate(0,expression)==first && stats().hits==before.hits+1,
          "memo and parsed expression keys must own borrowed text");
 }
 detsim::clear_observation_memo();
 {
  detsim::StateView view(root_a); snapshot_open_calls=0;
  require(view.evaluate(0,expression)==first,"memo miss evaluates the saved value");
  auto s=stats();
  require(s.lookups==1 && s.hits==0 && s.misses==1 && s.admitted==1 && s.nonlocal==0 && s.roots==1,"first local observation is admitted");
  require(snapshot_open_calls==1,"first observation opens one root");
  require(view.evaluate(0,expression)==first,"repeated same-node value is reused");
  s=stats();
  require(s.lookups==2 && s.hits==1 && s.misses==1 && s.admitted==1 && s.roots==1,"second observation is a hit");
  require(snapshot_open_calls==1,"memo hit does not open the root again");
  detsim::StateView copy=view;
  require(copy.evaluate(0,expression)==first,"copied StateView reuses the same observation");
  s=stats();
  require(s.lookups==3 && s.hits==2 && s.roots==1,"copied view contributes a hit");
  detsim::StateView fresh(root_a);
  require(fresh.evaluate(0,expression)==first,"fresh StateView reuses the same saved root");
  s=stats();
  require(s.lookups==4 && s.hits==3 && s.roots==1,"fresh view shares the retained observation");
  require(snapshot_open_calls==1,"fresh/copied views do not open the retained owner");
  detsim::clear_observation_memo();
  require(view.evaluate(0,expression)==first,"clear memo recomputes the same value");
  s=stats();
  require(s.lookups==1 && s.hits==0 && s.misses==1 && s.admitted==1 && s.roots==1,"clear resets counters and warms a new entry");
  require(snapshot_open_calls==1,"cleared memo still uses the view's pinned root");
 }
 {
  // Roots with different bytes must never share a memo entry.
  detsim::clear_observation_memo();
  detsim::StateView first_view(root_a), other_view(root_b); snapshot_open_calls=0;
  require(first_view.evaluate(0,expression)==first,"first branch value");
  require(other_view.evaluate(0,expression)==other,"other branch value");
  require(first_view.evaluate(0,expression)==first,"branch isolation returns the original bytes");
  const auto s=stats();
  require(s.lookups==3 && s.hits==1 && s.misses==2 && s.admitted==2 && s.nonlocal==0 && s.roots==2,"two roots keep separate retained observations");
  require(snapshot_open_calls==2,"each distinct root opens once");
 }
 {
  // A process-qualified read from node0 depends on node1's memory. Reusing the
  // node0 root must not freeze node1's changing bytes.
  sys_state qualified=root_a; qualified.ts_hash[1]=root_b.ts_hash[0];
  sys_state qualified2=root_a; qualified2.ts_hash[1]=root_b2.ts_hash[0];
  detsim::clear_observation_memo();
  {
   detsim::StateView before(qualified), after(qualified2);
   const std::string mixed="*(int*)0x1000 + tracee1(*(int*)0x1000)";
   require(before.evaluate(0,mixed)==first+other,"mixed observation sees both nodes");
   require(after.evaluate(0,mixed)==first+changed,
           "mixed observation must see a changed peer despite unchanged local root");
  }
  detsim::clear_observation_memo();
  detsim::StateView view(qualified);
  const std::string cross="tracee1(*(int*)0x1000)";
  require(view.evaluate(0,cross)==other,"qualified read sees node1");
  auto s=stats();
  require(s.lookups==1 && s.hits==0 && s.misses==1 && s.admitted==0 && s.nonlocal==1 && s.roots==0,"cross-node memory read is nonlocal and not admitted");
  detsim::StateView changed_view(qualified2);
  require(changed_view.evaluate(0,cross)==changed,"node1 change is visible through an unchanged node0 root");
  s=stats();
  require(s.lookups==2 && s.hits==0 && s.misses==2 && s.admitted==0 && s.nonlocal==2 && s.roots==0,"changed cross-node read neither hits nor admits");
  require(view.evaluate(0,"tracee1(*(int*)0x1000) ? 7 : 9")==7,"conditional reading node1 sees its value");
  s=stats();
  require(s.lookups==3 && s.hits==0 && s.nonlocal==3 && s.admitted==0 && s.roots==0,"condition on node1 is nonlocal and not memoized");
 }
 {
  // Only the selected branch's dependencies matter: an inactive cross-node
  // branch is safe to memoize because it contributes no reads.
  detsim::clear_observation_memo();
  sys_state qualified=root_a; qualified.ts_hash[1]=root_b.ts_hash[0];
  detsim::StateView view(qualified);
  const std::string inactive="1 ? *(int*)0x1000 : tracee1(*(int*)0x1000)";
  require(view.evaluate(0,inactive)==first,"inactive cross-node branch returns the local value");
  auto s=stats();
  require(s.lookups==1 && s.hits==0 && s.misses==1 && s.admitted==1 && s.nonlocal==0 && s.roots==1,"inactive branch admits a local observation");
  require(view.evaluate(0,inactive)==first,"inactive branch observation is reused");
  s=stats();
  require(s.lookups==2 && s.hits==1 && s.admitted==1 && s.nonlocal==0,"inactive branch is a safe hit");
 }
 {
  // The memo key includes the requested node, not just the cached root. Warm
  // tracee0 from node0=A, then ask node1=A while node0 holds B.
  detsim::clear_observation_memo();
  sys_state qualified=root_a; qualified.ts_hash[1]=root_b.ts_hash[0];
  detsim::StateView view(qualified);
  const std::string alias="tracee0(*(int*)0x1000)";
  require(view.evaluate(0,alias)==first,"tracee0 on node0 sees node0");
  auto s=stats();
  require(s.lookups==1 && s.hits==0 && s.misses==1 && s.admitted==1 && s.nonlocal==0 && s.roots==1,"qualified local read is admitted under the requested node");
  sys_state reversed; reversed.ts_hash[0]=root_b.ts_hash[0]; reversed.ts_hash[1]=root_a.ts_hash[0];
  detsim::StateView reversed_view(reversed);
  require(reversed_view.evaluate(1,alias)==other,"alias trap returns node0 B from a node1=A view");
  s=stats();
  require(s.lookups==2 && s.hits==0 && s.misses==2 && s.admitted==1 && s.nonlocal==1 && s.roots==1,"alias trap must miss the node0 cache entry");
 }
 {
  // A cold/missing root must reject; it must never borrow live memory.
  detsim::clear_observation_memo();
  sys_state missing; missing.ts_hash[0]=0xDEADBEEFCAFEBABEull;
  detsim::StateView view(missing);
  const auto before=stats();
  const auto error=rejection_message([&]{view.evaluate(0,expression);});
  require(error=="Missing state snapshot","missing saved root has the store error");
  auto s=stats();
  require(s.lookups==before.lookups+1 && s.misses==before.misses+1 && s.hits==before.hits && s.admitted==before.admitted,"missing root retains no observation");
  sys_state absent; detsim::StateView absent_view(absent);
  const auto before_absent=stats();
  const auto absent_error=rejection_message([&]{absent_view.evaluate(0,expression);});
  require(absent_error=="Observation has no saved process snapshot","absent node is rejected before opening a root");
  s=stats();
  require(s.lookups==before_absent.lookups && s.hits==before_absent.hits && s.admitted==before_absent.admitted,"absent node bypasses the memo counters");
 }
 {
  // Bounded roots: a new root evicts the oldest, whose value must be reloaded
  // from its own saved bytes rather than returned from a stale owner.
  detsim::clear_observation_memo();
  std::vector<sys_state> roots; std::vector<long> values;
  for(int i=0;i<17;++i) { values.push_back(0x1000+i); roots.push_back(save_serialized_fixture(metadata,true,4096,uint64_t(values.back()))); }
  for(size_t i=0;i<roots.size();++i) { detsim::StateView view(roots[i]); require(view.evaluate(0,expression)==values[i],"each distinct root evaluates its own value"); }
  auto s=stats();
  require(s.lookups==17 && s.hits==0 && s.misses==17 && s.admitted==17 && s.roots==16,"memo bounds roots at sixteen");
  detsim::StateView oldest(roots.front());
  require(oldest.evaluate(0,expression)==values.front(),"evicted root reloads its own value");
  s=stats();
  require(s.lookups==18 && s.hits==0 && s.misses==18 && s.admitted==18 && s.roots==16,"evicted root is a miss and stays bounded");
 }
 {
  // A root larger than the admission bound still evaluates but never enters
  // the memo, so repeated observations keep taking the original path.
  detsim::clear_observation_memo();
  constexpr long large_value=0x51525354L;
  const auto large_root=save_serialized_fixture(metadata,false,128*1024,uint64_t(large_value));
  detsim::StateView view(large_root);
  require(view.evaluate(0,expression)==large_value,"oversized root evaluates");
  auto s=stats();
  require(s.lookups==1 && s.hits==0 && s.misses==1 && s.admitted==0 && s.roots==0,"oversized root is not admitted");
  require(view.evaluate(0,expression)==large_value,"oversized root evaluates again");
  s=stats();
  require(s.lookups==2 && s.hits==0 && s.misses==2 && s.admitted==0 && s.roots==0,"oversized root never creates a hit");
 }
 {
  // The expression bound bypasses memo bookkeeping entirely and preserves the
  // ordinary evaluator result.
  detsim::clear_observation_memo();
  const auto padded_root=save_serialized_fixture(metadata,true,4096,uint64_t(first));
  detsim::StateView view(padded_root);
  std::string oversized(1100,' '); oversized+=expression;
  require(oversized.size()>1024,"oversized expression fixture");
  require(view.evaluate(0,oversized)==first,"oversized expression falls back to the evaluator");
  const auto s=stats();
  require(s.lookups==0 && s.hits==0 && s.misses==0 && s.admitted==0 && s.roots==0,"oversized expression does not touch the memo");
 }
 {
  // A local memory read followed by a failing RHS must not admit anything and
  // must restore the dependency recorder for the next observation.
  detsim::clear_observation_memo();
  const auto failure_root=save_serialized_fixture(metadata,true,4096,uint64_t(first));
  detsim::StateView view(failure_root);
  const std::string failing="*(int*)0x1000 + 1/0";
  const auto before=stats();
  const auto error=rejection_message([&]{view.evaluate(0,failing);});
  require(error=="Invalid arithmetic in observation expression","failed RHS reports the evaluator error");
  auto s=stats();
  require(s.lookups==before.lookups+1 && s.misses==before.misses+1 && s.hits==before.hits && s.admitted==before.admitted && s.roots==before.roots,"failed observation is not admitted");
  require(view.evaluate(0,expression)==first,"recorder is restored for the next observation");
  s=stats();
  require(s.lookups==before.lookups+2 && s.misses==before.misses+2 && s.admitted==before.admitted+1 && s.roots==before.roots+1,"next good observation is admitted");
  require(view.evaluate(0,expression)==first,"next good observation is reused");
  require(stats().hits==before.hits+1,"recorder restoration enables a hit");
 }
 detsim::clear_observation_memo();
 puts("PASS observation memo raw: off/on/verify, same-node hits, fresh/copy, branches, qualifiers, alias, missing, eviction, bounds");
}

// Symbol context: a local memo hit must be invalidated by cleanup/reload and by
// unrelated lazy type insertion. A symbol-only cross-node check must never make
// an expression local, even when its raw memory read is local.
static void saved_observation_memo_symbols(StateStore&, const StateStore::Config& config, const char* mode,
                                           const char* old_fixture, const char* new_fixture) {
 scoped_env env(observation_cache_env,mode);
 auto stats=[] { return detsim::observation_memo_stats(); };
 const std::string temp=config.packed_storage_path+"-observation-fixture";
 struct temp_guard {
  const std::string& path;
  ~temp_guard() {std::error_code ignored; std::filesystem::remove(path,ignored); std::filesystem::remove(path+".new",ignored);}
 } cleanup{temp};
 struct restore_exec {
  char*& slot; char* saved;
  restore_exec(char*& target):slot(target),saved(target){}
  ~restore_exec() {slot=saved;}
 } restore0(ptmc_state.tracee[0].executable), restore1(ptmc_state.tracee[1].executable);
 struct dwarf_guard { ~dwarf_guard() {dwarf_cleanup();} } symbol_scope;
 dwarf_cleanup();
 std::filesystem::copy_file(old_fixture,temp,std::filesystem::copy_options::overwrite_existing);
 ptmc_state.tracee[0].executable=const_cast<char*>(temp.c_str());
 ptmc_state.tracee[1].executable=const_cast<char*>(temp.c_str());
 dwarf_init(temp.c_str());
 require(dwarf_fixed_executable(),"symbol fixture is a fixed executable");
 const auto old_address=dwarf_get_global_addr("fixture_record"); require(old_address,"old fixture global");
 const auto old_offset=dwarf_member_offset("FixtureRecord","total");
 const auto old_size=dwarf_type_size("FixtureRecord");
 require(old_offset==8 && old_size==16,"old fixture total offset/size");
 struct RecordAddress final : ExprNode {
  EvalResult eval(int, bool& ok) const override {ok=true; EvalResult r(uint64_t{0x1000});r.type_name="FixtureRecord";return r;}
  EvalResult eval_address(int pid, bool& ok) const override {return eval(pid,ok);}
  std::string to_string() const override {return "record-address";}
 };
 MemberAccessNode retained_member(MemberOp::DOT,new RecordAddress,"total");
 bool member_ok=true;
 require(retained_member.eval_address(0,member_ok).as_address()==0x1008 && member_ok,
         "retained AST uses original member layout");
 dwarf_cleanup();
 dwarf_init(new_fixture);
 const auto new_address=dwarf_get_global_addr("fixture_record"); require(new_address,"changed fixture global");
 const auto new_offset=dwarf_member_offset("FixtureRecord","total");
 const auto new_size=dwarf_type_size("FixtureRecord");
 require(new_offset==16 && new_size==24,"changed fixture total offset/size");
 dwarf_cleanup();
 const auto metadata=serialize_fixture(tracee_state{});
 auto machine=save_mapped_fixture(metadata,{{0x1000,1000},{old_address+uint64_t(old_offset),123},{new_address+uint64_t(new_offset),456}});
 machine.ts_hash[1]=machine.ts_hash[0];
 dwarf_init(temp.c_str());
 detsim::StateView view(machine);
 detsim::clear_observation_memo();
 // Warm the type-size and member-offset caches through the public evaluator so
 // the first admitted observation sees a stable symbol context.
 require(view.evaluate(0,"sizeof(FixtureRecord)")==old_size,"old type size through evaluator");
 require(view.evaluate(0,"offsetof(FixtureRecord,total)")==old_offset,"old member offset through evaluator");
 detsim::clear_observation_memo();
 const std::string total="fixture_record.total";
 require(view.evaluate(0,total)==123,"saved old layout value");
 for(int attempt=0;attempt<8 && stats().roots==0;++attempt) require(view.evaluate(0,total)==123,"saved old layout warms");
 auto s=stats();
 require(s.admitted>=1 && s.roots==1,"old layout observation is retained");
 const auto hits=s.hits;
 require(view.evaluate(0,total)==123,"old layout observation is reused");
 require(stats().hits==hits+1,"old layout observation hits");
 // An unrelated lazy type insertion must not leave a stale hit behind.
 const auto hits_before_lazy=stats().hits;
 { const auto lazy_size=dwarf_type_size("FixtureOther"); require(lazy_size==16,("lazy type insertion size="+std::to_string(lazy_size)).c_str()); }
 require(view.evaluate(0,total)==123,"value survives lazy type insertion");
 require(stats().hits==hits_before_lazy,"lazy type insertion invalidates retained observation");
 for(int attempt=0;attempt<4 && stats().roots==0;++attempt) require(view.evaluate(0,total)==123,"re-admit after lazy insertion");
 require(stats().roots==1,"lazy insertion leaves a fresh observation");
 // Symbol-only dependency on node1: the raw read is local, so only the
 // recorded symbol check can mark this expression nonlocal.
 const std::string offset_cross="*(long*)0x1000 + tracee1(offsetof(FixtureRecord,total))";
 const std::string size_cross="*(long*)0x1000 + tracee1(sizeof(FixtureRecord))";
 const auto before_cross=stats();
 require(view.evaluate(0,offset_cross)==1008,"cross-node offset uses node1 symbols and local memory");
 require(view.evaluate(0,size_cross)==1016,"cross-node size uses node1 symbols and local memory");
 s=stats();
 require(s.hits==before_cross.hits && s.admitted==before_cross.admitted,"symbol-only cross-node reads are not admitted");
 {
  // Node1's configured executable changes while node0/root and the loaded
  // DWARF stay the same. The cross-node symbol dependency must reject, never
  // reuse the earlier local-looking success.
  char* saved_executable=ptmc_state.tracee[1].executable;
  ptmc_state.tracee[1].executable=const_cast<char*>(old_fixture);
  detsim::StateView mismatched(machine);
  const auto hits_before=stats().hits;
  const auto admitted_before=stats().admitted;
  const auto error=rejection_message([&]{mismatched.evaluate(0,size_cross);});
  require(error=="Observation symbols do not match the configured node executable","changed node1 executable rejects the cross-node symbol dependency");
  s=stats();
  require(s.hits==hits_before && s.admitted==admitted_before,"mismatched node1 context is not served from cache");
  ptmc_state.tracee[1].executable=saved_executable;
 }
 // Same saved root, expression and configured path: only the file behind the
 // path changes. Rename is atomic and happens with no mapped DWARF open.
 const auto before_reload=stats();
 dwarf_cleanup();
 std::filesystem::copy_file(new_fixture,temp+".new",std::filesystem::copy_options::overwrite_existing);
 std::filesystem::rename(temp+".new",temp);
 dwarf_init(temp.c_str());
 require(retained_member.eval_address(0,member_ok).as_address()==0x1010 && member_ok,
         "retained AST must refresh member layout after symbol reload");
 require(view.evaluate(0,total)==456,"same path/root/expression reloads the changed layout");
 require(stats().hits==before_reload.hits,"symbol reload must miss the retained old-layout observation");
 require(view.evaluate(0,offset_cross)==1016,"cross-node offset reloads the changed layout");
 require(view.evaluate(0,size_cross)==1024,"cross-node size reloads the changed layout");
 require(view.evaluate(0,"*(long*)0x1000 + sizeof(FixtureRecord)")==1024,"local type size reloads the changed layout");
 puts("PASS observation memo symbols: lazy insertion, cross-node sizeof/offsetof, atomic same-path reload");
}

// Store lifetime is part of the retained observation's validity. A warm memo
// may keep derived values only while the store generation that validated them
// remains open; a closed, reopened or poisoned store rejects before any hit.
static void saved_observation_memo_lifecycle(StateStore& store, const StateStore::Config& config, const char* mode) {
 scoped_env env(observation_cache_env,mode);
 auto stats=[] { return detsim::observation_memo_stats(); };
 const auto metadata=serialize_fixture(tracee_state{});
 constexpr long value=0x10203040L;
 const auto root=save_serialized_fixture(metadata,true,4096,uint64_t(value));
 const std::string expression="*(int*)0x1000";
 detsim::clear_observation_memo();
 detsim::StateView view(root); snapshot_open_calls=0;
 require(view.evaluate(0,expression)==value,"warm value before lifecycle");
 require(view.evaluate(0,expression)==value && stats().hits==1,"warm memo hit before lifecycle");
 store.wait_for_completion(); store.shutdown();
 const std::string closed_error=rejection_message([&]{view.evaluate(0,expression);});
 require(closed_error=="Observation belongs to a closed or failed snapshot store","closed store rejects a stale view");
 store.init(config);
 const std::string reopened_error=rejection_message([&]{view.evaluate(0,expression);});
 require(reopened_error=="Observation belongs to a closed or failed snapshot store","reopened store rejects the old view");
 detsim::clear_observation_memo();
 detsim::StateView fresh(root);
 require(fresh.evaluate(0,expression)==value,"fresh view after reopen reads the saved root");
 // Poisoning must not be bypassed by retained bytes in the memo.
 rejects([&]{store.attach_shared_pool(config.packed_storage_path+"/absent-observation-pool",false);});
 const std::string poisoned_error=rejection_message([&]{fresh.evaluate(0,expression);});
 require(poisoned_error=="Observation belongs to a closed or failed snapshot store","poisoned store rejects its stale view");
 try {store.shutdown();} catch(...) {}
 store.init(config);
 detsim::clear_observation_memo();
 detsim::StateView recovered(root);
 require(recovered.evaluate(0,expression)==value,"recovered store serves a fresh observation");
 puts("PASS observation memo lifecycle: shutdown, reopen and poison reject stale retained observations");
}

// Retained observations are derived data. Removing the backing store may keep
// an already validated memo hit usable, but a fresh miss must reject instead of
// substituting a cached/held projection as if it were saved memory.
static void saved_observation_memo_backing(StateStore& store, const StateStore::Config& config, const char* mode) {
 scoped_env env(observation_cache_env,mode);
 auto stats=[] { return detsim::observation_memo_stats(); };
 const auto metadata=serialize_fixture(tracee_state{});
 constexpr long a_value=0x0A0B0C0DL, b_value=0x01020304L;
 const std::string expression="*(int*)0x1000";
 detsim::clear_observation_memo();
 const auto root_a=save_serialized_fixture(metadata,true,4096,uint64_t(a_value));
 const auto root_b=save_serialized_fixture(metadata,true,4096,uint64_t(b_value));
 detsim::StateView evicted(root_b);
 require(evicted.evaluate(0,expression)==b_value,"second root warms");
 detsim::StateView retained(root_a);
 require(retained.evaluate(0,expression)==a_value,"retained root warms");
 require(retained.evaluate(0,expression)==a_value && stats().hits==1,"retained root has a hit");
 // Warm B first, then A: fifteen fillers evict B (oldest) and leave A retained.
 for(int i=0;i<15;++i) {
  const auto filler=save_serialized_fixture(metadata,true,4096,uint64_t(0x2000+i));
  detsim::StateView view(filler);
  require(view.evaluate(0,expression)==0x2000+i,"filler root evaluates");
 }
 auto s=stats();
 require(s.roots==16,"memo is full and B was evicted");
 // Push A and B out of the process-wide SnapshotView cache so a fresh miss
 // must reach the backing store rather than the parsed-view cache.
 for(int i=0;i<260;++i) {
  const auto filler=save_serialized_fixture(metadata,true,4096,uint64_t(0x3000+i));
  const auto opened=SnapshotView::open(filler.ts_hash[0]);
  require(bool(opened),"cache filler snapshot opens");
 }
 store.wait_for_completion();
 StateStorePacked::instance().shutdown();
 std::error_code remove_error;
 std::filesystem::remove_all(config.packed_storage_path,remove_error);
 require(!remove_error,"remove packed backing");
 const auto hits_before=stats().hits;
 require(retained.evaluate(0,expression)==a_value,"validated memo hit survives backing removal");
 require(stats().hits==hits_before+1,"backing removal does not invalidate the retained hit");
 // B is no longer memoized and its parsed view is no longer cached: this is a
 // genuine miss and must not return B's removed bytes or any held projection.
 detsim::StateView cold(root_b);
 const auto cold_hits=stats().hits;
 const auto cold_admitted=stats().admitted;
 const auto error=rejection_message([&]{cold.evaluate(0,expression);});
 require(error=="packed storage: store unavailable","fresh miss after backing removal rejects");
 s=stats();
 require(s.hits==cold_hits && s.admitted==cold_admitted,"fresh miss does not substitute a held observation");
 try {store.shutdown();} catch(...) {}
 store.init(config);
 detsim::clear_observation_memo();
 const auto recovered=save_serialized_fixture(metadata,true,4096,uint64_t(0x04050607L));
 detsim::StateView recovered_view(recovered);
 require(recovered_view.evaluate(0,expression)==0x04050607L,"store recovers after backing removal");
 puts("PASS observation memo backing: retained hit survives, evicted fresh miss rejects");
}

int main(int argc, char** argv) {
 require(argc==4,"expected two distinct DWARF binaries and changed-layout fixture");
 char directory[]="/tmp/detsim-view-XXXXXX";
 require(mkdtemp(directory),"mkdtemp");
 try {
  StateStore::Config cfg; cfg.packed_storage_path=std::string(directory)+"/packed"; cfg.hot_cache_size=1; cfg.warm_cache_size=1; cfg.prefetch_threads=0;
  auto& store=StateStore::instance(); store.init(cfg);
  captured_stdio_observation(store,cfg);
  compact_metadata_constructor(store,cfg);
  warm_saved_observation(store,cfg);
  live_observation_reads_fresh();
  const bool had_cache_env=std::getenv(observation_cache_env)!=nullptr;
  const std::string initial_cache_env=had_cache_env?std::getenv(observation_cache_env):"";
  for(const char* mode:{"on","verify"}) {
   saved_observation_memo_raw(store,cfg,mode);
   saved_observation_memo_symbols(store,cfg,mode,argv[1],argv[3]);
   saved_observation_memo_lifecycle(store,cfg,mode);
   saved_observation_memo_backing(store,cfg,mode);
   require((std::getenv(observation_cache_env)!=nullptr)==had_cache_env && (!had_cache_env || initial_cache_env==std::getenv(observation_cache_env)),"scoped environment is restored");
  }
  RegionInfo r{}; r.start=0x1000; r.end=0x2000; r.prot=PROT_READ|PROT_WRITE; memcpy(r.flags,"rw-p",5);
  std::vector<uint8_t> bytes(4096); long value=123; memcpy(bytes.data(),&value,sizeof(value));
  auto page=store.save(bytes.data(),bytes.size());
  tracee_state saved; saved.tv={12,34};
  auto descriptors=std::make_shared<FdManager>(saved.fd_manager_state);
  saved.fs_state.set_fd_manager(descriptors);
  int file_fd=saved.fs_state.do_open("/captured",O_CREAT|O_RDWR|O_NONBLOCK|O_CLOEXEC,0600);
  require(file_fd>=0,"captured file"); require(saved.fs_state.do_write(file_fd,"abc",3)==3,"captured file bytes");
  int pipe_fds[2]; require(saved.fs_state.do_pipe(pipe_fds,O_NONBLOCK|O_CLOEXEC)==0,"captured pipe");
  const int pipe_id=saved.fs_state.pipe_fds_.at(pipe_fds[0]); saved.fs_state.pipes_.at(pipe_id).buffer={'p','i','p','e'};
  const int tcp_fd=descriptors->allocate_fd(FdType::SOCKET,O_RDWR|O_NONBLOCK,FD_CLOEXEC);
  const int udp_fd=descriptors->allocate_fd(FdType::SOCKET,O_RDWR,0);
  const int listener_fd=descriptors->allocate_fd(FdType::SOCKET,O_RDWR,0);
  const int epoll_fd=descriptors->allocate_fd(FdType::EPOLL,O_RDWR,FD_CLOEXEC);
  Socket stream; stream.fd=tcp_fd; stream.domain=AF_INET; stream.type=SOCK_STREAM; stream.connected=true;
  stream.local_addr.sin_family=AF_INET; stream.local_addr.sin_port=htons(1000); stream.local_addr.sin_addr.s_addr=htonl(0x7f000001);
  stream.peer_addr=stream.local_addr; stream.peer_addr.sin_port=htons(1001); stream.keep_alive=true;
  Socket datagram; datagram.fd=udp_fd; datagram.domain=AF_INET; datagram.type=SOCK_DGRAM;
  Socket listener; listener.fd=listener_fd; listener.domain=AF_INET; listener.type=SOCK_STREAM; listener.bound=true; listener.listening=true; listener.backlog=1;
  TcpConnection connection; connection.local_fd=tcp_fd; connection.local_addr=stream.local_addr; connection.peer_addr=stream.peer_addr; connection.recv_buffer={"tcp-payload",""};
  TcpConnection pending=connection; pending.local_fd=42; pending.recv_buffer={"pending-payload"};
  const uint64_t pending_key=(uint64_t(pending.local_addr.sin_addr.s_addr)<<32)|pending.local_addr.sin_port;
  listener.pending_connections.push_back(pending_key); listener.established_connections[pending_key]=pending;
  std::map<int,Socket> sockets{{tcp_fd,stream},{udp_fd,datagram},{listener_fd,listener}};
  UdpDatagram packet; packet.content="udp-payload"; packet.from=stream.peer_addr;
  std::map<int,std::deque<UdpDatagram>> udp{{udp_fd,{packet}}};
  std::map<int,TcpConnection> tcp{{tcp_fd,connection}};
  EpollInstance epoll; epoll.epoll_fd=epoll_fd; epoll.watched_fds[tcp_fd]={EPOLLIN,1234};
  std::map<int,EpollInstance> epolls{{epoll_fd,epoll}};
  std::ostringstream network_encoded; { cereal::BinaryOutputArchive archive(network_encoded); archive(sockets,udp,tcp,epolls); }
  std::istringstream network_input(network_encoded.str()); { cereal::BinaryInputArchive archive(network_input); archive(saved.sock_state); }
  saved.fd_manager_state=*descriptors;
  thread_state thread; thread.tid=1; thread.physical_tid=123456; thread.alive=true; thread.is_main=true; thread.regs.rax=987; thread.regs.rip=0x1234; saved.threads.push_back(thread);
  saved.futex_state.emplace();
  std::ostringstream encoded; { cereal::BinaryOutputArchive archive(encoded); archive(saved); }
  const auto serialized=encoded.str(); std::vector<uint8_t> metadata(serialized.begin(),serialized.end()); user_regs_struct regs{};
  auto data=encode_page_manifest({r},metadata,"maps",regs,{{r.start,4096,page}});
  sys_state machine; machine.ts_hash[0]=store.save(data.data(),data.size()); machine.child[0].tv={12,34};
  detsim::StateView view(machine);
  StateDataHeader header{}; header.magic=0x4453544D; header.version=FLAT_SNAPSHOT_VERSION; header.num_regions=1;
  header.tstate_offset=sizeof(header)+sizeof(r); header.tstate_size=metadata.size();
  header.maps_offset=header.tstate_offset+metadata.size(); header.maps_size=4;
  header.regs_offset=header.maps_offset+header.maps_size+bytes.size();
  std::vector<uint8_t> flat(header.regs_offset+sizeof(regs)); auto flat_region=r; flat_region.offset=header.maps_offset+header.maps_size;
  memcpy(flat.data(),&header,sizeof(header)); memcpy(flat.data()+sizeof(header),&flat_region,sizeof(r));
  memcpy(flat.data()+header.tstate_offset,metadata.data(),metadata.size()); memcpy(flat.data()+header.maps_offset,"maps",4);
  memcpy(flat.data()+flat_region.offset,bytes.data(),bytes.size()); memcpy(flat.data()+header.regs_offset,&regs,sizeof(regs));
  sys_state flat_machine=machine; flat_machine.ts_hash[0]=store.save(flat.data(),flat.size()); detsim::StateView flat_view(flat_machine);
  require(flat_view.evaluate(0,"*(long*)0x1000")==view.evaluate(0,"*(long*)0x1000"),"identical observation across storage backends");
  const auto metadata_json=view.inspect_json(0);
  require(metadata_json==flat_view.inspect_json(0),"identical canonical metadata across storage backends");
  const auto inspection=nlohmann::json::parse(metadata_json);
  require(inspection.at("schema")==1 && inspection.at("threads")[0].at("id")==1 && inspection.at("threads")[0].at("registers").at("rax")==987,"logical thread/register inspection");
  require(metadata_json.find("physical_tid")==std::string::npos && metadata_json.find("tcp-payload")==std::string::npos,"metadata excludes physical IDs and payloads");
  require(inspection.at("lifecycle")=="running" && inspection.at("exit_code").is_null(), "named lifecycle without private status headers");
  require(inspection.at("sockets")[0].at("options").at("keep_alive")==true,"captured socket options");
  require(inspection.at("sockets")[2].at("pending")[0]==std::to_string(pending_key),"endpoint keys use lossless decimal strings");
  char payload[32]{};
  require(view.read_buffer(0,DETSIM_BUFFER_FILE,file_fd,0,0,1,payload,sizeof(payload))==2 && std::string(payload,2)=="bc","file partial observation");
  require(view.read_buffer(0,DETSIM_BUFFER_FILE,file_fd,0,0,3,payload,sizeof(payload))==0,"file EOF");
  require(view.read_buffer(0,DETSIM_BUFFER_PIPE,pipe_fds[1],0,0,1,payload,2)==2 && std::string(payload,2)=="ip","pipe write endpoint observation");
  require(view.read_buffer(0,DETSIM_BUFFER_TCP,tcp_fd,0,0,0,payload,3)==3 && std::string(payload,3)=="tcp","TCP prefix");
  require(view.read_buffer(0,DETSIM_BUFFER_TCP,tcp_fd,0,1,0,payload,3)==0,"persistent EOF chunk");
  require(view.read_buffer(0,DETSIM_BUFFER_UDP,udp_fd,0,0,0,payload,3)==3 && std::string(payload,3)=="udp","UDP prefix");
  require(view.read_buffer(0,DETSIM_BUFFER_TCP_PENDING,listener_fd,pending_key,0,0,payload,7)==7 && std::string(payload,7)=="pending","preaccept prefix");
  rejects([&]{view.read_buffer(0,DETSIM_BUFFER_FILE,999,0,0,0,payload,1);});
  rejects([&]{view.read_buffer(0,DETSIM_BUFFER_FILE,file_fd,0,0,4,payload,1);});
  rejects([&]{view.read_buffer(0,DETSIM_BUFFER_UDP,udp_fd,0,9,0,payload,1);});
  rejects([&]{view.read_buffer(0,DETSIM_BUFFER_TCP_PENDING,listener_fd,0,0,0,payload,1);});
  rejects([&]{view.read_buffer(0,DETSIM_BUFFER_PIPE,file_fd,0,0,0,payload,1);});
  require(view.inspect_json(0)==metadata_json,"buffer observations do not consume queues or offsets");
  ptmc_state.fs_states[0]=FileSystemState(); ptmc_state.sock_states[0]=SockState(); machine.status[0]=1;
  require(view.inspect_json(0)==metadata_json,"saved resource observation independent of runtime mutation");
  ptmc_state.cursor=NP-1; ptmc_state.running_state.ts_hash[0]=0;
  long output=0; view.read(0,0x1000,&output,sizeof(output)); require(output==123,"saved read");
  require(view.evaluate(0,"*(long*)0x1000")==123,"saved expression independent of live state");
  require(view.evaluate(0,"tracee0(*(long*)0x1000)")==123,"qualified saved read");
  require(view.evaluate(0,"2 + 3 * 4")==14,"arithmetic");
  require(view.time(0).tv_sec==12 && view.time(0).tv_usec==34,"saved clock");
  sys_state cold_dead; cold_dead.ts_hash[0]=machine.ts_hash[0];
  detsim::StateView cold_dead_view(cold_dead);
  require(cold_dead_view.time(0).tv_sec==12 && cold_dead_view.time(0).tv_usec==34,"cold dead-node clock comes from saved metadata");
  rejects([&]{view.evaluate(0,"tracee999(1)");});
  for(const char* expression:{"*(long*)0x1000 = 7","++*(long*)0x1000","(*(long*)0x1000)++","0 && ++*(long*)0x1000","1 ? 2 : (*(long*)0x1000 = 7)","sizeof(++*(long*)0x1000)"}) rejects([&]{view.evaluate(0,expression);});
  rejects([&]{view.evaluate(0,"*(long*)0x3000");});
  rejects([&]{view.evaluate(0,"unknown_symbol");});
  rejects([&]{view.evaluate(0,"1 +");});
  rejects([&]{view.evaluate(0,"1 / 0");});
  rejects([&]{view.evaluate(0,"1 << 64");});
  rejects([&]{view.evaluate(0,"9223372036854775807 + 1");});
  rejects([&]{view.read(NP,0x1000,&output,sizeof(output));});
  view.read(0,0x1000,&output,sizeof(output)); require(output==123,"observation cannot mutate");
  machine.ts_hash[0]=0; machine.child[0].tv={99,0};
  require(view.evaluate(0,"*(long*)0x1000")==123 && view.time(0).tv_sec==12,"view binds immutable identity");
  sys_state missing; detsim::StateView unavailable(missing); rejects([&]{unavailable.evaluate(0,"*(long*)0x1000");});
  {
    // Both fixtures have the same named globals. The service must never apply
    // one binary's symbol/type table to the other configured logical node.
    ptmc_state.tracee[0].executable=argv[1]; ptmc_state.tracee[1].executable=argv[2];
    dwarf_init(argv[1]);
    require(dwarf_fixed_executable(),"non-PIE symbol fixture");
    const auto address=dwarf_get_global_addr("fixture_record"); require(address,"fixture global");
    auto symbolic_region=r; symbolic_region.start=address & ~uint64_t(4095); symbolic_region.end=symbolic_region.start+4096;
    auto symbol_bytes=bytes; memset(symbol_bytes.data(),0,symbol_bytes.size());
    memcpy(symbol_bytes.data()+address-symbolic_region.start+8,&value,sizeof(value));
    const auto symbol_page=store.save(symbol_bytes.data(),symbol_bytes.size());
    auto symbol_manifest=encode_page_manifest({symbolic_region},metadata,"maps",regs,{{symbolic_region.start,4096,symbol_page}});
    sys_state symbol_machine; symbol_machine.ts_hash[0]=store.save(symbol_manifest.data(),symbol_manifest.size()); symbol_machine.ts_hash[1]=symbol_machine.ts_hash[0];
    detsim::StateView symbolic(symbol_machine);
    require(symbolic.evaluate(0,"fixture_record.total")==123,"matching main binary symbolic observation");
    rejects([&]{symbolic.evaluate(1,"fixture_record.total");});
    rejects([&]{symbolic.evaluate(1,"sizeof(struct FixtureRecord)");});
    require(symbolic.evaluate(1,"2+3")==5,"mixed binary constants stay usable");
    symbolic.read(1,address+8,&output,sizeof(output)); require(output==123,"mixed binary raw read stays usable");
    dwarf_init(argv[2]); rejects([&]{symbolic.evaluate(0,"fixture_record.total");});
    dwarf_init(argv[3]); ptmc_state.tracee[0].executable=argv[3];
    const auto layout_address=dwarf_get_global_addr("fixture_record"); require(layout_address,"changed-layout global");
    auto layout_region=r; layout_region.start=layout_address & ~uint64_t(4095); layout_region.end=layout_region.start+4096;
    memset(symbol_bytes.data(),0,symbol_bytes.size());
    long layout_value=456; memcpy(symbol_bytes.data()+layout_address-layout_region.start+16,&layout_value,sizeof(layout_value));
    auto layout_page=store.save(symbol_bytes.data(),symbol_bytes.size());
    auto layout_manifest=encode_page_manifest({layout_region},metadata,"maps",regs,{{layout_region.start,4096,layout_page}});
    sys_state layout_machine; layout_machine.ts_hash[0]=store.save(layout_manifest.data(),layout_manifest.size());
    detsim::StateView layout_view(layout_machine);
    require(layout_view.evaluate(0,"fixture_record.total")==456,"symbol reload invalidates cached same-name member layout");
    dwarf_cleanup();
    ptmc_state.tracee[0].executable=const_cast<char*>("/proc/self/exe");
    dwarf_init("/proc/self/exe");
    detsim::StateView own_image(symbol_machine);
    if (!dwarf_fixed_executable()) rejects([&]{own_image.evaluate(0,"sizeof(struct FixtureRecord)");});
    require(own_image.evaluate(0,"*(long*)"+std::to_string(address+8))==123,"raw casts do not need symbolic layout");
    dwarf_cleanup();
  }
  ptmc_state.fd_managers[0]=descriptors; ptmc_state.fs_states[0]=saved.fs_state; ptmc_state.sock_states[0]=saved.sock_state;
  auto live=detsim::StateView::stopped_live(); const auto live_json=live.inspect_json(0);
  require(!nlohmann::json::parse(live_json).at("registers_available").get<bool>() && nlohmann::json::parse(live_json).at("threads").is_null(),"live registers explicitly unavailable");
  ptmc_state.fs_states[0]=FileSystemState(); ptmc_state.sock_states[0]=SockState();
  require(live.inspect_json(0)==live_json,"live metadata query binds a consistent private copy");
  store.shutdown(); rejects([&]{view.evaluate(0,"*(long*)0x1000");});
  rejects([&]{view.inspect_json(0);}); rejects([&]{view.time(0);});
  rejects([&]{view.read_buffer(0,DETSIM_BUFFER_FILE,file_fd,0,0,0,payload,1);});
  store.init(cfg); rejects([&]{view.inspect_json(0);}); rejects([&]{view.time(0);});
  require(detsim::StateView(flat_machine).inspect_json(0)==metadata_json,"new view after reopen retains canonical metadata");
  store.shutdown();
  std::filesystem::remove_all(directory); puts("PASS read-only state observation"); return 0;
 } catch(const std::exception& e) { fprintf(stderr,"state view: %s\n",e.what()); StateStore::instance().shutdown(); std::filesystem::remove_all(directory); return 1; }
}
