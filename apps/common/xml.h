/*
 * XML as events, one at a time (embedded-xml's reader): what is between a '<' and its '>', and
 * the text between those. Everything is a slice of the text, nothing is copied or decoded
 * (entities stay as written) or checked: UTF-8, no DTD, unclosed elements go unnoticed, text
 * keeps its whitespace. A self-closing element is its XML_START, then its XML_CLOSE.
 */
#pragma once

#include "str.h"

typedef enum { XML_END, XML_PI, XML_DTD, XML_CDATA, XML_COMMENT, XML_START, XML_TEXT, XML_CLOSE } xml_event;

typedef struct {
  str rest;
  str name;    /* XML_PI, XML_START, XML_CLOSE */
  str attrs;   /* XML_PI, XML_START: for xml_attr */
  str content; /* XML_DTD, XML_CDATA, XML_COMMENT, XML_TEXT */
  str closing; /* a self-closing element: its XML_CLOSE comes next */
} xml_reader;

static inline xml_reader xml_init(str text) { return (xml_reader){.rest = text}; }
xml_event xml_next(xml_reader *reader);
/* The next of `attrs` (name="value", 'value' or value), `*attrs` moved past it: false after the last */
bool xml_attr(str *attrs, str *name, str *value);
/* The value of the attribute `name`, any case */
bool xml_attr_get(str attrs, str name, str *value);
