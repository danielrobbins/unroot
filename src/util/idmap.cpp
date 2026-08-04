#include "util/idmap.hpp"

#include <climits>
#include <utility>

namespace util {
namespace {

bool validExtent(const IdMapExtent& extent) {
    return extent.count != 0 &&
           extent.inside <= UINT_MAX - (extent.count - 1) &&
           extent.outside <= UINT_MAX - (extent.count - 1);
}

bool validRoot(const IdMapExtent& extent) {
    return extent.inside == 0 && extent.count == 1;
}

bool overlaps(const IdMapExtent& left, const IdMapExtent& right) {
    return left.outside <= right.outside + (right.count - 1) &&
           right.outside <= left.outside + (left.count - 1);
}

} // namespace

IdMapPlan makeSingleIdMap(uid_t uid, gid_t gid) {
    return {IdMapMode::Single,
            {{0, static_cast<unsigned int>(uid), 1}},
            {{0, static_cast<unsigned int>(gid), 1}}, "process"};
}

IdMapPlan makeRichIdMap(uid_t uid, gid_t gid, unsigned int uidStart,
                        unsigned int gidStart, unsigned int count,
                        std::string source) {
    return {IdMapMode::Rich,
            {{0, static_cast<unsigned int>(uid), 1}, {1, uidStart, count}},
            {{0, static_cast<unsigned int>(gid), 1}, {1, gidStart, count}},
            std::move(source)};
}

IdMapPlan makeNativeIdMap() {
    return {IdMapMode::Native, {}, {}, "host"};
}

std::string validateIdMapPlan(const IdMapPlan& plan) {
    if (plan.source.empty()) return "ID map has no allocation source";
    if (plan.mode == IdMapMode::Native)
        return plan.uids.empty() && plan.gids.empty()
                   ? std::string{}
                   : "native ownership must not contain ID-map extents";
    const size_t expected = plan.mode == IdMapMode::Rich ? 2 : 1;
    if (plan.uids.size() != expected || plan.gids.size() != expected)
        return "ID map has an unsupported extent layout";
    for (const auto& extent : plan.uids)
        if (!validExtent(extent)) return "UID map contains an invalid extent";
    for (const auto& extent : plan.gids)
        if (!validExtent(extent)) return "GID map contains an invalid extent";
    if (!validRoot(plan.uids.front()) || !validRoot(plan.gids.front()))
        return "ID map must map container ID 0 exactly once";
    if (plan.mode == IdMapMode::Rich) {
        if (plan.uids[1].inside != 1 || plan.gids[1].inside != 1 ||
            plan.uids[1].count != plan.gids[1].count ||
            plan.uids[1].count > 65535)
            return "rich ID map must use one contiguous subordinate extent";
        if (overlaps(plan.uids[0], plan.uids[1]) ||
            overlaps(plan.gids[0], plan.gids[1]))
            return "rich ID map contains overlapping host extents";
    }
    return {};
}

unsigned int subordinateIdCount(const IdMapPlan& plan) {
    return plan.mode == IdMapMode::Rich && plan.uids.size() == 2
               ? plan.uids[1].count
               : 0;
}

} // namespace util
