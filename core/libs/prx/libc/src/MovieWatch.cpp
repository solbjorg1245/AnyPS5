#include "prx/libc/include/MovieWatch.hpp"
#include "prx/libc/include/ReadPatch.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace {

bool Enabled() {
    static const bool enabled = std::getenv("APS5_MOVIE_WATCH") != nullptr;
    return enabled;
}

double Now() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

struct Movie {
    std::string name;
    std::uint64_t fileBytes = 0;
    MovieWatch::Header header;
    MovieWatch::Pass pass;
    int passes = 1;
    bool announced = false;
};

// A descriptor's movie and its own position (reads without an offset).
struct Descriptor {
    std::shared_ptr<Movie> movie;
    std::uint64_t position = 0;
};

struct Registry {
    std::mutex mutex;
    std::atomic<int> descriptorCount{0};
    std::map<std::filesystem::path::string_type, std::shared_ptr<Movie>> movies;
    std::unordered_map<int, Descriptor> descriptors;
};

Registry& Movies() {
    static Registry registry;
    return registry;
}

// The movie of hostPath, its header read from the host file the first time (registry mutex held).
std::shared_ptr<Movie> MovieFor(Registry& registry, const std::filesystem::path& hostPath) {
    auto& movie = registry.movies[ReadPatch::Key(hostPath)];
    if (movie) return movie;
    movie = std::make_shared<Movie>();
    movie->name = hostPath.filename().string();
    std::error_code error;
    movie->fileBytes = std::filesystem::file_size(hostPath, error);
    if (error) movie->fileBytes = 0;
    char bytes[MovieWatch::HeaderBytes] = {};
    std::ifstream stream(hostPath, std::ios::binary);
    if (stream.read(bytes, sizeof(bytes))) movie->header = MovieWatch::ParseHeader(bytes, sizeof(bytes));
    movie->pass = MovieWatch::Pass(movie->fileBytes, movie->header.Seconds());
    return movie;
}

double Megabytes(std::uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

void PassEnded(const Movie& movie, const char* why) {
    if (!movie.pass.Started()) return;
    std::fprintf(stderr, "[movie] %s pass %d ended (%s): %.1f MB read over %.2f s (%.2f s at %.2f fps)\n", movie.name.c_str(), movie.passes, why,
        Megabytes(movie.pass.Bytes()), movie.pass.Seconds(), movie.header.Seconds(), movie.header.Fps());
}

// One read of [begin, end) of the movie (registry mutex held).
void Note(Movie& movie, const char* api, std::uint64_t begin, std::uint64_t end) {
    const double now = Now();
    if (!movie.announced) {
        movie.announced = true;
        if (movie.header.valid) {
            std::fprintf(stderr, "[movie] %s (%s): %ux%u, %u frames at %.2f fps = %.2f s, %.1f MB\n", movie.name.c_str(), api, movie.header.width, movie.header.height,
                movie.header.frames, movie.header.Fps(), movie.header.Seconds(), Megabytes(movie.fileBytes));
        } else {
            std::fprintf(stderr, "[movie] %s (%s): no Bink 2 header, %.1f MB; speed not measured\n", movie.name.c_str(), api, Megabytes(movie.fileBytes));
        }
    }
    if (!movie.header.valid) return;
    if (movie.pass.Loops(begin)) {
        PassEnded(movie, "loop");
        movie.pass.Restart();
        ++movie.passes;
    }
    if (movie.pass.Read(now, begin, end)) {
        const double speed = movie.pass.Speed();
        if (speed > 0.0) {
            std::fprintf(stderr, "[movie] %s pass %d: speed %.2fx (the middle half read in %.2f s, %.2f s at %.2f fps)\n", movie.name.c_str(), movie.passes, speed,
                movie.pass.MiddleSeconds(), movie.header.Seconds() / 2.0, movie.header.Fps());
        } else {
            std::fprintf(stderr, "[movie] %s pass %d: speed not measurable (the middle half read in %.2f s: read ahead of playback)\n", movie.name.c_str(), movie.passes,
                movie.pass.MiddleSeconds());
        }
    }
    std::fflush(stderr);
}

}

bool WatchesMovie_nid_no_patch(const std::filesystem::path& hostPath) {
    return Enabled() && MovieWatch::IsMovie(hostPath);
}

void TrackMovieDescriptor_nid_no_patch(int fd, const std::filesystem::path& hostPath) {
    if (!Enabled()) return;
    auto& registry = Movies();
    std::lock_guard lock(registry.mutex);
    if (registry.descriptors.erase(fd) != 0) --registry.descriptorCount;
    if (!MovieWatch::IsMovie(hostPath)) return;
    registry.descriptors[fd] = Descriptor{MovieFor(registry, hostPath), 0};
    ++registry.descriptorCount;
}

void ForgetMovieDescriptor_nid_no_patch(int fd) {
    if (!Enabled()) return;
    auto& registry = Movies();
    if (registry.descriptorCount.load(std::memory_order_relaxed) == 0) return;
    std::lock_guard lock(registry.mutex);
    const auto found = registry.descriptors.find(fd);
    if (found == registry.descriptors.end()) return;
    auto& movie = *found->second.movie;
    // Another descriptor may still read the file; a pass ends only with the last one.
    bool shared = false;
    for (const auto& [other, descriptor] : registry.descriptors) shared = shared || (other != fd && descriptor.movie == found->second.movie);
    registry.descriptors.erase(found);
    --registry.descriptorCount;
    if (shared) return;
    PassEnded(movie, "close");
    std::fflush(stderr);
    movie.pass.Restart();
    movie.passes = 1;
    movie.announced = false;
}

void NoteMovieDescriptorRead_nid_no_patch(int fd, const char* api, std::int64_t offset, std::uint64_t bytes) {
    if (!Enabled() || bytes == 0) return;
    auto& registry = Movies();
    if (registry.descriptorCount.load(std::memory_order_relaxed) == 0) return;
    std::lock_guard lock(registry.mutex);
    const auto found = registry.descriptors.find(fd);
    if (found == registry.descriptors.end()) return;
    auto& descriptor = found->second;
    std::uint64_t begin = offset >= 0 ? static_cast<std::uint64_t>(offset) : descriptor.position;
    // A read past the end through the descriptor's own position follows a seek back: the start again.
    if (offset < 0 && descriptor.movie->fileBytes != 0 && begin >= descriptor.movie->fileBytes) begin = 0;
    if (offset < 0) descriptor.position = begin + bytes;
    Note(*descriptor.movie, api, begin, begin + bytes);
}

void NoteMovieRead_nid_no_patch(const std::filesystem::path& hostPath, const char* api, std::uint64_t offset, std::uint64_t bytes) {
    if (!Enabled() || bytes == 0) return;
    auto& registry = Movies();
    std::lock_guard lock(registry.mutex);
    Note(*MovieFor(registry, hostPath), api, offset, offset + bytes);
}
