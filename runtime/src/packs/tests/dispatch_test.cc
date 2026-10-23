#include "guest_table.h"
#include "hle.h"
#include <cassert>
#include <cstdlib>

uint8_t *g_mem = nullptr;
void log_write(const char *, ...) {}
namespace threads {
const char *current_name() { return "synthetic"; }
} // namespace threads
extern "C" PPC_NORETURN void rt_fatal_exit() { std::abort(); }

namespace {
void text(PPCContext *context) { context->r[3] = 1; }
void cave(PPCContext *context) { context->r[3] = 2; }
void cave_tail(PPCContext *context) { context->r[3] = 3; }
} // namespace
const GuestFnEntry g_guest_functions[] = {
    {mem::kText, text},
    {mem::kText + 0x100, text},
#ifndef TEST_NO_CAVES
    {mem::kCodeCave, cave},
    {mem::kCodeCave + 0x100, cave_tail},
#endif
};
const uint32_t g_guest_function_count =
    sizeof(g_guest_functions) / sizeof(g_guest_functions[0]);

int main() {
  dispatch::init();
  assert(dispatch::lookup(mem::kText) == text);
  assert(dispatch::lookup(mem::kText + 0x100) == text);
  assert(!dispatch::lookup(mem::kText + 4));
  assert(!dispatch::lookup(mem::kText + 0x104));
#ifndef TEST_NO_CAVES
  assert(dispatch::lookup(mem::kCodeCave) == cave);
  assert(dispatch::lookup(mem::kCodeCave + 0x100) == cave_tail);
  PPCContext context{};
  rt_call(&context, mem::kCodeCave);
  assert(context.r[3] == 2);
  rt_call(&context, mem::kCodeCave + 0x100);
  assert(context.r[3] == 3);
#else
  assert(!dispatch::lookup(mem::kCodeCave));
#endif
  assert(!dispatch::lookup(mem::kCodeCave - 4));
  assert(!dispatch::lookup(mem::kCodeCave + 4));
  assert(!dispatch::lookup(mem::kCodeCave + 0x104));
  assert(!dispatch::lookup(mem::kText - 4));
  assert(!dispatch::lookup(0));
  assert(!dispatch::lookup(0xffffffff));
  dispatch::add_import_stub(0xc0000000, cave, "test", "call");
  assert(dispatch::lookup(0xc0000000) == cave);
}
