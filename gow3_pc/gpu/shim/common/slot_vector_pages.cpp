// gow3 (SpartanPort c22329f): address-space reservation for Common::SlotVector, so its entries
// never move when it grows (common/slot_vector.h).

#include <cstddef>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace Common {

void* SlotVectorReservePages(std::size_t size) noexcept {
#ifdef _WIN32
    void* base = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
    if (!base) {
        std::fprintf(stderr, "SlotVector: reserving %zu bytes failed (%lu)\n", size,
                     GetLastError());
        std::abort();
    }
#else
    void* base = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) {
        std::fprintf(stderr, "SlotVector: reserving %zu bytes failed\n", size);
        std::abort();
    }
#endif
    return base;
}

void SlotVectorCommitPages(void* address, std::size_t size) noexcept {
#ifdef _WIN32
    if (!VirtualAlloc(address, size, MEM_COMMIT, PAGE_READWRITE)) {
        std::fprintf(stderr, "SlotVector: committing %zu bytes failed (%lu)\n", size,
                     GetLastError());
        std::abort();
    }
#else
    if (mprotect(address, size, PROT_READ | PROT_WRITE) != 0) {
        std::fprintf(stderr, "SlotVector: committing %zu bytes failed\n", size);
        std::abort();
    }
#endif
}

void SlotVectorReleasePages(void* base, std::size_t size) noexcept {
    if (!base) {
        return;
    }
#ifdef _WIN32
    (void)size;
    VirtualFree(base, 0, MEM_RELEASE);
#else
    munmap(base, size);
#endif
}

} // namespace Common
