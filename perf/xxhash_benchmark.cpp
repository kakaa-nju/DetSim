/*
 * xxhash_benchmark.cpp - Benchmark XXH64 vs XXH32 throughput
 */

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <vector>
#include <random>
#include <algorithm>

// Original XXHash64 implementation from project
class XXHash64 {
public:
    static uint64_t hash(const void* input, size_t len, uint64_t seed = 0) {
        const uint8_t* data = (const uint8_t*)input;
        const uint8_t* end = data + len;
        uint64_t h64;
        
        const uint64_t PRIME64_1 = 11400714785074694791ULL;
        const uint64_t PRIME64_2 = 14029467366897019727ULL;
        const uint64_t PRIME64_3 = 1609587929392839161ULL;
        const uint64_t PRIME64_4 = 9650029242287828579ULL;
        const uint64_t PRIME64_5 = 2870177450012600261ULL;
        
        if (len >= 32) {
            uint64_t v1 = seed + PRIME64_1 + PRIME64_2;
            uint64_t v2 = seed + PRIME64_2;
            uint64_t v3 = seed + 0;
            uint64_t v4 = seed - PRIME64_1;
            
            while (data + 32 <= end) {
                v1 += read64(data) * PRIME64_2; v1 = rotl64(v1, 31); v1 *= PRIME64_1; data += 8;
                v2 += read64(data) * PRIME64_2; v2 = rotl64(v2, 31); v2 *= PRIME64_1; data += 8;
                v3 += read64(data) * PRIME64_2; v3 = rotl64(v3, 31); v3 *= PRIME64_1; data += 8;
                v4 += read64(data) * PRIME64_2; v4 = rotl64(v4, 31); v4 *= PRIME64_1; data += 8;
            }
            
            h64 = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
            v1 *= PRIME64_2; v1 = rotl64(v1, 31); v1 *= PRIME64_1; h64 ^= v1; h64 = h64 * PRIME64_1 + PRIME64_4;
            v2 *= PRIME64_2; v2 = rotl64(v2, 31); v2 *= PRIME64_1; h64 ^= v2; h64 = h64 * PRIME64_1 + PRIME64_4;
            v3 *= PRIME64_2; v3 = rotl64(v3, 31); v3 *= PRIME64_1; h64 ^= v3; h64 = h64 * PRIME64_1 + PRIME64_4;
            v4 *= PRIME64_2; v4 = rotl64(v4, 31); v4 *= PRIME64_1; h64 ^= v4; h64 = h64 * PRIME64_1 + PRIME64_4;
        } else {
            h64 = seed + PRIME64_5;
        }
        
        h64 += len;
        while (data + 8 <= end) {
            uint64_t k1 = read64(data);
            k1 *= PRIME64_2; k1 = rotl64(k1, 31); k1 *= PRIME64_1;
            h64 ^= k1;
            h64 = rotl64(h64, 27) * PRIME64_1 + PRIME64_4;
            data += 8;
        }
        if (data + 4 <= end) {
            h64 ^= (uint64_t)(read32(data)) * PRIME64_1;
            h64 = rotl64(h64, 23) * PRIME64_2 + PRIME64_3;
            data += 4;
        }
        while (data < end) {
            h64 ^= (*data) * PRIME64_5;
            h64 = rotl64(h64, 11) * PRIME64_1;
            data++;
        }
        h64 ^= h64 >> 33; h64 *= PRIME64_2; h64 ^= h64 >> 29; h64 *= PRIME64_3; h64 ^= h64 >> 32;
        return h64;
    }
    
    // Current implementation: just truncate 64-bit result
    static uint32_t hash32_trunc(const void* input, size_t len, uint32_t seed = 0) {
        return (uint32_t)(hash(input, len, seed) & 0xFFFFFFFF);
    }

private:
    static inline uint64_t read64(const uint8_t* ptr) {
        uint64_t val; __builtin_memcpy(&val, ptr, 8); return val;
    }
    static inline uint32_t read32(const uint8_t* ptr) {
        uint32_t val; __builtin_memcpy(&val, ptr, 4); return val;
    }
    static inline uint64_t rotl64(uint64_t x, int r) {
        return (x << r) | (x >> (64 - r));
    }
};

// True XXHash32 implementation (32-bit operations)
class XXHash32 {
public:
    static uint32_t hash(const void* input, size_t len, uint32_t seed = 0) {
        const uint8_t* data = (const uint8_t*)input;
        const uint8_t* end = data + len;
        uint32_t h32;
        
        const uint32_t PRIME32_1 = 2654435761U;
        const uint32_t PRIME32_2 = 2246822519U;
        const uint32_t PRIME32_3 = 3266489917U;
        const uint32_t PRIME32_4 = 668265263U;
        const uint32_t PRIME32_5 = 374761393U;
        
        if (len >= 16) {
            uint32_t v1 = seed + PRIME32_1 + PRIME32_2;
            uint32_t v2 = seed + PRIME32_2;
            uint32_t v3 = seed + 0;
            uint32_t v4 = seed - PRIME32_1;
            
            while (data + 16 <= end) {
                v1 += read32(data) * PRIME32_2; v1 = rotl32(v1, 13); v1 *= PRIME32_1; data += 4;
                v2 += read32(data) * PRIME32_2; v2 = rotl32(v2, 13); v2 *= PRIME32_1; data += 4;
                v3 += read32(data) * PRIME32_2; v3 = rotl32(v3, 13); v3 *= PRIME32_1; data += 4;
                v4 += read32(data) * PRIME32_2; v4 = rotl32(v4, 13); v4 *= PRIME32_1; data += 4;
            }
            
            h32 = rotl32(v1, 1) + rotl32(v2, 7) + rotl32(v3, 12) + rotl32(v4, 18);
        } else {
            h32 = seed + PRIME32_5;
        }
        
        h32 += (uint32_t)len;
        while (data + 4 <= end) {
            h32 += read32(data) * PRIME32_3;
            h32 = rotl32(h32, 17) * PRIME32_4;
            data += 4;
        }
        while (data < end) {
            h32 += (*data) * PRIME32_5;
            h32 = rotl32(h32, 11) * PRIME32_1;
            data++;
        }
        
        h32 ^= h32 >> 15; h32 *= PRIME32_2; h32 ^= h32 >> 13; h32 *= PRIME32_3; h32 ^= h32 >> 16;
        return h32;
    }

private:
    static inline uint32_t read32(const uint8_t* ptr) {
        uint32_t val; __builtin_memcpy(&val, ptr, 4); return val;
    }
    static inline uint32_t rotl32(uint32_t x, int r) {
        return (x << r) | (x >> (32 - r));
    }
};

