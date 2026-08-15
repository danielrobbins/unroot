#include <cstdlib>
#if __has_include(<shadow/subid.h>)
#include <shadow/subid.h>
#elif __has_include(<subid.h>)
#include <subid.h>
#else
#error "libsubid header unavailable"
#endif

int main() {
  struct subid_range* ranges = nullptr;
  if (!subid_init("probe", nullptr)) return 1;
  const int count = subid_get_uid_ranges("0", &ranges);
  std::free(ranges);
  return count < 0;
}
