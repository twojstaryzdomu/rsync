#include "dfasearch.c"
#include "pcresearch.c"
#include "kwsearch.c"

static void
Gcompile (char const *pattern, size_t size)
{
  return GEAcompile (pattern, size,
		     RE_SYNTAX_GREP
		     | RE_HAT_LISTS_NOT_NEWLINE
		     | RE_NO_EMPTY_RANGES);
}

static void
Ecompile (char const *pattern, size_t size)
{
  return GEAcompile (pattern, size, RE_SYNTAX_POSIX_EGREP | RE_NO_EMPTY_RANGES);
}

static void
Acompile (char const *pattern, size_t size)
{
  return GEAcompile (pattern, size, RE_SYNTAX_AWK);
}

struct matcher const matchers[] = {
  { "grep",    Gcompile, EGexecute },
  { "egrep",   Ecompile, EGexecute },
  { "awk",     Acompile, EGexecute },
  { "fgrep",   Fcompile, Fexecute },
  { "perl",    Pcompile, Pexecute },
  { NULL, NULL, NULL },
};