#include "upload_read.h"

#include <windows.h>
#include <intrin.h>
#include <smmintrin.h>

#include <cstdint>
#include <cstring>

namespace pc2vr::uploads {
namespace {

bool StreamingLoadsSupported() {
    static const bool supported = [] {
        int info[4];
        __cpuid(info, 1);
        return (info[2] & (1 << 19)) != 0;  // SSE4.1, independent of CPU/GPU vendor
    }();
    return supported;
}

}  // namespace

bool ReadSnapshot(void* dst, const void* src, size_t bytes, bool writeCombined) {
    const bool stream = writeCombined && StreamingLoadsSupported() &&
                        !(reinterpret_cast<uintptr_t>(src) & 15) && !(bytes & 15);
    __try {
        if (!stream) {
            std::memcpy(dst, src, bytes);
            return true;
        }
        // Streaming-load buffers are not required to snoop another producer.
        // Fence each snapshot; never retain their contents across submissions.
        _mm_mfence();
        auto* from = reinterpret_cast<__m128i*>(const_cast<void*>(src));
        auto* to = reinterpret_cast<__m128i*>(dst);
        size_t i = 0;
        const size_t count = bytes / 16;
        // Read each streaming line together before writing the cached snapshot.
        for (; i + 4 <= count; i += 4) {
            const __m128i a = _mm_stream_load_si128(from + i);
            const __m128i b = _mm_stream_load_si128(from + i + 1);
            const __m128i c = _mm_stream_load_si128(from + i + 2);
            const __m128i d = _mm_stream_load_si128(from + i + 3);
            _mm_storeu_si128(to + i, a);
            _mm_storeu_si128(to + i + 1, b);
            _mm_storeu_si128(to + i + 2, c);
            _mm_storeu_si128(to + i + 3, d);
        }
        for (; i < count; ++i)
            _mm_storeu_si128(to + i, _mm_stream_load_si128(from + i));
        _mm_mfence();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace pc2vr::uploads