static inline uint64_t get_time_ns() {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
}

static std::string format_bytes(size_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    int unit = 0;
    double size = bytes;
    while (size >= 1024 && unit < 3) { size /= 1024; unit++; }
    char buf[32]; snprintf(buf, sizeof(buf), "%.2f%s", size, units[unit]); return buf;
}

static std::string format_speed(double bytes_per_sec) {
    const char* units[] = {"B/s", "KB/s", "MB/s", "GB/s"};
    int unit = 0;
    double speed = bytes_per_sec;
    while (speed >= 1024 && unit < 3) { speed /= 1024; unit++; }
    char buf[32]; snprintf(buf, sizeof(buf), "%.2f%s", speed, units[unit]); return buf;
}

int main() {
    printf("=== XXHash Benchmark ===\n\n");
    
    // Test data sizes (typical state sizes)
    std::vector<size_t> sizes = {64, 256, 512, 1024, 4096, 65536, 524288};
    // Scale iterations inversely with size
    std::vector<int> iterations_list = {5000000, 2000000, 1000000, 500000, 100000, 10000, 2000};
    
    // Random data generator
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, 255);
    
    std::vector<uint8_t> max_data(524288);
    for (auto& b : max_data) b = (uint8_t)dist(rng);
    
    printf("%-10s %-15s %-20s %-20s %-20s\n", 
           "Size", "Iterations", "XXH64", "XXH32 (trunc)", "XXH32 (true)");
    printf("%-10s %-15s %-20s %-20s %-20s\n", 
           "==========", "===============", "====================", "====================", "====================");
    
    for (size_t idx = 0; idx < sizes.size(); idx++) {
        size_t size = sizes[idx];
        int iterations = iterations_list[idx];
        
        std::vector<uint8_t> data(max_data.begin(), max_data.begin() + size);
        volatile uint64_t hash64 = 0;
        volatile uint32_t hash32_trunc = 0;
        volatile uint32_t hash32_true = 0;
        
        // Warmup
        for (int i = 0; i < std::min(10000, iterations/10); i++) {
            hash64 = XXHash64::hash(data.data(), size);
            hash32_trunc = XXHash64::hash32_trunc(data.data(), size);
            hash32_true = XXHash32::hash(data.data(), size);
        }
        
        // Benchmark XXH64
        uint64_t start = get_time_ns();
        for (int i = 0; i < iterations; i++) {
            hash64 = XXHash64::hash(data.data(), size);
        }
        uint64_t elapsed64 = get_time_ns() - start;
        
        // Benchmark XXH32 (truncated from 64)
        start = get_time_ns();
        for (int i = 0; i < iterations; i++) {
            hash32_trunc = XXHash64::hash32_trunc(data.data(), size);
        }
        uint64_t elapsed32_trunc = get_time_ns() - start;
        
        // Benchmark XXH32 (true 32-bit)
        start = get_time_ns();
        for (int i = 0; i < iterations; i++) {
            hash32_true = XXHash32::hash(data.data(), size);
        }
        uint64_t elapsed32_true = get_time_ns() - start;
        
        // Calculate throughputs
        double bytes_processed = (double)size * iterations;
        double speed64 = bytes_processed / (elapsed64 / 1e9);
        double speed32_trunc = bytes_processed / (elapsed32_trunc / 1e9);
        double speed32_true = bytes_processed / (elapsed32_true / 1e9);
        
        printf("%-10s %-15d %-20s %-20s %-20s\n",
               format_bytes(size).c_str(),
               iterations,
               format_speed(speed64).c_str(),
               format_speed(speed32_trunc).c_str(),
               format_speed(speed32_true).c_str());
        
        // Prevent optimization
        (void)hash64; (void)hash32_trunc; (void)hash32_true;
    }
    
    printf("\n=== Summary ===\n");
    printf("XXH32 (trunc): Uses XXH64 then masks low 32 bits - same computation cost\n");
    printf("XXH32 (true):  Native 32-bit implementation - 2x less data processed per round\n");
    printf("\nFor StateStore use case (512KB states):\n");
    printf("- XXH64 provides better hash quality (64-bit space)\n");
    printf("- XXH32 trunc is 'free' if you only need 32 bits\n");
    printf("- XXH32 true may be slightly faster on 32-bit platforms\n");
    
    return 0;
}
