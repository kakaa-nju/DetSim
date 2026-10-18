#pragma once
#include "protocol.h"
#include "scheduler.h"

namespace detsim::search {
wire::DoneKind completion_kind(EnumerationKind kind);
// Before any tracee starts: bind worker lifetime to the socket's coordinator.
void prepare_worker_process(int fd, int blob_fd = -1);
// Called only after normal configuration, tracee and state initialization.
// Any failed task poisons the process: emit Error if possible and return 1.
// Shutdown returns 0. The caller performs ordinary owned-process cleanup.
int worker_loop(int fd, wire::Limits limits = {}, int blob_fd = -1);
}
