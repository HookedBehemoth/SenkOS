/* xml.h on every kind of event, a broken document and attributes (embedded-xml's tests) */
#include <assert.h>

#include "xml.h"

static void next(xml_reader *r, xml_event event, str name) {
  const xml_event got = xml_next(r);
  assert(got == event);
  assert(str_eq(event == XML_START || event == XML_CLOSE || event == XML_PI ? r->name : r->content, name));
}

static void attr(str attrs, str name, str value) {
  str v;
  assert(xml_attr_get(attrs, name, &v) && str_eq(v, value));
}

int main(void) {
  xml_reader r = xml_init(S("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                            "<root attr1=\"value1\" attr2=\"value2\">"
                            "<child>Text</child>"
                            "<?example processing=\"instructions\"?>"
                            "<child>More text</child>"
                            "<![CDATA[doesn't need to be \xff utf-8 encoded]]>\n"
                            "<!-- a comment -->"
                            "<self-closing />"
                            "<self-closing/>"
                            "</root>"));
  next(&r, XML_PI, S("xml"));
  attr(r.attrs, S("version"), S("1.0"));
  attr(r.attrs, S("encoding"), S("UTF-8"));
  attr(r.attrs, S("standalone"), S("yes"));
  next(&r, XML_START, S("root"));
  attr(r.attrs, S("attr1"), S("value1"));
  attr(r.attrs, S("attr2"), S("value2"));
  next(&r, XML_START, S("child"));
  next(&r, XML_TEXT, S("Text"));
  next(&r, XML_CLOSE, S("child"));
  next(&r, XML_PI, S("example"));
  str attrs = r.attrs, n, v;
  assert(xml_attr(&attrs, &n, &v) && str_eq(n, S("processing")) && str_eq(v, S("instructions")));
  assert(!xml_attr(&attrs, &n, &v));
  next(&r, XML_START, S("child"));
  next(&r, XML_TEXT, S("More text"));
  next(&r, XML_CLOSE, S("child"));
  next(&r, XML_CDATA, S("doesn't need to be \xff utf-8 encoded"));
  next(&r, XML_COMMENT, S("a comment"));
  next(&r, XML_START, S("self-closing"));
  next(&r, XML_CLOSE, S("self-closing"));
  next(&r, XML_START, S("self-closing"));
  next(&r, XML_CLOSE, S("self-closing"));
  next(&r, XML_CLOSE, S("root"));
  next(&r, XML_END, S(""));

  r = xml_init(S("This isn't actually an XML document"));
  next(&r, XML_END, S(""));
  /* Unclosed elements aren't noticed */
  r = xml_init(S("<?xml?><unclosed><child>Text</child>"));
  next(&r, XML_PI, S("xml"));
  next(&r, XML_START, S("unclosed"));
  next(&r, XML_START, S("child"));
  next(&r, XML_TEXT, S("Text"));
  next(&r, XML_CLOSE, S("child"));
  next(&r, XML_END, S(""));
  /* Cut short inside a tag */
  r = xml_init(S("<root><chi"));
  next(&r, XML_START, S("root"));
  next(&r, XML_END, S(""));

  attrs = S("foo=\"bar\" baz='qux' bare=1");
  attr(attrs, S("FOO"), S("bar"));
  attr(attrs, S("baz"), S("qux"));
  attr(attrs, S("bare"), S("1"));
  assert(!xml_attr_get(attrs, S("nonexistent"), &v));
  attr(S("style=\"text-align: center\""), S("style"), S("text-align: center"));
  return 0;
}
