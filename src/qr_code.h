#pragma once

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Compact, dependency-free QR Code generator (Byte mode, ECC Level L, Versions 1..8)
namespace QrCode {

struct Matrix {
    int size = 0;
    std::vector<uint8_t> modules; // 1 = dark, 0 = light

    bool Get(int x, int y) const {
        if (x < 0 || y < 0 || x >= size || y >= size) return false;
        return modules[static_cast<size_t>(y * size + x)] != 0;
    }
};

inline uint8_t GfMul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; ++i) {
        if (b & 1) r ^= a;
        bool hi = (a & 0x80) != 0;
        a <<= 1;
        if (hi) a ^= 0x1D; // 0x11D
        b >>= 1;
    }
    return r;
}

inline std::vector<uint8_t> RsCompute(const uint8_t* data, int dataLen, int ecLen) {
    std::vector<uint8_t> gen(ecLen + 1, 0);
    gen[0] = 1;
    uint8_t root = 1;
    for (int i = 0; i < ecLen; ++i) {
        for (int j = ecLen; j >= 1; --j) {
            gen[j] = gen[j - 1] ^ GfMul(gen[j], root);
        }
        gen[0] = GfMul(gen[0], root);
        root = GfMul(root, 2);
    }
    std::vector<uint8_t> rem(ecLen, 0);
    for (int i = 0; i < dataLen; ++i) {
        uint8_t factor = data[i] ^ rem[0];
        for (int j = 0; j < ecLen - 1; ++j) rem[j] = rem[j + 1];
        rem[ecLen - 1] = 0;
        for (int j = 0; j < ecLen; ++j) {
            rem[j] ^= GfMul(gen[ecLen - 1 - j], factor);
        }
    }
    return rem;
}

struct VerSpec {
    int ver;
    int totalCodewords;
    int ecPerBlock;
    int blocksG1;
    int dataG1;
    int blocksG2;
    int dataG2;
    int alignPos; // 0 if none, else single alignment coordinate (for v2..v6: 18..34; v7..v8 use 22, 38)
};

// Standard ISO/IEC 18004 ECC-L tables for Versions 1..6 (max 134 bytes payload)
constexpr VerSpec kSpecsL[] = {
    {1,  26,  7, 1, 19, 0,  0,  0},
    {2,  44, 10, 1, 34, 0,  0, 18},
    {3,  70, 15, 1, 55, 0,  0, 22},
    {4, 100, 20, 1, 80, 0,  0, 26},
    {5, 134, 26, 1,108, 0,  0, 30},
    {6, 172, 18, 2, 68, 0,  0, 34},
};

