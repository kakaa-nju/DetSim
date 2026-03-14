#include <cstdio>
#include <cstring>
#include "core/state_store.h"
#include "core/sysstate_store.h"
#include "core/config.h"

// NCursesUI 集成
#include "ui/ncurses_ui.h"
#include "ui/log_wrapper.h"

void init_monitor(int argc, char *argv[]);
void ui_mainloop();
void init_state();
void init_dwarf();
void cleanup_all();

// 全局 UI 实例
static detsim::ui::NCursesUI* g_ui = nullptr;

// 外部访问接口
extern "C" detsim::ui::NCursesUI* get_ncurses_ui() { return g_ui; }

int main(int argc, char *argv[])
{
  /* 解析参数，检查是否有 --no-ui */
  bool use_ui = true;
  
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--no-ui") == 0) {
      use_ui = false;
    }
  }
  
  /* 先解析参数（设置 loglevel） */
  parse_args(argc, argv);
  
  /* 初始化 UI */
  if (use_ui) {
    static detsim::ui::NCursesUI ui(11);  // 11行状态区
    g_ui = &ui;
    
    if (!ui.init()) {
      fprintf(stderr, "Warning: UI init failed, using stdio mode\n");
      g_ui = nullptr;
    } else {
      // 初始化日志包装器
      detsim::ui::init_log_wrapper(g_ui);
      
      // 输出欢迎信息
      detsim::ui::ui_println("detsim tracer with NCursesUI");
      detsim::ui::ui_println("Loglevel: %d", loglevel);
      detsim::ui::ui_println("Type 'help' for available commands");
      detsim::ui::ui_println("");
      
      // 设置初始状态
      ui.set_status_line(1, "=== detsim tracer ===");
      ui.set_status_line(3, "Status: Ready");
      ui.set_status_line(4, "States: 0");
      ui.set_status_line(5, "Depth: 0");
      ui.set_status_line(9, "Commands: c(ontinue), s(tep), b(ack), q(uit)");
    }
  }
  
  /* 初始化监控器 */
  init_monitor(argc, argv);

  /* 启动 tracee */
  init_state();

  /* 分析变量地址 */
  init_dwarf();

  // 使用日志宏输出调试信息
  UI_LOG_DEBUG("About to call ui_mainloop");
  ui_mainloop();
  UI_LOG_DEBUG("ui_mainloop returned");
  
  /* 清理资源 */
  cleanup_all();
  UI_LOG_DEBUG("cleanup completed");
  
  /* 清理 */
  g_ui = nullptr;
  detsim::ui::init_log_wrapper(nullptr);
  
  return 0;
}
