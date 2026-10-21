#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_MOVIEWATCH_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_MOVIEWATCH_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

// APS5_MOVIE_WATCH=1 (docs/design/frame-rate.md S6a; diagnostic, default off): how fast each Bink 2 movie
// the title streams plays, from its file reads, so a run with the vblank above 60 Hz shows whether the
// movies keep their speed (every movie in the dump is 30 fps: the boot logos, the intro, the main menu's
// background loop). A streaming player reads its file as it shows it, so the time its reads take to cross
// the middle half of the file, against half the movie's length, is the playback speed (1.00 = right;
// read-ahead moves both ends alike). Readers: APR ReadFile (by path), sceKernelRead/Pread and AIO on
// descriptors from sceKernelOpen. Lines: "[movie] NAME (api): ..." at the first read, "[movie] NAME pass N:
// speed S" once the middle half is read, "[movie] NAME pass N ended" at a loop or a close.
namespace MovieWatch {

// The Bink 2 header: "KB2" and a revision, the size after 8 bytes, the frame count, the largest frame, the
// frame count again, width, height, the frame rate as numerator / denominator.
inline constexpr std::size_t HeaderBytes = 36;

struct Header {
    std::uint32_t frames = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t rateNumerator = 0;
    std::uint32_t rateDenominator = 0;
    bool valid = false;

    double Fps() const { return valid ? static_cast<double>(rateNumerator) / rateDenominator : 0.0; }
    double Seconds() const { return valid ? frames * static_cast<double>(rateDenominator) / rateNumerator : 0.0; }
};

inline std::uint32_t ReadU32(const unsigned char* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8u) | (static_cast<std::uint32_t>(bytes[2]) << 16u) |
        (static_cast<std::uint32_t>(bytes[3]) << 24u);
}

inline Header ParseHeader(const void* data, std::size_t size) {
    Header header;
    if (data == nullptr || size < HeaderBytes) return header;
    const auto* bytes = static_cast<const unsigned char*>(data);
    if (std::memcmp(bytes, "KB2", 3) != 0) return header;
    header.frames = ReadU32(bytes + 8);
    header.width = ReadU32(bytes + 20);
    header.height = ReadU32(bytes + 24);
    header.rateNumerator = ReadU32(bytes + 28);
    header.rateDenominator = ReadU32(bytes + 32);
    header.valid = header.frames != 0 && header.rateNumerator != 0 && header.rateDenominator != 0;
    return header;
}

// A file the watch follows: a .bk2 (any case).
inline bool IsMovie(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    for (auto& character : extension) {
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    }
    return extension == ".bk2";
}

// One pass through one movie file. Reads arrive as [begin, end) at a time in seconds; `reached` is the
// furthest end so far. The times the reads cross a quarter and three quarters of the file are interpolated
// between the read that crossed and the one before it (a player reads in chunks).
class Pass {
public:
    Pass() = default;
    Pass(std::uint64_t fileBytes, double nominalSeconds) : _fileBytes(fileBytes), _nominalSeconds(nominalSeconds) {}

    // A read near the start once the pass has read past half the file: the movie looped (or restarted).
    bool Loops(std::uint64_t begin) const { return _fileBytes != 0 && _reached >= _fileBytes / 2 && begin < _fileBytes / 10; }

    // Starts the next pass of the same file.
    void Restart() { *this = Pass(_fileBytes, _nominalSeconds); }

    // Returns true when this read crossed three quarters of the file: the pass is measured.
    bool Read(double seconds, std::uint64_t begin, std::uint64_t end) {
        if (end < begin) end = begin;
        _bytes += end - begin;
        if (!_started) {
            _started = true;
            _first = seconds;
            _previous = seconds;
        }
        _last = seconds;
        bool measured = false;
        if (end > _reached) {
            const auto quarter = _fileBytes / 4, threeQuarters = _fileBytes - _fileBytes / 4;
            if (_reached < quarter && end >= quarter) _quarter = Crossed(quarter, seconds, end);
            if (_reached < threeQuarters && end >= threeQuarters) {
                _threeQuarters = Crossed(threeQuarters, seconds, end);
                _measured = measured = _fileBytes != 0;
            }
            _reached = end;
            _previous = seconds;
        }
        return measured;
    }

    bool Measured() const { return _measured; }
    // Seconds between the reads crossing a quarter and three quarters of the file.
    double MiddleSeconds() const { return _measured ? _threeQuarters - _quarter : 0.0; }
    // Half the movie's length over MiddleSeconds(); 0 when that half was read far ahead of any playback
    // (under 5% of its length: a preload, a whole-file read).
    double Speed() const {
        const double nominal = _nominalSeconds / 2.0, middle = MiddleSeconds();
        return _measured && nominal > 0.0 && middle > 0.05 * nominal ? nominal / middle : 0.0;
    }
    std::uint64_t Bytes() const { return _bytes; }
    std::uint64_t Reached() const { return _reached; }
    double Seconds() const { return _started ? _last - _first : 0.0; }
    bool Started() const { return _started; }

private:
    double Crossed(std::uint64_t mark, double seconds, std::uint64_t end) const {
        if (end <= _reached) return seconds;
        return _previous + (seconds - _previous) * static_cast<double>(mark - _reached) / static_cast<double>(end - _reached);
    }

    std::uint64_t _fileBytes = 0;
    double _nominalSeconds = 0.0;
    std::uint64_t _reached = 0;
    std::uint64_t _bytes = 0;
    bool _started = false;
    bool _measured = false;
    double _first = 0.0;
    double _last = 0.0;
    double _previous = 0.0;
    double _quarter = 0.0;
    double _threeQuarters = 0.0;
};

}

// Exported from libc with C linkage (other modules import only unmangled _nid_no_patch names). Every call
// returns at once with APS5_MOVIE_WATCH unset.
// Whether the watch follows hostPath (APR marks each file once, when it resolves the path).
extern "C" bool WatchesMovie_nid_no_patch(const std::filesystem::path& hostPath);
// Descriptors: sceKernelOpen tells the watch every file it opens (a reused number starts clean), close forgets it.
extern "C" void TrackMovieDescriptor_nid_no_patch(int fd, const std::filesystem::path& hostPath);
extern "C" void ForgetMovieDescriptor_nid_no_patch(int fd);
// A finished read of `bytes` at `offset` through `api`; offset -1: the descriptor's own position (sceKernelRead),
// counted by the watch from the reads since the open.
extern "C" void NoteMovieDescriptorRead_nid_no_patch(int fd, const char* api, std::int64_t offset, std::uint64_t bytes);
extern "C" void NoteMovieRead_nid_no_patch(const std::filesystem::path& hostPath, const char* api, std::uint64_t offset, std::uint64_t bytes);

#endif
