#if defined(UNROOT_PROBE_LIBSUBID)

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

#elif defined(UNROOT_PROBE_LIBARCHIVE)

#include <archive.h>

int main() {
  struct archive* archive = archive_read_new();
  return archive_read_free(archive);
}

#else
#error "optional library probe not selected"
#endif
