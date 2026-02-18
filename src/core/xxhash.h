/*
 * Single-header xxHash64 implementation
 * Based on xxHash https://github.com/Cyan4973/xxHash
 */
#ifndef XXHASH_H
#define XXHASH_H

#include <cstdint>
#include <cstddef>

// Simple xxHash64 implementation
class XXHash64 {
public:
    static uint64_t hash(const void* input, size_t len, uint64_t seed = 0) {
        const uint8_t* data = (const uint8_t*)input;
        const uint8_t* end = data + len;
        uint64_t h64;
        
        // Prime numbers
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
            
            // Process 32 bytes at a time
            while (data + 32 <= end) {
                v1 += read64(data) * PRIME64_2; v1 = rotl64(v1, 31); v1 *= PRIME64_1;
                data += 8;
                v2 += read64(data) * PRIME64_2; v2 = rotl64(v2, 31); v2 *= PRIME64_1;
                data += 8;
                v3 += read64(data) * PRIME64_2; v3 = rotl64(v3, 31); v3 *= PRIME64_1;
                data += 8;
                v4 += read64(data) * PRIME64_2; v4 = rotl64(v4, 31); v4 *= PRIME64_1;
                data += 8;
            }
            
            h64 = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
            
            v1 *= PRIME64_2; v1 = rotl64(v1, 31); v1 *= PRIME64_1; h64 ^= v1;
            h64 = h64 * PRIME64_1 + PRIME64_4;
            
            v2 *= PRIME64_2; v2 = rotl64(v2, 31); v2 *= PRIME64_1; h64 ^= v2;
            h64 = h64 * PRIME64_1 + PRIME64_4;
            
            v3 *= PRIME64_2; v3 = rotl64(v3, 31); v3 *= PRIME64_1; h64 ^= v3;
            h64 = h64 * PRIME64_1 + PRIME64_4;
            
            v4 *= PRIME64_2; v4 = rotl64(v4, 31); v4 *= PRIME64_1; h64 ^= v4;
            h64 = h64 * PRIME64_1 + PRIME64_4;
        } else {
            h64 = seed + PRIME64_5;
        }
        
        h64 += len;
        
        // Process remaining 8-byte chunks
        while (data + 8 <= end) {
            uint64_t k1 = read64(data);
            k1 *= PRIME64_2; k1 = rotl64(k1, 31); k1 *= PRIME64_1;
            h64 ^= k1;
            h64 = rotl64(h64, 27) * PRIME64_1 + PRIME64_4;
            data += 8;
        }
        
        // Process remaining 4-byte chunk
        if (data + 4 <= end) {
            h64 ^= (uint64_t)(read32(data)) * PRIME64_1;
            h64 = rotl64(h64, 23) * PRIME64_2 + PRIME64_3;
            data += 4;
        }
        
        // Process remaining bytes
        while (data < end) {
            h64 ^= (*data) * PRIME64_5;
            h64 = rotl64(h64, 11) * PRIME64_1;
            data++;
        }
        
        // Finalization
        h64 ^= h64 >> 33;
        h64 *= PRIME64_2;
        h64 ^= h64 >> 29;
        h64 *= PRIME64_3;
        h64 ^= h64 >> 32;
        
        return h64;
    }
    
    // 32-bit version for compatibility
    static uint32_t hash32(const void* input, size_t len, uint32_t seed = 0) {
        return (uint32_t)(hash(input, len, seed) & 0xFFFFFFFF);
    }

private:
    static inline uint64_t read64(const uint8_t* ptr) {
        uint64_t val;
        __builtin_memcpy(&val, ptr, 8);
        return val;
    }
    
    static inline uint32_t read32(const uint8_t* ptr) {
        uint32_t val;
        __builtin_memcpy(&val, ptr, 4);
        return val;
    }
    
    static inline uint64_t rotl64(uint64_t x, int r) {
        return (x << r) | (x >> (64 - r));
    }
};

#endif // XXHASH_H
