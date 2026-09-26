#pragma once
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4459)
#endif
#include "../vendor/json.hpp"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <algorithm>
#include <cmath>
#include <ctime>
#include <string>

namespace meter {
using Json = nlohmann::json;
struct Window { bool known = false; int remaining = 0; long long reset = 0; int minutes = 0; };
struct Quota { Window shortTerm, weekly; std::string bucket; };

inline Window parseWindow(const Json& value) {
    Window w;
    if (!value.is_object() || !value.contains("usedPercent") || !value["usedPercent"].is_number()) return w;
    double used = value["usedPercent"].get<double>();
    if (!std::isfinite(used)) return w;
    w.known = true;
    w.remaining = static_cast<int>(std::round(100.0 - std::max(0.0, std::min(100.0, used))));
    if (value.contains("resetsAt") && value["resetsAt"].is_number_integer()) w.reset = value["resetsAt"].get<long long>();
    if (value.contains("windowDurationMins") && value["windowDurationMins"].is_number_integer()) w.minutes = value["windowDurationMins"].get<int>();
    return w;
}

inline Quota parseQuota(const Json& result, const std::string& preferred = "codex") {
    const Json* snapshot = nullptr;
    auto buckets = result.find("rateLimitsByLimitId");
    if (buckets != result.end() && buckets->is_object() && !buckets->empty()) {
        auto found = buckets->find(preferred);
        // Never silently select an unrelated model's quota.
        if (found == buckets->end() || !found->is_object()) throw std::runtime_error("bucket_missing");
        snapshot = &*found;
    } else if (result.contains("rateLimits") && result["rateLimits"].is_object()) {
        snapshot = &result["rateLimits"];
        if (snapshot->contains("limitId") && (*snapshot)["limitId"].is_string() && (*snapshot)["limitId"].get<std::string>() != preferred)
            throw std::runtime_error("bucket_missing");
    }
    if (!snapshot) throw std::runtime_error("quota_missing");
    Quota q; q.bucket = preferred;
    for (const char* key : {"primary", "secondary"}) {
        if (!snapshot->contains(key)) continue;
        Window w = parseWindow((*snapshot)[key]);
        if (!w.known) continue;
        // Classify by duration, not primary/secondary order. Unknown durations stay unknown.
        if (w.minutes == 10080) q.weekly = w;
        else if (w.minutes > 0 && w.minutes < 1440) q.shortTerm = w;
    }
    return q;
}

inline long long nextReset(const Quota& q) {
    long long a = q.shortTerm.known ? q.shortTerm.reset : 0;
    long long b = q.weekly.known ? q.weekly.reset : 0;
    return a > 0 && b > 0 ? std::min(a,b) : std::max(a,b);
}
inline std::wstring remainingTime(long long seconds) {
    if (seconds <= 0) return L"갱신 대기";
    if (seconds > 86400)
        return std::to_wstring(seconds/86400)+L"d "+std::to_wstring((seconds%86400)/3600)+L"h";
    long long minutes = (seconds+59)/60;
    return std::to_wstring(minutes/60)+L"h "+std::to_wstring(minutes%60)+L"m";
}
struct Placement { int x = 0, y = 0, width = 0, height = 0; bool compact = true; };
struct StripSize { int desired, minimum; };
inline StripSize stripSize(const Quota& q,int maxWidth,int minWidth) {
    int count = static_cast<int>(q.shortTerm.known)+static_cast<int>(q.weekly.known);
    if (count == 2) return {maxWidth,minWidth};
    // Omit unavailable quota windows, including the short-term window on some plans.
    return {count == 1 ? std::max(180,maxWidth-100) : 140, std::max(110,minWidth-70)};
}
// Button coordinates are window-relative physical pixels supplied by DWM.
inline Placement placeTitlebar(int left, int top, int buttonLeft, int buttonTop, int buttonBottom,
                               unsigned dpi, int maxWidth, int minWidth, int gap, int reservedLeft) {
    auto scale = [dpi](int v) { return static_cast<int>((static_cast<long long>(v) * dpi + 48) / 96); };
    int available = buttonLeft-scale(gap)-scale(reservedLeft);
    int height = scale(24), bandHeight = buttonBottom-buttonTop;
    // Never squeeze text, cover the left menu, or move down into the work surface.
    if (available < scale(minWidth) || bandHeight < height+scale(2)) return {};
    int width = std::min(scale(maxWidth),available);
    return {left+buttonLeft-scale(gap)-width, top+buttonTop+(bandHeight-height)/2,
            width, height, width < scale(260)};
}
inline bool shouldShow(bool foregroundMatches, bool visible, bool minimized, bool cloaked, bool paused, int width, int height) {
    return foregroundMatches && visible && !minimized && !cloaked && !paused && width >= 200 && height >= 120;
}
}
