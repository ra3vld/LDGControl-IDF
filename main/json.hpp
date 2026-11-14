// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <ArduinoJson.h>
#include <cstdlib>
#include <cstddef>
// Bound each document including parser nodes; payload length alone is insufficient.
class JsonBudget final : public ArduinoJson::Allocator {
    struct alignas(std::max_align_t) Header {
        size_t size;
    };
    size_t used_ = 0, limit_;

  public:
    explicit JsonBudget(size_t limit) : limit_(limit) {}
    void *allocate(size_t n) override {
        if (n > limit_ - used_)
            return nullptr;
        auto h = static_cast<Header *>(malloc(sizeof(Header) + n));
        if (!h)
            return nullptr;
        h->size = n;
        used_ += n;
        return h + 1;
    }
    void deallocate(void *p) override {
        if (!p)
            return;
        auto h = static_cast<Header *>(p) - 1;
        used_ -= h->size;
        free(h);
    }
    void *reallocate(void *p, size_t n) override {
        if (!p)
            return allocate(n);
        auto h = static_cast<Header *>(p) - 1;
        const size_t old = h->size;
        if (n > limit_ - (used_ - old))
            return nullptr;
        h = static_cast<Header *>(realloc(h, sizeof(Header) + n));
        if (!h)
            return nullptr;
        h->size = n;
        used_ = used_ - old + n;
        return h + 1;
    }
};
struct BoundedJson {
    JsonBudget allocator;
    JsonDocument doc;
    explicit BoundedJson(size_t limit = 8192) : allocator(limit), doc(&allocator) {}
};
