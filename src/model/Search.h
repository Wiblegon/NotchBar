#pragma once
// Search coordination primitives (pure): query generations so that results of an older query can
// never overwrite a newer one, and fuzzy-ish app name ranking.
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace nb::search {

// Each keystroke starts a new generation. Background jobs capture their generation and
// a cancellation flag; results are applied only if the generation is still current.
class QueryGate {
public:
    struct Ticket {
        uint64_t generation = 0;
        std::shared_ptr<std::atomic<bool>> cancelled;
        bool IsCancelled() const { return cancelled && cancelled->load(std::memory_order_relaxed); }
    };

    Ticket Begin() {
        if (current_.cancelled) current_.cancelled->store(true);
        current_.generation = ++counter_;
        current_.cancelled = std::make_shared<std::atomic<bool>>(false);
        return current_;
    }
    void CancelAll() {
        if (current_.cancelled) current_.cancelled->store(true);
        current_.generation = ++counter_;
        current_.cancelled.reset();
    }
    bool IsCurrent(const Ticket& t) const { return t.generation == current_.generation && !t.IsCancelled(); }
    uint64_t Current() const { return current_.generation; }

private:
    uint64_t counter_ = 0;
    Ticket current_;
};

// Folds case and Turkish/Latin diacritics ("Görev Yöneticisi" -> "gorev yoneticisi", "İ"/"ı" -> "i")
// so that typing without special characters still matches.
std::wstring Fold(const std::wstring& s);

// 0 = no match; higher is better. `foldedQuery` must already be folded.
int RankName(const std::wstring& foldedName, const std::wstring& foldedQuery);

} // namespace nb::search
