#include "config.h"
#include "search/compatibility.h"
#include "search/parallel.h"
#include "ncurses_ui.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <getopt.h>
#include <sched.h>

extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }
namespace fs = std::filesystem;
static void require(bool good, const char* message) {
    if (!good) throw std::runtime_error(message);
}
static void write(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path); file << value << '\n';
    require(bool(file), "write synthetic topology");
}
static void cpu(const fs::path& root, int id, int core, const std::string& shared, int package=0) {
    const auto path = root / ("cpu" + std::to_string(id));
    write(path / "topology/physical_package_id", std::to_string(package));
    write(path / "topology/core_id", std::to_string(core));
    write(path / "cache/index3/type", "Unified");
    write(path / "cache/index3/level", "3");
    write(path / "cache/index3/shared_cpu_list", shared);
}
static void parse_case(std::vector<std::string> options, bool worker, bool success, int expected=-1) {
    const auto child = fork(); require(child >= 0, "fork parser case");
    if (!child) {
        worker_host_cpu=-1; worker_control_fd=-1; worker_blob_fd=-1;
        worker_shared_pool_path.clear(); tracee_native_cwd.clear();
        auto_mode=0; batch_file=nullptr; optind=0;
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) _exit(5);
        std::vector<std::string> args{"placement-parser"};
        args.insert(args.end(), options.begin(), options.end());
        if (worker) { args.push_back("--worker-fd"); args.push_back(std::to_string(sockets[0])); }
        std::vector<char*> argv; for (auto& arg : args) argv.push_back(arg.data()); argv.push_back(nullptr);
        try {
            parse_args(static_cast<int>(args.size()), argv.data());
            _exit(success && worker_host_cpu==expected ? 0 : 2);
        } catch (const std::exception&) { _exit(success ? 3 : 0); }
    }
    int status=0; require(waitpid(child,&status,0)==child,"wait parser case");
    require(WIFEXITED(status) && WEXITSTATUS(status)==0,"private CPU parser expectation");
}
int main() {
    char temporary[]="/tmp/detsim-placement-XXXXXX";
    require(mkdtemp(temporary),"temporary topology directory");
    const fs::path root(temporary);
    try {
        using detsim::search::worker_cpu_order;
        require(detsim::search::Options{}.worker_placement == detsim::search::Options::WorkerPlacement::None,
                "placement defaults off");
        const std::vector<int> all{0,1,2,3,4,5,6,7,8,9,10,11};
        for (int id : all) cpu(root,id,id%6, id%6<3 ? "0-2,6-8" : "3-5,9-11");
        require(worker_cpu_order(all,root.string()) == std::vector<int>({0,3,1,4,2,5,6,9,7,10,8,11}),"balanced LLC/core/SMT ordering");
        require(worker_cpu_order({11,8,2,5},root.string()) == std::vector<int>({2,5,8,11}),"restricted mask only");
        require(worker_cpu_order({0,1,3,6,7,9},root.string()) == std::vector<int>({0,3,1,6,9,7}),"uneven LLC core counts");
        write(root/"cpu0/cache/index3/shared_cpu_list","0-2");
        require(worker_cpu_order(all,root.string())==all,"inconsistent membership fallback");
        cpu(root,0,0,"0-2,6-8");
        fs::remove(root/"cpu0/topology/core_id");
        require(worker_cpu_order({3,0,6},root.string())==std::vector<int>({0,3,6}),"missing topology deterministic fallback");
        cpu(root,0,0,"0-2,6-8");
        write(root/"cpu0/cache/index3/level","3garbage");
        require(worker_cpu_order(all,root.string())==all,"malformed topology fallback");
        require(worker_cpu_order({4,1},(root/"absent").string())==std::vector<int>({1,4}),"absent sysfs fallback");
        bool rejected=false; try { (void)worker_cpu_order({1,1},root.string()); } catch (...) { rejected=true; }
        require(rejected,"duplicate CPU candidates reject");
        rejected=false; try { (void)worker_cpu_order({},root.string()); } catch (...) { rejected=true; }
        require(rejected,"empty CPU candidates reject");
        const auto other = root/"other";
        cpu(other,2,0,"2,14",7); cpu(other,14,0,"2,14",7);
        cpu(other,5,0,"5,17",9); cpu(other,17,0,"5,17",9);
        require(worker_cpu_order({17,14,5,2},other.string())==std::vector<int>({2,5,14,17}),
                "non-host indices and repeated core IDs across packages");
        parse_case({},false,true);
        parse_case({"--worker-host-cpu","0"},true,true,0);
        parse_case({"--worker-host-cpu=1"},true,true,1);
        for (const auto* value : {"-1","+1"," 1","1tail","","999999999999999999999"})
            parse_case({"--worker-host-cpu",value},true,false);
        parse_case({"--worker-host-cpu",std::to_string(CPU_SETSIZE)},true,false);
        parse_case({"--worker-host-cpu","0"},false,false);
        parse_case({"--worker-host-cpu","0","--worker-host-cpu","1"},true,false);
        fs::remove_all(root);
        std::cout << "PASS worker placement topology/default/private parser\n";
    } catch (...) { fs::remove_all(root); throw; }
}
