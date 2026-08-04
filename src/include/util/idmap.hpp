#pragma once

#include <string>
#include <sys/types.h>
#include <vector>

namespace util {

enum class IdMapMode { Single, Rich, Native };

struct IdMapExtent {
    unsigned int inside = 0;
    unsigned int outside = 0;
    unsigned int count = 0;
};

inline bool operator==(const IdMapExtent& left, const IdMapExtent& right) {
    return left.inside == right.inside && left.outside == right.outside &&
           left.count == right.count;
}

struct IdMapPlan {
    IdMapMode mode = IdMapMode::Single;
    std::vector<IdMapExtent> uids;
    std::vector<IdMapExtent> gids;
    std::string source;
};

IdMapPlan makeSingleIdMap(uid_t uid, gid_t gid);
IdMapPlan makeRichIdMap(uid_t uid, gid_t gid, unsigned int uidStart,
                        unsigned int gidStart, unsigned int count,
                        std::string source);
IdMapPlan makeNativeIdMap();
std::string validateIdMapPlan(const IdMapPlan& plan);
unsigned int subordinateIdCount(const IdMapPlan& plan);

} // namespace util
