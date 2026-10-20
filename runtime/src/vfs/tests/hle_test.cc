// SPDX-License-Identifier: MIT
// Exercise the actual FS hooks with synthetic guest memory; no RPX required.
#include "hle.h"
#include "paths.h"
#include "vfs/overlay.h"
#include <cassert>
#include <chrono>
#include <fstream>
#include <set>
#include <string>

extern "C" {
uint8_t *g_mem = nullptr;
void hle_coreinit_FSInit(PPCContext *);
void hle_coreinit_FSOpenFile(PPCContext *);
void hle_coreinit_FSReadFile(PPCContext *);
void hle_coreinit_FSCloseFile(PPCContext *);
void hle_coreinit_FSGetStatFile(PPCContext *);
void hle_coreinit_FSGetStat(PPCContext *);
void hle_coreinit_FSOpenDir(PPCContext *);
void hle_coreinit_FSReadDir(PPCContext *);
void hle_coreinit_FSCloseDir(PPCContext *);
}
std::string g_game_root, g_user_root;
int g_log_level = 0;
void log_write(const char *, ...) {}
HleRegistration::HleRegistration(const char *, const char *, GuestFn) {}

int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
                      ("botw-hle-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto game = root / utf8_path("jogo ação"), mod = root / utf8_path("tradução português"),
               local = root / utf8_path("usuário");
    g_game_root = utf8_string(game);
    g_user_root = utf8_string(local);
    fs::create_directories(game / "content/Pack");
    fs::create_directories(mod / "content/pack");
    std::ofstream(game / "content/Pack/Test") << "base";
    std::ofstream(game / "content/Pack/BaseOnly") << "base only";
    std::ofstream(mod / "content/pack/TEST") << "replacement";
    std::ofstream(mod / utf8_path("content/pack/Áudio")) << "added";
    vfs::content_overlay().configure(game, {{"tradução", mod}});
    uint8_t memory[8192]{};
    g_mem = memory;
    PPCContext c{};
    hle_coreinit_FSInit(&c);
    strcpy(reinterpret_cast<char *>(mem::host(128)), "//VOL///CONTENT/PACK/./test");
    strcpy(reinterpret_cast<char *>(mem::host(256)), "rb");
    c.r[5] = 128;
    c.r[6] = 256;
    c.r[7] = 512;
    c.r[8] = ~0u;
    hle_coreinit_FSOpenFile(&c);
    assert(c.r[3] == 0);
    auto file = mem::r32(512);
    c.r[5] = 128;
    c.r[6] = 1024;
    c.r[7] = ~0u;
    hle_coreinit_FSGetStat(&c);
    assert(c.r[3] == 0 && mem::r32(1024 + 0x10) == 11);
    c.r[5] = file;
    c.r[6] = 1024;
    hle_coreinit_FSGetStatFile(&c);
    assert(c.r[3] == 0 && mem::r32(1024 + 0x10) == 11);
    c.r[5] = 2048;
    c.r[6] = 1;
    c.r[7] = 32;
    c.r[8] = file;
    hle_coreinit_FSReadFile(&c);
    assert(c.r[3] == 11 && std::string(mem::str(2048), 11) == "replacement");
    c.r[5] = file;
    hle_coreinit_FSCloseFile(&c);
    strcpy(reinterpret_cast<char *>(mem::host(128)), "/vol/content/PACK/");
    c.r[5] = 128;
    c.r[6] = 512;
    c.r[7] = ~0u;
    hle_coreinit_FSOpenDir(&c);
    assert(c.r[3] == 0);
    auto dir = mem::r32(512);
    // A live merged directory must reserve its handle slot.
    hle_coreinit_FSOpenDir(&c);
    assert(c.r[3] == 0 && mem::r32(512) != dir);
    c.r[5] = mem::r32(512);
    hle_coreinit_FSCloseDir(&c);
    std::set<std::string> names;
    for (;;) {
        c.r[5] = dir;
        c.r[6] = 1024;
        hle_coreinit_FSReadDir(&c);
        if (c.r[3] == uint32_t(-2))
            break;
        assert(c.r[3] == 0);
        names.insert(mem::str(1024 + 0x64));
        if (std::string(mem::str(1024 + 0x64)) == "TEST")
            assert(mem::r32(1024 + 0x10) == 11);
    }
    assert((names == std::set<std::string>{"Áudio", "BaseOnly", "TEST"}));
    c.r[5] = dir;
    hle_coreinit_FSCloseDir(&c);
    // Closing a merged handle must free its slot.
    c.r[5] = 128;
    c.r[6] = 512;
    c.r[7] = ~0u;
    hle_coreinit_FSOpenDir(&c);
    assert(c.r[3] == 0 && mem::r32(512) == dir);
    c.r[5] = dir;
    hle_coreinit_FSCloseDir(&c);
    strcpy(reinterpret_cast<char *>(mem::host(128)), "/vol/content/Missing");
    c.r[5] = 128;
    c.r[6] = 1024;
    c.r[7] = ~0u;
    hle_coreinit_FSGetStat(&c);
    assert(c.r[3] == uint32_t(-6));
    // Overlay normalization must preserve W1's volume and host-path guards.
    for (const char *bad :
         {"/VOL/CONTENT/../../save/x", "/vol/contentious/x", "/vol/content/C:/secret", "/vol/content/..\\secret"}) {
        strcpy(reinterpret_cast<char *>(mem::host(128)), bad);
        c.r[5] = 128;
        c.r[6] = 1024;
        c.r[7] = ~0u;
        hle_coreinit_FSGetStat(&c);
        assert(c.r[3] == uint32_t(-6));
    }
    // Non-content directories use W1's native iterator even with mods active.
    fs::create_directories(local / "save/common");
    std::ofstream(local / utf8_path("save/common/ação")) << "save";
    strcpy(reinterpret_cast<char *>(mem::host(128)), "/vol/save/common");
    c.r[5] = 128;
    c.r[6] = 512;
    c.r[7] = ~0u;
    hle_coreinit_FSOpenDir(&c);
    assert(c.r[3] == 0);
    dir = mem::r32(512);
    c.r[5] = dir;
    c.r[6] = 1024;
    hle_coreinit_FSReadDir(&c);
    assert(c.r[3] == 0 && std::string(mem::str(1024 + 0x64)) == "ação");
    assert(mem::r32(1024 + 0x10) == 4);
    hle_coreinit_FSReadDir(&c);
    assert(c.r[3] == uint32_t(-2));
    hle_coreinit_FSCloseDir(&c);
    vfs::content_overlay().configure(game, {});
    strcpy(reinterpret_cast<char *>(mem::host(128)), "/vol/content/Pack/Test");
    c.r[5] = 128;
    c.r[6] = 1024;
    c.r[7] = ~0u;
    hle_coreinit_FSGetStat(&c);
    assert(c.r[3] == 0 && mem::r32(1024 + 0x10) == 4);
    fs::remove_all(root);
}
