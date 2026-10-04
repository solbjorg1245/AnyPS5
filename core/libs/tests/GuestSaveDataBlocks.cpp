#include "prx/libc/include/general/VabiMacros.hpp"
#include "SceTypes.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

extern "C" {
int APS5_VABI sceSaveDataMount3(const SaveDataMount3*, SaveDataMountResult*);
int APS5_VABI sceSaveDataUmount2(std::uint32_t, const SaveDataMountPoint*);
int APS5_VABI sceSaveDataGetMountInfo(const SaveDataMountPoint*, SaveDataMountInfo*);
int APS5_VABI sceSaveDataDirNameSearch(const SaveDataDirNameSearchCond*, SaveDataDirNameSearchResult*);
int APS5_VABI sceSaveDataDelete(const SaveDataDelete*);
}

namespace {

constexpr std::uint32_t MountRdonly = 1;
constexpr std::uint32_t MountRdwr = 2;
constexpr std::uint32_t MountCreate = 4;

int failures = 0;

void Check(bool condition, const std::string& what) {
    if (condition) return;
    std::fprintf(stderr, "savedata blocks check failed: %s\n", what.c_str());
    ++failures;
}

SceSaveDataDirName Name(const char* text) {
    SceSaveDataDirName name{};
    std::snprintf(name.data, sizeof(name.data), "%s", text);
    return name;
}

int Mount(const SceSaveDataDirName& name, std::uint32_t mode, std::uint64_t blocks, SaveDataMountResult& result) {
    SaveDataMount3 mount{};
    mount.user_id = 1;
    mount.dir_name = &name;
    mount.blocks = blocks;
    mount.mount_mode = mode;
    return sceSaveDataMount3(&mount, &result);
}

SaveDataMountInfo QueryMountInfo(const SaveDataMountResult& mounted) {
    SaveDataMountInfo info{};
    Check(sceSaveDataGetMountInfo(&mounted.mount_point, &info) == 0, "mount info of a mounted save");
    return info;
}

}

int main() {
    const auto root = std::filesystem::temp_directory_path() / ("anyps5-savedata-blocks-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(root);
    const auto previous = std::filesystem::current_path();
    std::filesystem::current_path(root);

    // A save reports the size it was created with, and what its files leave of it.
    const auto game = Name("GAME0");
    SaveDataMountResult mounted{};
    Check(Mount(game, MountCreate | MountRdwr, 96, mounted) == 0, "create a 96-block save");
    std::ofstream(std::filesystem::path("_sd") / "GAME0" / "data.bin", std::ios::binary) << std::string(0x18000, 'x');
    auto info = QueryMountInfo(mounted);
    Check(info.blocks == 96, "created save reports its 96 blocks");
    Check(info.free_blocks == 94, "a 96 KiB file uses two 64 KiB blocks");
    Check(sceSaveDataUmount2(0, &mounted.mount_point) == 0, "unmount");
    Check(Mount(game, MountRdonly, 0, mounted) == 0, "reopen without a size");
    Check(QueryMountInfo(mounted).blocks == 96, "reopened save keeps its size");
    Check(sceSaveDataUmount2(0, &mounted.mount_point) == 0, "unmount again");

    std::vector<SceSaveDataDirName> names(4);
    std::vector<SaveDataSearchInfo> infos(4);
    SaveDataDirNameSearchCond cond{};
    cond.user_id = 1;
    SaveDataDirNameSearchResult result{};
    result.dir_names = names.data();
    result.dir_names_num = static_cast<std::uint32_t>(names.size());
    result.infos = infos.data();
    Check(sceSaveDataDirNameSearch(&cond, &result) == 0 && result.set_num == 1, "search finds the save");
    Check(infos[0].blocks == 96 && infos[0].free_blocks == 94, "search info carries blocks and free blocks");
    // A second save lands 32 bytes after the first name and 48 bytes after the first info, as titles
    // index the arrays.
    SaveDataMountResult second{};
    Check(Mount(Name("GAME1"), MountCreate | MountRdwr, 48, second) == 0, "create a second save");
    Check(sceSaveDataUmount2(0, &second.mount_point) == 0, "unmount the second save");
    Check(sizeof(SceSaveDataDirName) == 32 && sizeof(SaveDataSearchInfo) == 48, "search result element sizes");
    Check(sceSaveDataDirNameSearch(&cond, &result) == 0 && result.set_num == 2, "search finds both saves");
    const bool firstIsGame0 = std::strcmp(names[0].data, "GAME0") == 0;
    Check(std::strcmp(names[firstIsGame0 ? 1 : 0].data, "GAME1") == 0, "the second name is where a title reads it");
    Check(infos[firstIsGame0 ? 1 : 0].blocks == 48, "the second info is where a title reads it");
    SaveDataDelete delSecond{};
    const auto gameOne = Name("GAME1");
    delSecond.dir_name = &gameOne;
    Check(sceSaveDataDelete(&delSecond) == 0, "delete the second save");

    // A save made before sizes were recorded takes the size the title mounts it with.
    std::filesystem::create_directories(std::filesystem::path("_sd") / "OLD0");
    const auto old = Name("OLD0");
    Check(Mount(old, MountRdonly, 48, mounted) == 0, "open an unrecorded save");
    info = QueryMountInfo(mounted);
    Check(info.blocks == 48 && info.free_blocks == 48, "unrecorded save takes the mounted size");
    Check(sceSaveDataUmount2(0, &mounted.mount_point) == 0, "unmount old save");

    SaveDataDelete del{};
    del.dir_name = &game;
    Check(sceSaveDataDelete(&del) == 0, "delete the save");
    Check(!std::filesystem::exists(std::filesystem::path("_sd") / "GAME0.blocks"), "delete removes the size record");

    std::filesystem::current_path(previous);
    std::error_code error;
    std::filesystem::remove_all(root, error);
    if (failures != 0) return 1;
    std::printf("savedata blocks tests passed\n");
    return 0;
}
