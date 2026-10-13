#include "jar.h"

#include <stdlib.h>
#include <string.h>

#include "zip.h"

/* The manifest's `key` (case aside), its continuation lines (" more") joined */
static bool attribute(str manifest, str key, char *out, size_t size) {
  size_t n = 0;
  bool found = false;
  out[0] = '\0';
  while (manifest.n > 0) {
    str line = str_cut(&manifest, '\n');
    if (line.n > 0 && line.p[line.n - 1] == '\r')
      line.n--;
    if (found && line.n > 0 && line.p[0] == ' ')
      str_put(out, size, &n, &(str){line.p + 1, line.n - 1}, 1);
    else if (found)
      break;
    else if ((found = line.n > key.n && line.p[key.n] == ':' && str_ieq(str_head(line, key.n), key)))
      str_put(out, size, &n, &(str){line.p + key.n + 1, line.n - key.n - 1}, 1);
  }
  const str value = str_trim(str_c(out));
  memmove(out, value.p, value.n);
  out[value.n] = '\0';
  return value.n > 0;
}

/* Field `i` of "a, b, c", trimmed */
static str field(str s, int i) {
  str f = str_cut(&s, ',');
  while (i-- > 0)
    f = str_cut(&s, ',');
  return str_trim(f);
}

static bool manifest_attribute(const char *path, str key, char *out, size_t size) {
  size_t n = 0;
  uint8_t *manifest = zip_read(path, S("META-INF/MANIFEST.MF"), &n);
  const bool ok = manifest != NULL && attribute((str){(const char *)manifest, n}, key, out, size);
  free(manifest);
  return ok;
}

static bool utf8(const char *s) {
  for (const uint8_t *p = (const uint8_t *)s; *p;) {
    int more = *p < 0x80 ? 0 : (*p & 0xe0) == 0xc0 ? 1 : (*p & 0xf0) == 0xe0 ? 2 : (*p & 0xf8) == 0xf0 ? 3 : -1;
    if (more < 0)
      return false;
    for (p++; more > 0 && more--; p++)
      if ((*p & 0xc0) != 0x80)
        return false;
  }
  return true;
}

bool jar_name(const char *path, char *out, size_t size) {
  char midlet[256];
  if (!manifest_attribute(path, S("MIDlet-Name"), out, size)) {
    if (!manifest_attribute(path, S("MIDlet-1"), midlet, sizeof(midlet)))
      return false;
    str_copy(out, size, field(str_c(midlet), 0));
  }
  if (utf8(out))
    return out[0] != '\0';
  out[0] = '\0';
  return false;
}

uint8_t *jar_icon(const char *path, size_t *size) {
  char icon[256];
  str name = S("");
  if (manifest_attribute(path, S("MIDlet-Icon"), icon, sizeof(icon)))
    name = str_c(icon);
  else if (manifest_attribute(path, S("MIDlet-1"), icon, sizeof(icon)))
    name = field(str_c(icon), 1);
  while (name.n > 0 && name.p[0] == '/')
    name = str_at(name, 1);
  return name.n > 0 ? zip_read(path, name, size) : NULL;
}