inline bool Encode(const std::string& text, Matrix& out) {
    const int nBytes = static_cast<int>(text.size());
    const VerSpec* spec = nullptr;
    for (const auto& s : kSpecsL) {
        int capBytes = s.blocksG1 * s.dataG1 + s.blocksG2 * s.dataG2;
        if (2 + nBytes <= capBytes) { spec = &s; break; } // 4-bit mode + 8-bit count + payload
    }
    if (!spec) return false;

    const int ver = spec->ver;
    const int sz = ver * 4 + 17;
    const int totalDataBytes = spec->blocksG1 * spec->dataG1 + spec->blocksG2 * spec->dataG2;

    std::vector<uint8_t> bits;
    auto pushBits = [&](uint32_t val, int count) {
        for (int i = count - 1; i >= 0; --i) bits.push_back((val >> i) & 1);
    };
    pushBits(0b0100, 4); // Byte mode
    pushBits(static_cast<uint32_t>(nBytes), 8);
    for (unsigned char c : text) pushBits(c, 8);
    int maxBits = totalDataBytes * 8;
    int term = std::min(4, maxBits - static_cast<int>(bits.size()));
    pushBits(0, term);
    while (bits.size() % 8 != 0) bits.push_back(0);
    bool alt = false;
    while (static_cast<int>(bits.size()) < maxBits) {
        pushBits(alt ? 0x11 : 0xEC, 8);
        alt = !alt;
    }

    std::vector<uint8_t> dataCw(totalDataBytes, 0);
    for (int i = 0; i < totalDataBytes; ++i) {
        uint8_t b = 0;
        for (int j = 0; j < 8; ++j) b = static_cast<uint8_t>((b << 1) | bits[static_cast<size_t>(i * 8 + j)]);
        dataCw[i] = b;
    }

    int numBlocks = spec->blocksG1 + spec->blocksG2;
    std::vector<std::vector<uint8_t>> dBlocks(numBlocks), eBlocks(numBlocks);
    int off = 0;
    for (int b = 0; b < numBlocks; ++b) {
        int len = (b < spec->blocksG1) ? spec->dataG1 : spec->dataG2;
        dBlocks[b].assign(dataCw.begin() + off, dataCw.begin() + off + len);
        eBlocks[b] = RsCompute(dBlocks[b].data(), len, spec->ecPerBlock);
        off += len;
    }

    std::vector<uint8_t> finalCw;
    int maxD = std::max(spec->dataG1, spec->dataG2);
    for (int i = 0; i < maxD; ++i) {
        for (int b = 0; b < numBlocks; ++b) {
            if (i < static_cast<int>(dBlocks[b].size())) finalCw.push_back(dBlocks[b][i]);
        }
    }
    for (int i = 0; i < spec->ecPerBlock; ++i) {
        for (int b = 0; b < numBlocks; ++b) finalCw.push_back(eBlocks[b][i]);
    }

    std::vector<uint8_t> grid(sz * sz, 0);
    std::vector<uint8_t> isFunc(sz * sz, 0);
    auto setFunc = [&](int x, int y, bool dark) {
        if (x < 0 || y < 0 || x >= sz || y >= sz) return;
        grid[y * sz + x] = dark ? 1 : 0;
        isFunc[y * sz + x] = 1;
    };
    auto drawFinder = [&](int cx, int cy) {
        for (int dy = -4; dy <= 4; ++dy) {
            for (int dx = -4; dx <= 4; ++dx) {
                int dist = std::max(std::abs(dx), std::abs(dy));
                setFunc(cx + dx, cy + dy, dist != 2 && dist != 4);
            }
        }
    };
    drawFinder(3, 3);
    drawFinder(sz - 4, 3);
    drawFinder(3, sz - 4);

    if (spec->alignPos > 0) {
        int ax = spec->alignPos, ay = spec->alignPos;
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                setFunc(ax + dx, ay + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
            }
        }
    }
    for (int i = 8; i < sz - 8; ++i) {
        setFunc(i, 6, (i % 2) == 0);
        setFunc(6, i, (i % 2) == 0);
    }
    setFunc(8, sz - 8, true); // dark module

    // Reserve format info modules
    for (int i = 0; i <= 8; ++i) {
        if (i != 6) { isFunc[8 * sz + i] = 1; isFunc[i * sz + 8] = 1; }
    }
    for (int i = sz - 8; i < sz; ++i) isFunc[8 * sz + i] = 1;
    for (int i = sz - 7; i < sz; ++i) isFunc[i * sz + 8] = 1;

    // Place data bits (zigzag)
    int bitIdx = 0;
    const int totalBits = static_cast<int>(finalCw.size()) * 8;
    for (int right = sz - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < sz; ++vert) {
            for (int j = 0; j < 2; ++j) {
                int x = right - j;
                bool upward = (((right + 1) & 2) == 0);
                int y = upward ? (sz - 1 - vert) : vert;
                if (!isFunc[y * sz + x]) {
                    bool dark = false;
                    if (bitIdx < totalBits) {
                        dark = ((finalCw[bitIdx >> 3] >> (7 - (bitIdx & 7))) & 1) != 0;
                        ++bitIdx;
                    }
                    grid[y * sz + x] = dark ? 1 : 0;
                }
            }
        }
    }

    // Choose best mask (0..7)
    auto applyFormat = [&](std::vector<uint8_t>& g, int mask) {
        uint32_t data = (0b01u << 3) | static_cast<uint32_t>(mask); // ECC L = 01
        uint32_t rem = data;
        for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ (((rem >> 9) & 1) ? 0x537u : 0u);
        uint32_t fmt = ((data << 10) | rem) ^ 0x5412u;
        for (int i = 0; i <= 5; ++i) g[8 * sz + i] = ((fmt >> i) & 1) ? 1 : 0;
        g[8 * sz + 7] = ((fmt >> 6) & 1) ? 1 : 0;
        g[8 * sz + 8] = ((fmt >> 7) & 1) ? 1 : 0;
        g[7 * sz + 8] = ((fmt >> 8) & 1) ? 1 : 0;
        for (int i = 9; i < 15; ++i) g[(14 - i) * sz + 8] = ((fmt >> i) & 1) ? 1 : 0;
        for (int i = 0; i < 8; ++i) g[8 * sz + (sz - 1 - i)] = ((fmt >> i) & 1) ? 1 : 0;
        for (int i = 8; i < 15; ++i) g[(sz - 15 + i) * sz + 8] = ((fmt >> i) & 1) ? 1 : 0;
    };

    int bestScore = 0x7fffffff;
    std::vector<uint8_t> bestGrid;
    for (int mask = 0; mask < 8; ++mask) {
        std::vector<uint8_t> g = grid;
        for (int y = 0; y < sz; ++y) {
            for (int x = 0; x < sz; ++x) {
                if (isFunc[y * sz + x]) continue;
                bool inv = false;
                switch (mask) {
                    case 0: inv = ((x + y) % 2) == 0; break;
                    case 1: inv = (y % 2) == 0; break;
                    case 2: inv = (x % 3) == 0; break;
                    case 3: inv = ((x + y) % 3) == 0; break;
                    case 4: inv = (((y / 2) + (x / 3)) % 2) == 0; break;
                    case 5: inv = ((x * y) % 2 + (x * y) % 3) == 0; break;
                    case 6: inv = ((((x * y) % 2) + ((x * y) % 3)) % 2) == 0; break;
                    case 7: inv = ((((x + y) % 2) + ((x * y) % 3)) % 2) == 0; break;
                }
                if (inv) g[y * sz + x] ^= 1;
            }
        }
        applyFormat(g, mask);
        int score = 0;
        for (int y = 0; y < sz; ++y) {
            int run = 1;
            for (int x = 1; x < sz; ++x) {
                if (g[y * sz + x] == g[y * sz + x - 1]) ++run;
                else { if (run >= 5) score += run - 2; run = 1; }
            }
            if (run >= 5) score += run - 2;
        }
        for (int x = 0; x < sz; ++x) {
            int run = 1;
            for (int y = 1; y < sz; ++y) {
                if (g[y * sz + x] == g[(y - 1) * sz + x]) ++run;
                else { if (run >= 5) score += run - 2; run = 1; }
            }
            if (run >= 5) score += run - 2;
        }
        for (int y = 0; y < sz - 1; ++y) {
            for (int x = 0; x < sz - 1; ++x) {
                uint8_t c = g[y * sz + x];
                if (c == g[y * sz + x + 1] && c == g[(y + 1) * sz + x] && c == g[(y + 1) * sz + x + 1]) score += 3;
            }
        }
        if (score < bestScore) { bestScore = score; bestGrid = std::move(g); }
    }

    out.size = sz;
    out.modules = std::move(bestGrid);
    return true;
}

} // namespace QrCode
