#include "xml.h"

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }

static str trim(str s) {
  while (s.n > 0 && is_space(s.p[0]))
    s.p++, s.n--;
  while (s.n > 0 && is_space(s.p[s.n - 1]))
    s.n--;
  return s;
}

/* Index of the first whitespace; s.n without one */
static size_t find_space(str s) {
  size_t i = 0;
  while (i < s.n && !is_space(s.p[i]))
    i++;
  return i;
}

static size_t find(str s, str needle) {
  const char *at = memmem(s.p, s.n, needle.p, needle.n);
  return at != NULL ? (size_t)(at - s.p) : s.n;
}

static void name_and_attrs(xml_reader *r, str block) {
  const size_t space = find_space(block);
  r->name = str_head(block, space);
  r->attrs = trim(str_at(block, space + 1));
}

xml_event xml_next(xml_reader *r) {
  r->name = r->attrs = r->content = (str){0};
  if (r->closing.n > 0) {
    r->name = str_head(r->closing, find_space(r->closing));
    r->closing = (str){0};
    return XML_CLOSE;
  }
  const size_t lt = str_find(r->rest, '<');
  if (lt == r->rest.n) {
    r->rest = str_at(r->rest, r->rest.n);
    return XML_END;
  }
  if (trim(str_head(r->rest, lt)).n > 0) {
    r->content = str_head(r->rest, lt);
    r->rest = str_at(r->rest, lt);
    return XML_TEXT;
  }
  r->rest = str_at(r->rest, lt);
  if (r->rest.n < 3)
    return XML_END;

  static const struct {
    char c1, c2; /* after the '<'; 0: any */
    xml_event event;
    str open, close;
  } kinds[] = {
      {'!', '[', XML_CDATA, S("<![CDATA["), S("]]>")}, {'!', '-', XML_COMMENT, S("<!--"), S("-->")},
      {'!', 0, XML_DTD, S("<!"), S(">")},              {'?', 0, XML_PI, S("<?"), S("?>")},
      {'/', 0, XML_CLOSE, S("</"), S(">")},            {0, 0, XML_START, S("<"), S(">")},
  };
  size_t k = 0;
  while ((kinds[k].c1 != 0 && kinds[k].c1 != r->rest.p[1]) || (kinds[k].c2 != 0 && kinds[k].c2 != r->rest.p[2]))
    k++;
  const size_t start = find(r->rest, kinds[k].open) + kinds[k].open.n;
  const size_t end = start <= r->rest.n ? start + find(str_at(r->rest, start), kinds[k].close) : r->rest.n;
  if (start > r->rest.n || end == r->rest.n) {
    r->rest = str_at(r->rest, r->rest.n);
    return XML_END;
  }
  str block = {r->rest.p + start, end - start};
  if (kinds[k].event == XML_START && block.n > 0 && block.p[block.n - 1] == '/') {
    block.n--;
    r->closing = trim(block);
  }
  block = trim(block);
  r->rest = str_at(r->rest, end + kinds[k].close.n);
  switch (kinds[k].event) {
  case XML_PI:
  case XML_START:
    name_and_attrs(r, block);
    break;
  case XML_CLOSE:
    r->name = block;
    break;
  default:
    r->content = block;
  }
  return kinds[k].event;
}

bool xml_attr(str *attrs, str *name, str *value) {
  str s = trim(*attrs);
  const size_t eq = str_find(s, '=');
  if (s.n == 0 || eq == s.n || eq + 1 == s.n)
    return false;
  *name = str_head(s, eq);
  s = str_at(s, eq + 1);
  const char quote = s.p[0];
  if (quote == '"' || quote == '\'') {
    s = str_at(s, 1);
    const size_t end = str_find(s, quote);
    if (end == s.n)
      return false;
    *value = str_head(s, end);
    *attrs = str_at(s, end + 1);
  } else {
    *value = str_head(s, find_space(s));
    *attrs = str_at(s, value->n);
  }
  return true;
}

bool xml_attr_get(str attrs, str name, str *value) {
  for (str n; xml_attr(&attrs, &n, value);)
    if (str_ieq(n, name))
      return true;
  return false;
}
