/*
 * Copyright(c) 2021 ADLINK Technology Limited and others
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v. 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
 * v. 1.0 which is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <inttypes.h>

#include "idl/string.h"
#include "idl/processor.h"
#include "idl/print.h"
#include "idl/stream.h"

#include "generator.h"

#define CHUNK (4096)

static idl_retcode_t vputf(idl_buffer_t *buf, const char *fmt, va_list ap)
{
  va_list aq;
  int cnt;
  char str[1], *data = str;
  size_t size = 0;

  assert(buf);
  assert(fmt);

  va_copy(aq, ap);
  if (buf->data && (size = (buf->size - buf->used)) > 0)
    data = buf->data + buf->used;
  cnt = idl_vsnprintf(data, size+1, fmt, aq);
  va_end(aq);

  if (cnt >= 0 && size <= (size_t)cnt) {
    size = buf->size + ((((size_t)cnt - size) / CHUNK) + 1) * CHUNK;
    if (!(data = realloc(buf->data, size+1)))
      return IDL_RETCODE_NO_MEMORY;
    buf->data = data;
    buf->size = size;
    cnt = idl_vsnprintf(buf->data + buf->used, size, fmt, ap);
  }

  if (cnt < 0)
    return IDL_RETCODE_NO_MEMORY;
  buf->used += (size_t)cnt;
  return IDL_RETCODE_OK;
}

static idl_retcode_t putf(idl_buffer_t *buf, const char *fmt, ...)
{
  va_list ap;
  idl_retcode_t ret;

  va_start(ap, fmt);
  ret = vputf(buf, fmt, ap);
  va_end(ap);
  return ret;
}

static int get_array_accessor(char* str, size_t size, const void* node, void* user_data)
{
  (void)node;
  uint32_t depth = *((uint32_t*)user_data);
  return idl_snprintf(str, size, "a_%u", depth);
}

struct sequence_holder {
  const char* sequence_accessor;
  size_t depth;
};
typedef struct sequence_holder sequence_holder_t;

static int get_sequence_member_accessor(char* str, size_t size, const void* node, void* user_data)
{
  (void)node;
  sequence_holder_t* sh = (sequence_holder_t*)user_data;
  static const char *fmt = "%1$s[i_%2$u]";
  return idl_snprintf(str, size, fmt, sh->sequence_accessor, (uint32_t)sh->depth);
}

enum instance_mask {
  TYPEDEF           = 0x1 << 0,
  UNION_BRANCH      = 0x1 << 1,
  SEQUENCE          = 0x1 << 2,
  ARRAY             = 0x1 << 3,
  OPTIONAL          = 0x1 << 4,
  EXTERNAL          = 0x1 << 5
};

struct instance_location {
  char *parent;
  uint32_t type;
};
typedef struct instance_location instance_location_t;

static int get_instance_accessor(char* str, size_t size, const void* node, void* user_data)
{
  instance_location_t loc = *(instance_location_t *)user_data;

  if (loc.type & TYPEDEF) {
    return idl_snprintf(str, size, "%s", loc.parent);
  } else {
    const char *fmt = "%s.%s()";
    if (loc.type & EXTERNAL)
      fmt = "(*%s.%s())";
    else if (loc.type & OPTIONAL)
      fmt = "%s.%s().value()";

    const idl_declarator_t* decl = (const idl_declarator_t*)node;
    const char* name = get_cpp11_name(decl);
    return idl_snprintf(str, size, fmt, loc.parent, name);
  }
}

struct streams {
  struct generator *generator;
  idl_buffer_t write;
  idl_buffer_t read;
  idl_buffer_t move;
  idl_buffer_t max;
  idl_buffer_t props;
};

static void setup_streams(struct streams* str, struct generator* gen)
{
  assert(str);
  memset(str, 0, sizeof(struct streams));
  str->generator = gen;
}

static void cleanup_streams(struct streams* str)
{
  if (str->write.data)
    free(str->write.data);
  if (str->read.data)
    free(str->read.data);
  if (str->move.data)
    free(str->move.data);
  if (str->max.data)
    free(str->max.data);
  if (str->props.data)
    free(str->props.data);
}

static idl_retcode_t flush_stream(idl_buffer_t* str, FILE* f)
{
  if (str->data && fputs(str->data, f) < 0)
    return IDL_RETCODE_NO_MEMORY;
  if (str->size &&
      str->data)
      str->data[0] = '\0';
  str->used = 0;

  return IDL_RETCODE_OK;
}

static idl_retcode_t flush(struct generator* gen, struct streams* streams)
{
  if (IDL_RETCODE_OK != flush_stream(&streams->props, gen->impl.handle)
   || IDL_RETCODE_OK != flush_stream(&streams->write, gen->header.handle)
   || IDL_RETCODE_OK != flush_stream(&streams->read, gen->header.handle)
   || IDL_RETCODE_OK != flush_stream(&streams->move, gen->header.handle)
   || IDL_RETCODE_OK != flush_stream(&streams->max, gen->header.handle))
    return IDL_RETCODE_NO_MEMORY;

  return IDL_RETCODE_OK;
}

#define WRITE (1u<<0)
#define READ (1u<<1)
#define MOVE (1u<<2)
#define MAX (1u<<3)
#define CONST (WRITE | MOVE | MAX)
#define ALL (CONST | READ)
#define NOMAX (ALL & ~MAX)

//mapping of streaming flags and token replacements
static const char tokens[2] = {'T', 'C'};

static struct { uint32_t id; size_t O; const char *token_replacements[2]; } map[] = {
  { WRITE, offsetof(struct streams, write), {"write", "const "} },
  { READ,  offsetof(struct streams, read),  {"read",  ""} },
  { MOVE,  offsetof(struct streams, move),  {"move",  "const "} },
  { MAX,   offsetof(struct streams, max),   {"max",   "const "} }
};

/* scan over string looking for {tok} */
static idl_retcode_t print_until_token(struct streams *out, uint32_t mask, const char *fmt, size_t *fmt_position)
{
  int err = 0;
  size_t start_pos = *fmt_position;
  bool end_found = false;
  while (!end_found) {
    if (fmt[*fmt_position] == '\0') {
      end_found = true;
    }
    if (fmt[*fmt_position] == '{') {
      for (size_t i = 0, n = sizeof(tokens)/sizeof(tokens[0]); i < n; i++)
        if (fmt[*fmt_position+1] == tokens[i])
          end_found = true;
    }
    if (!end_found)
      (void)(*fmt_position)++;
  }

  size_t sub_len = *fmt_position-start_pos;
  if (sub_len) {
    char *substring = NULL;
    if ((substring = malloc(sub_len+1))) {
      memcpy(substring, fmt+start_pos, sub_len);
      substring[sub_len] = '\0';

      for (uint32_t i=0, n=(sizeof(map)/sizeof(map[0])); i < n && !err; i++) {
        if (!(map[i].id & mask))
          continue;

        if (putf((idl_buffer_t*)((char*)out+map[i].O), substring))
          err = 1;
      }
      free (substring);
    } else {
      err = 1;
    }
  }

  return err ? IDL_RETCODE_NO_MEMORY : IDL_RETCODE_OK;
}

static idl_retcode_t replace_token(struct streams *out, uint32_t mask, const char *fmt, size_t *fmt_position) {
  int err = 0;

  for (size_t i = 0, ntoks = sizeof(tokens)/sizeof(tokens[0]); i < ntoks && !err; i++) {
    if (fmt[*fmt_position+1] != tokens[i])
      continue;

    for (uint32_t j=0, n=(sizeof(map)/sizeof(map[0])); j < n && !err; j++) {
      if (!(map[j].id & mask))
        continue;

      if (putf((idl_buffer_t*)((char*)out+map[j].O), map[j].token_replacements[i]))
        err = 1;
    }
  }

  *fmt_position += 3;
  return err ? IDL_RETCODE_NO_MEMORY : IDL_RETCODE_OK;
}

static idl_retcode_t multi_putf(struct streams *out, uint32_t mask, const char *fmt, ...)
{
  char *withtokens = NULL;
  size_t tlen;
  va_list ap, aq;
  int err = 0;

  va_start(ap, fmt);
  va_copy(aq, ap);
  int cnt = idl_vsnprintf(withtokens, 0, fmt, aq);
  va_end(aq);
  if (cnt >= 0) {
    tlen = (size_t)cnt;
    if (tlen != SIZE_MAX) {
      withtokens = malloc(tlen + 1u);
      if (withtokens) {
        cnt = idl_vsnprintf(withtokens, tlen + 1u, fmt, ap);
        err = ((size_t)cnt != tlen);
      } else {
        err = 1;
      }
    } else {
      err = 1;
    }
  } else {
    err = 1;
  }
  va_end(ap);

  if (!err) {
    size_t str_pos = 0;
    while (!err && withtokens[str_pos]) {
      if (print_until_token(out, mask, withtokens, &str_pos))
        err = 1;
      if (!err && withtokens[str_pos]) {
        if (replace_token(out, mask, withtokens, &str_pos))
          err = 1;
      }
    }
  }

  if (withtokens)
    free(withtokens);

  return err ? IDL_RETCODE_NO_MEMORY : IDL_RETCODE_OK;
}

static idl_retcode_t
write_string_streaming_functions(
  struct streams* streams,
  const idl_type_spec_t* type_spec,
  const char* accessor,
  const char* read_accessor)
{
  uint32_t maximum = ((const idl_string_t*)type_spec)->maximum;

  static const char* fmt =
    "      if (!{T}_string(streamer, %1$s, %2$"PRIu32"))\n"
    "        return false;\n";

  if (multi_putf(streams, CONST, fmt, accessor, maximum)
   || multi_putf(streams, READ, fmt, read_accessor, maximum))
    return IDL_RETCODE_NO_MEMORY;

  return IDL_RETCODE_OK;
}

static idl_retcode_t
write_typedef_streaming_functions(
  struct streams* streams,
  const idl_type_spec_t* type_spec,
  const char* accessor,
  const char* read_accessor)
{
  static const char* fmt =
    "      if (!{T}_%1$s(streamer, %2$s))\n"
    "        return false;\n";
  char* name = NULL;
  if (IDL_PRINTA(&name, get_cpp11_name_typedef, type_spec, streams->generator) < 0
   || multi_putf(streams, CONST, fmt, name, accessor)
   || multi_putf(streams, READ, fmt, name, read_accessor))
    return IDL_RETCODE_NO_MEMORY;

  return IDL_RETCODE_OK;
}

static idl_retcode_t
write_constructed_type_streaming_functions(
  struct streams* streams,
  const char* accessor,
  const char* read_accessor)
{
  static const char* fmt =
    "      if (!{T}(streamer, %1$s, prop))\n"
    "        return false;\n";

  if (multi_putf(streams, CONST, fmt, accessor)
   || multi_putf(streams, READ, fmt, read_accessor))
    return IDL_RETCODE_NO_MEMORY;

  return IDL_RETCODE_OK;
}

static idl_retcode_t
write_base_type_streaming_functions(
  struct streams* streams,
  const idl_type_spec_t* type_spec,
  const char* accessor,
  const char* read_accessor,
  instance_location_t loc)
{
  const char* fmt = idl_is_enum(type_spec) ?
    "      if (!{T}(streamer, %1$s, prop))\n"
    "        return false;\n" :
    "      if (!{T}(streamer, %1$s))\n"
    "        return false;\n";
  const char* rfmt = fmt;

  if (loc.type & SEQUENCE
   && idl_mask(type_spec) == IDL_BOOL) {
    rfmt =
      "      {\n"
      "        bool b(false);\n"
      "        if (!{T}(streamer, b))\n"
      "          return false;\n"
      "        %1$s = b;\n"
      "      }\n";

    fmt =
      "      {\n"
      "        if (!{T}(streamer, bool(%1$s)))\n"
      "          return false;\n"
      "      }\n";
  }

  if (multi_putf(streams, CONST, fmt, accessor)
   || multi_putf(streams, READ, rfmt, read_accessor))
    return IDL_RETCODE_NO_MEMORY;

  return IDL_RETCODE_OK;
}

static idl_retcode_t
write_streaming_functions(
  struct streams* streams,
  const idl_type_spec_t* type_spec,
  const char* accessor,
  const char* read_accessor,
  instance_location_t loc)
{
  if (idl_is_alias(type_spec))
    return write_typedef_streaming_functions(streams, type_spec, accessor, read_accessor);
  else if (idl_is_string(type_spec))
    return write_string_streaming_functions(streams, type_spec, accessor, read_accessor);
  else if (idl_is_union(type_spec) || idl_is_struct(type_spec))
    return write_constructed_type_streaming_functions(streams, accessor, read_accessor);
  else
    return write_base_type_streaming_functions(streams, type_spec, accessor, read_accessor, loc);
}

static idl_retcode_t
unroll_sequence(const idl_pstate_t* pstate,
  struct streams* streams,
  const idl_sequence_t* seq,
  size_t depth,
  const char* accessor,
  const char* read_accessor,
  instance_location_t loc);

static idl_retcode_t
sequence_writes(const idl_pstate_t* pstate,
  struct streams* streams,
  const idl_sequence_t* seq,
  size_t depth,
  const char* accessor,
  const char* read_accessor,
  instance_location_t loc)
{
  const idl_type_spec_t *type_spec = seq->type_spec;

  if ((idl_is_base_type(type_spec) || idl_is_enum(type_spec))
    && (idl_mask(type_spec) & IDL_BOOL) != IDL_BOOL) {

    const char *e_prop = idl_is_enum(type_spec) ? ", prop" : "";
    const char* sfmt = "      if (se_%3$u > 0 &&\n"
                       "          !{T}(streamer, %1$s[0]%2$s, se_%3$u))\n"
                       "        return false;\n";
    const char* mfmt = "      if (se_%3$u > 0 &&\n"
                       "          !{T}(streamer, %1$s()%2$s, se_%3$u))\n"
                       "        return false;\n";
    char* type = NULL;

    if (IDL_PRINTA(&type, get_cpp11_type, type_spec, streams->generator) < 0
      || multi_putf(streams, MOVE | MAX, mfmt, type, e_prop, depth)
      || multi_putf(streams, WRITE, sfmt, accessor, e_prop, depth)
      || multi_putf(streams, READ, sfmt, read_accessor, e_prop, depth))
        return IDL_RETCODE_NO_MEMORY;

    return IDL_RETCODE_OK;
  }

  static const char* fmt = "      for (uint32_t i_%1$u = 0; i_%1$u < se_%1$u; i_%1$u++) {\n";
  if (multi_putf(streams, ALL, fmt, depth, ""))
    return IDL_RETCODE_NO_MEMORY;

  sequence_holder_t sh = (sequence_holder_t){ .sequence_accessor = accessor, .depth = depth};
  char* new_accessor = NULL;
  if (IDL_PRINTA(&new_accessor, get_sequence_member_accessor, &sh, &sh) < 0)
    return IDL_RETCODE_NO_MEMORY;

  sh.sequence_accessor = read_accessor;
  char* new_read_accessor = NULL;
  if (IDL_PRINTA(&new_read_accessor, get_sequence_member_accessor, &sh, &sh) < 0)
    return IDL_RETCODE_NO_MEMORY;

  loc.type |= SEQUENCE;

  if (idl_is_sequence(type_spec)) {
    if (unroll_sequence (pstate, streams, (idl_sequence_t*)type_spec, depth + 1, new_accessor, new_read_accessor, loc))
      return IDL_RETCODE_NO_MEMORY;
  } else {
    if (write_streaming_functions (streams, type_spec, new_accessor, new_read_accessor, loc))
      return IDL_RETCODE_NO_MEMORY;
  }

  static const char* cfmt = "      }  //i_%1$u\n";
  if (multi_putf(streams, ALL, cfmt, depth))
    return IDL_RETCODE_NO_MEMORY;

  return IDL_RETCODE_OK;
}

idl_retcode_t
unroll_sequence(const idl_pstate_t* pstate,
  struct streams* streams,
  const idl_sequence_t* seq,
  size_t depth,
  const char* accessor,
  const char* read_accessor,
  instance_location_t loc)
{
  uint32_t maximum = seq->maximum;

  static const char *consec_start_fmt =
      "      if (!streamer.start_consecutive(false, %1$s))\n"
      "        return false;\n";
  static const char *consec_finish_fmt =
      "      if (!streamer.finish_consecutive())\n"
      "        return false;\n";
  static const char* fmt1 =
    "      {\n"
    "      uint32_t se_%1$u = uint32_t(%2$s.size());\n";
  static const char* length_check =
    "      if (se_%1$u > %2$u &&\n"
    "          streamer.status(serialization_status::{T}_bound_exceeded))\n"
    "        return false;\n";
  static const char* fmt2 =
    "      if (!{T}(streamer, se_%1$u))\n"
    "        return false;\n";
  static const char* rfmt =
    "      %1$s.resize(se_%2$u);\n";
  static const char* mfmt =
    "      {\n"
    "      uint32_t se_%1$u = %2$u;\n";

  const idl_type_spec_t *root_type_spec = idl_strip(seq->type_spec, IDL_STRIP_ALIASES | IDL_STRIP_FORWARD);
  if (multi_putf(streams, ALL, consec_start_fmt, idl_is_base_type(root_type_spec) && !idl_is_array(root_type_spec) ? "true" : "false"))
    return IDL_RETCODE_NO_MEMORY;

  if (multi_putf(streams, READ, fmt1, depth, read_accessor)
   || multi_putf(streams, (WRITE | MOVE), fmt1, depth, accessor)
   || multi_putf(streams, MAX, mfmt, depth, maximum)
   || (maximum && multi_putf(streams, NOMAX, length_check, depth, maximum))
   || multi_putf(streams, ALL, fmt2, depth)
   || multi_putf(streams, READ, rfmt, read_accessor, depth))
    return IDL_RETCODE_NO_MEMORY;

  if (sequence_writes(pstate, streams, seq, depth, accessor, read_accessor, loc))
    return IDL_RETCODE_NO_MEMORY;

  //close sequence
  if (multi_putf(streams, ALL, "      }  //end sequence %1$lu\n", depth))
    return IDL_RETCODE_NO_MEMORY;

  if (multi_putf(streams, ALL, consec_finish_fmt))
    return IDL_RETCODE_NO_MEMORY;

  if (maximum == 0
   && putf(&streams->max, "      streamer.position(SIZE_MAX);\n"))
    return IDL_RETCODE_NO_MEMORY;

  return IDL_RETCODE_OK;
}

static idl_retcode_t
insert_array_primitives_copy(
  struct streams *streams,
  const idl_type_spec_t *type_spec,
  const char *accessor,
  const char *read_accessor)
{
  const char *e_prop = idl_is_enum(type_spec) ? ", prop" : "";
  static const char *fmt =
    "      if (!{T}(streamer, %1$s[0]%2$s, %1$s.size()))\n"
    "        return false;\n";

  if (multi_putf(streams, CONST, fmt, accessor, e_prop) ||
      multi_putf(streams, READ, fmt, read_accessor, e_prop))
    return IDL_RETCODE_NO_MEMORY;

  return IDL_RETCODE_OK;
}

static idl_retcode_t
process_entity(
  const idl_pstate_t *pstate,
  struct streams *streams,
  const idl_declarator_t* declarator,
  const idl_type_spec_t* type_spec,
  instance_location_t loc)
{
  if (idl_is_array(declarator))
    loc.type |= ARRAY;
  if (idl_is_sequence(type_spec))
    loc.type |= SEQUENCE;

  char* accessor = NULL;
  if (IDL_PRINTA(&accessor, get_instance_accessor, declarator, &loc) < 0)
    return IDL_RETCODE_NO_MEMORY;

  static const char *consec_start_fmt =
      "      if (!streamer.start_consecutive(true, %1$s))\n"
      "        return false;\n";
  static const char *consec_finish_fmt =
      "      if (!streamer.finish_consecutive())\n"
      "        return false;\n";
  static const char *array_iterate1 =
       "      for ({C}auto & a_%1$u:%2$s) {  //array depth %1$u\n";
  static const char *array_iterate2 =
       "      for ({C}auto & a_%1$u:a_%2$u) {  //array depth %1$u\n";
  static const char *array_close =
       "      }  //array depth %1$u\n";

  const idl_type_spec_t *root_type_spec = idl_strip(type_spec, 0);
  const idl_type_spec_t *aliases_stripped = idl_strip(type_spec, IDL_STRIP_ALIASES | IDL_STRIP_FORWARD);

  const char* read_accessor;
  if (loc.type & UNION_BRANCH)
    read_accessor = "obj";
  else
    read_accessor = accessor;

  //unroll arrays
  uint32_t n_arr = 0;
  bool batch_copy = false;
  if (idl_is_array(declarator)) {
    const idl_literal_t* lit = (const idl_literal_t*)declarator->const_expr;
    if (multi_putf(streams, ALL, consec_start_fmt, idl_is_base_type(root_type_spec) ? "true" : "false"))
      return IDL_RETCODE_NO_MEMORY;

    while (lit) {
      const idl_literal_t* next = idl_next(lit);

      if (!next && (idl_is_base_type(aliases_stripped) || idl_is_enum(aliases_stripped))) {
        batch_copy = true;
        break;
      }

      if (n_arr == 0) {
        if (multi_putf(streams, CONST, array_iterate1, n_arr+1, accessor) ||
            multi_putf(streams, READ, array_iterate1, n_arr+1, read_accessor))  //write iteration over initial array
          return IDL_RETCODE_NO_MEMORY;
      } else {
        if (multi_putf(streams, ALL, array_iterate2, n_arr+1, n_arr))  //write iteration over deeper array
          return IDL_RETCODE_NO_MEMORY;
      }
      n_arr++;
      lit = next;
    }

    if (n_arr) {
      if (IDL_PRINTA(&accessor, get_array_accessor, declarator, &n_arr) < 0)  //update accessor to become "a_$n_arr$"
        return IDL_RETCODE_NO_MEMORY;
      read_accessor = accessor;
    }
  }

  if (batch_copy) {
    if (insert_array_primitives_copy(streams, aliases_stripped, accessor, read_accessor))
      return IDL_RETCODE_NO_MEMORY;
  } else if (idl_is_sequence(type_spec)) {
    //unroll sequences (if any)
    if (unroll_sequence(pstate, streams, (idl_sequence_t*)type_spec, 1, accessor, read_accessor, loc))
      return IDL_RETCODE_NO_MEMORY;
  } else {
    if (write_streaming_functions(streams, type_spec, accessor, read_accessor, loc))
      return IDL_RETCODE_NO_MEMORY;
  }

  while (n_arr) {
    if (multi_putf(streams, ALL, array_close, n_arr--))
      return IDL_RETCODE_NO_MEMORY;
  }

  if (idl_is_array(declarator))
    return multi_putf(streams, ALL, consec_finish_fmt);
  else
    return IDL_RETCODE_OK;
}

static idl_retcode_t
generate_entity_properties(
  const idl_node_t *parent,
  const idl_type_spec_t *type_spec,
  const idl_declarator_t *decl,
  struct streams *streams)
{
  while (idl_is_alias(type_spec) || idl_is_sequence(type_spec)) {
    if (idl_is_alias(type_spec))
      type_spec = idl_strip(type_spec, 0);
    if (idl_is_sequence(type_spec))
      type_spec = ((const idl_sequence_t*)type_spec)->type_spec;
  }

  const char *opt = is_optional(decl) ? "true" : "false";
  if (idl_is_struct(type_spec)
   || idl_is_union(type_spec)) {
    char *type = NULL;
    if (IDL_PRINTA(&type, get_cpp11_fully_scoped_name, type_spec, streams->generator) < 0)
      return IDL_RETCODE_NO_MEMORY;

    /* structs and unions need to set their properties as members through the set_member_props
     * function as they are copied from the static references of the class*/
    if (putf(&streams->props, "  props.m_members_by_seq.push_back(get_type_props<%1$s>());\n"
                              "  props.m_members_by_seq.back().set_member_props(%2$"PRIu32",%3$s);\n", type, decl->id.value, opt))
      return IDL_RETCODE_NO_MEMORY;
  } else {
    if (putf(&streams->props, "  props.m_members_by_seq.push_back(entity_properties_t(%1$"PRIu32",%2$s));\n", decl->id.value, opt))
      return IDL_RETCODE_NO_MEMORY;
  }

  switch (get_extensibility(parent)) {
    case IDL_APPENDABLE:
      if (putf(&streams->props, "  props.m_members_by_seq.back().p_ext = extensibility::ext_appendable;\n"))
        return IDL_RETCODE_NO_MEMORY;
      break;
    case IDL_MUTABLE:
      if (putf(&streams->props, "  props.m_members_by_seq.back().p_ext = extensibility::ext_mutable;\n"))
        return IDL_RETCODE_NO_MEMORY;
      break;
    default:
      break;
  }

  if (must_understand(type_spec) &&
      putf(&streams->props, "  props.m_members_by_seq.back().must_understand_local = true;\n"))
    return IDL_RETCODE_NO_MEMORY;

  switch (get_extensibility(type_spec)) {
    case IDL_APPENDABLE:
      if (putf(&streams->props, "  props.m_members_by_seq.back().e_ext = extensibility::ext_appendable;\n"))
        return IDL_RETCODE_NO_MEMORY;
      break;
    case IDL_MUTABLE:
      if (putf(&streams->props, "  props.m_members_by_seq.back().e_ext = extensibility::ext_mutable;\n"))
        return IDL_RETCODE_NO_MEMORY;
      break;
    default:
      break;
  }

  const char *bb = NULL;
  if (idl_is_base_type(type_spec)) {
    uint32_t tp = idl_mask(type_spec) & (IDL_BASE_TYPE*2-1);
    switch (tp) {
      case IDL_CHAR:
      case IDL_BOOL:
      case IDL_OCTET:
      case IDL_INT8:
      case IDL_UINT8:
        bb = "bb_8_bits";
        break;
      case IDL_SHORT:
      case IDL_USHORT:
      case IDL_INT16:
      case IDL_UINT16:
        bb = "bb_16_bits";
        break;
      case IDL_LONG:
      case IDL_ULONG:
      case IDL_INT32:
      case IDL_UINT32:
      case IDL_FLOAT:
        bb = "bb_32_bits";
        break;
      case IDL_LLONG:
      case IDL_ULLONG:
      case IDL_INT64:
      case IDL_UINT64:
      case IDL_DOUBLE:
        bb = "bb_64_bits";
        break;
  /*IDL_WCHAR:
  IDL_ANY:
  IDL_LDOUBLE:*/
    }
  } else if (idl_is_enum(type_spec)) {
    const idl_enum_t *en = type_spec;
    bb = "bb_32_bits";
    if (en->bit_bound.annotation) {
      if (en->bit_bound.value > 32) {
        bb = "bb_64_bits";
      } else if (en->bit_bound.value > 16) {
        bb = "bb_32_bits";
      } else if (en->bit_bound.value > 8) {
        bb = "bb_16_bits";
      } else {
        bb = "bb_8_bits";
      }
    }
  } else if (idl_is_bitmask(type_spec)) {
    const idl_bitmask_t *bm = type_spec;
    bb = "bb_32_bits";
    if (bm->bit_bound.annotation) {
      if (bm->bit_bound.value > 32) {
        bb = "bb_64_bits";
      } else if (bm->bit_bound.value > 16) {
        bb = "bb_32_bits";
      } else if (bm->bit_bound.value > 8) {
        bb = "bb_16_bits";
      } else {
        bb = "bb_8_bits";
      }
    }
  }

  if (bb && putf(&streams->props, "  props.m_members_by_seq.back().e_bb = %1$s;\n", bb))
    return IDL_RETCODE_NO_MEMORY;

  return IDL_RETCODE_OK;
}

static idl_retcode_t
add_member_start(
  const idl_declarator_t *decl,
  struct streams *streams)
{
  instance_location_t loc = {.parent = "instance"};
  char *accessor = NULL;
  char *type = NULL;

  const idl_type_spec_t *type_spec = NULL;
  if (idl_is_array(decl))
    type_spec = decl;
  else
    type_spec = idl_type_spec(decl);

  if (IDL_PRINTA(&accessor, get_instance_accessor, decl, &loc) < 0
   || IDL_PRINTA(&type, get_cpp11_type, type_spec, streams->generator) < 0)
    return IDL_RETCODE_NO_MEMORY;

  if (multi_putf(streams, ALL, "      if (!streamer.start_member(prop"))
    return IDL_RETCODE_NO_MEMORY;

  if (is_external(decl)) {
    if (multi_putf(streams, ALL, "))\n        return false;\n", accessor)
     || multi_putf(streams, READ, "      if (!%1$s)\n"
                                  "        %1$s = std::make_shared<%2$s>();\n", accessor, type)
     || multi_putf(streams, (WRITE|MOVE), "      if (!%1$s)\n"
                                  "        return false;\n", accessor))
      return IDL_RETCODE_NO_MEMORY;
  } else if (is_optional(decl)) {
    if (multi_putf(streams, ALL, ", %1$s.has_value()))\n        return false;\n", accessor)
     || multi_putf(streams, (WRITE|MOVE), "      if (%1$s.has_value()) {\n", accessor)
     || multi_putf(streams, READ, "      %1$s = %2$s();\n", accessor, type))
      return IDL_RETCODE_NO_MEMORY;
  } else {
    if (multi_putf(streams, ALL, "))\n        return false;\n"))
      return IDL_RETCODE_NO_MEMORY;
  }

  return IDL_RETCODE_OK;
}

static idl_retcode_t
add_member_finish(
  const idl_declarator_t *decl,
  struct streams *streams)
{
  if (is_optional(decl)) {
    instance_location_t loc = {.parent = "instance"};
    char *accessor = NULL;
    if (IDL_PRINTA(&accessor, get_instance_accessor, decl, &loc) < 0
     || multi_putf(streams, (WRITE|MOVE), "      }\n")
     || multi_putf(streams, ALL,
          "      if (!streamer.finish_member(prop, %1$s.has_value()))\n"
          "        return false;\n", accessor))
      return IDL_RETCODE_NO_MEMORY;
  } else {
    if (multi_putf(streams, ALL, "      if (!streamer.finish_member(prop))\n"
                                 "        return false;\n"))
      return IDL_RETCODE_NO_MEMORY;
  }

  if (multi_putf(streams, ALL, "      break;\n"))
    return IDL_RETCODE_NO_MEMORY;

  return IDL_RETCODE_OK;
}

static idl_retcode_t
process_member(
  const idl_pstate_t* pstate,
  const bool revisit,
  const idl_path_t* path,
  const void* node,
  void* user_data)
{
  (void)revisit;
  (void)path;

  struct streams *streams = user_data;
  const idl_member_t *mem = node;
  const idl_declarator_t *declarator = NULL;
  const idl_type_spec_t *type_spec = mem->type_spec;

  IDL_FOREACH(declarator, mem->declarators) {
    //generate case
    static const char *fmt =
      "      case %"PRIu32":\n";

    if (multi_putf(streams, ALL, fmt, declarator->id.value)
     || add_member_start(declarator, streams))
      return IDL_RETCODE_NO_MEMORY;

    instance_location_t loc = {.parent = "instance"};
    if (is_external(mem))
      loc.type |= EXTERNAL;
    if (is_optional(mem))
      loc.type |= OPTIONAL;

    if (generate_entity_properties(mem->node.parent, type_spec, declarator, streams))
      return IDL_RETCODE_NO_MEMORY;

    // only use the @key annotations when you do not use the keylist
    if (!(pstate->config.flags & IDL_FLAG_KEYLIST) &&
        mem->key.value &&
        putf(&streams->props, "  keylist.add_key_endpoint(std::list<uint32_t>{%1$"PRIu32"});\n", declarator->id.value))
      return IDL_RETCODE_NO_MEMORY;

    if (process_entity(pstate, streams, declarator, type_spec, loc)
     || add_member_finish(declarator, streams))
      return IDL_RETCODE_NO_MEMORY;
  }

  return IDL_RETCODE_OK;
}

static idl_retcode_t
process_case(
  const idl_pstate_t* pstate,
  const bool revisit,
  const idl_path_t* path,
  const void* node,
  void* user_data)
{
  (void)path;

  struct streams *streams = user_data;
  const idl_case_t* _case = (const idl_case_t*)node;
  const idl_switch_type_spec_t* _switch = ((const idl_union_t*)_case->node.parent)->switch_type_spec;
  const idl_union_t* _union = (const idl_union_t*)_case->node.parent;
  bool single = (idl_degree(_case->labels) == 1) && !(idl_mask(_case->labels) == IDL_DEFAULT_CASE_LABEL),
       simple = (idl_is_base_type(_case->type_spec) || idl_is_bitmask(_case->type_spec)) && !idl_is_array(_case->declarator),
       constructed_type = idl_is_constr_type(_case->type_spec) && !idl_is_enum(_case->type_spec) && !idl_is_array(_case->declarator) && !idl_is_bitmask(_case->type_spec);
  instance_location_t loc = { .parent = "instance", .type = UNION_BRANCH };

  static const char *max_start =
    "  {\n"
    "    size_t pos = streamer.position();\n"
    "    size_t alignment = streamer.alignment();\n";
  static const char *max_end =
    "    if (union_max < streamer.position()) {\n"
    "      union_max = streamer.position();\n"
    "      alignment_max = streamer.alignment();\n"
    "    }\n"
    "    streamer.position(pos);\n"
    "    streamer.alignment(alignment);\n"
    "  }\n";

  const char* read_start = simple ? "    {\n"
                                    "      std::remove_cv_t<std::remove_reference_t<decltype(%1$s)>> obj = %2$s;\n"
                                  : "    {\n"
                                    "      std::remove_cv_t<std::remove_reference_t<decltype(%1$s)>> obj;\n";

  const char* read_end = single   ? "      instance.%1$s(obj);\n"
                                    "    }\n"
                                    "    break;\n"
                                  : "      instance.%1$s(obj, d);\n"
                                    "    }\n"
                                    "    break;\n";
  const char* get_props = constructed_type    ? "      auto prop = get_type_props<std::remove_cv_t<std::remove_reference_t<decltype(%1$s)>>>();\n"
                                              : "",
            * check_props = constructed_type  ? "      props.is_present = prop.is_present;\n"
                                              : "";

  if (revisit) {
    const char *name = get_cpp11_name(_case->declarator);

    char *accessor = NULL, *value = NULL;
    if (IDL_PRINTA(&accessor, get_instance_accessor, _case->declarator, &loc) < 0 ||
        (simple && IDL_PRINTA(&value, get_cpp11_default_value, _case->type_spec, streams->generator) < 0))
      return IDL_RETCODE_NO_MEMORY;

    if (multi_putf(streams, (WRITE | MOVE), "      {\n")
     || putf(&streams->read, read_start, accessor, value)
     || putf(&streams->max, max_start)
     || multi_putf(streams, ALL, get_props, accessor))
      return IDL_RETCODE_NO_MEMORY;

    //only read the field if the union is not read as a key stream
    if ((_switch->key.value && multi_putf(streams, ALL, "      if (!streamer.is_key()) {\n"))
     || process_entity(pstate, streams, _case->declarator, _case->type_spec, loc)
     || (_switch->key.value && multi_putf(streams, ALL, "      } //!streamer.is_key()\n")))
      return IDL_RETCODE_NO_MEMORY;

    if (multi_putf(streams, READ, check_props))
      return IDL_RETCODE_NO_MEMORY;

    if (multi_putf(streams, (WRITE | MOVE), "      break; }\n")
     || putf(&streams->read, read_end, name)
     || putf(&streams->max, max_end))
      return IDL_RETCODE_NO_MEMORY;

    if (idl_next(_case)) {
      return IDL_RETCODE_OK;
    } else {
      //if last entry, and no default case was present for this union
      const idl_case_label_t *def = _union->default_case;
      if (idl_is_union(def->node.parent) &&
          multi_putf(streams, READ, "    default:\n      instance._d(d);\n"))
        return IDL_RETCODE_NO_MEMORY;
    }

    if (multi_putf(streams, NOMAX, "  }\n"))
      return IDL_RETCODE_NO_MEMORY;
  } else {
    if (idl_previous(_case))
      return IDL_VISIT_REVISIT;
    if (multi_putf(streams, NOMAX,  "  switch(d)\n  {\n"))
      return IDL_RETCODE_NO_MEMORY;
    return IDL_VISIT_REVISIT;
  }

  return IDL_RETCODE_OK;
}

static const idl_declarator_t*
resolve_member(const idl_struct_t *type_spec, const char *member_name)
{
  type_spec = idl_strip(type_spec, IDL_STRIP_ALIASES | IDL_STRIP_FORWARD);

  if (idl_is_struct(type_spec)) {
    const idl_struct_t *_struct = (const idl_struct_t *)type_spec;
    const idl_member_t *member = NULL;
    const idl_declarator_t *decl = NULL;
    IDL_FOREACH(member, _struct->members) {
      IDL_FOREACH(decl, member->declarators) {
        if (0 == idl_strcasecmp(decl->name->identifier, member_name))
          return decl;
      }
    }
  }
  return NULL;
}

static idl_retcode_t
process_key(
  struct streams *streams,
  const idl_struct_t *_struct,
  const idl_key_t *key)
{
  const idl_type_spec_t *type_spec = _struct;
  const idl_declarator_t *decl = NULL;

  if (putf(&streams->props, "    keylist.add_key_endpoint(std::list<uint32_t>{"))
    return IDL_RETCODE_NO_MEMORY;

  for (size_t i = 0; i < key->field_name->length; i++) {
    if (!(decl = resolve_member(type_spec, key->field_name->names[i]->identifier))) {
      //this happens if the key field name points to something that does not exist
      //or something that cannot be resolved, should never occur in a correctly
      //parsed idl file
      assert(0);
      return IDL_RETCODE_SEMANTIC_ERROR;
    }

    const idl_member_t *mem = (const idl_member_t *)((const idl_node_t *)decl)->parent;
    type_spec = mem->type_spec;

    if (putf(&streams->props, "%1$"PRIu32, decl->id.value))
      return IDL_RETCODE_NO_MEMORY;
    if (i < key->field_name->length-1 &&
        putf(&streams->props, ", ", decl->id.value))
      return IDL_RETCODE_NO_MEMORY;
  }

  if (putf(&streams->props, "});\n"))
    return IDL_RETCODE_NO_MEMORY;

  return IDL_RETCODE_OK;
}

static idl_retcode_t
process_keylist(
  struct streams *streams,
  const idl_struct_t *_struct)
{
  const idl_key_t *key = NULL;

  IDL_FOREACH(key, _struct->keylist->keys) {
    if (process_key(streams, _struct, key))
      return IDL_RETCODE_NO_MEMORY;
  }

  return IDL_RETCODE_OK;
}

static idl_retcode_t
print_constructed_type_open(struct streams *streams, const idl_node_t *node)
{
  char* name = NULL;
  if (IDL_PRINTA(&name, get_cpp11_fully_scoped_name, node, streams->generator) < 0)
    return IDL_RETCODE_NO_MEMORY;

  static const char *fmt =
    "template<typename T, std::enable_if_t<std::is_base_of<cdr_stream, T>::value, bool> = true >\n"
    "bool {T}(T& streamer, {C}%1$s& instance, entity_properties_t &props) {\n"
    "  (void)instance;\n";
  static const char *pfmt1 =
    "template<>\n"
    "entity_properties_t get_type_props<%s>()%s";
  static const char *pfmt2 =
    " {\n"
    "  static std::mutex mtx;\n"
    "  static entity_properties_t props;\n"
    "  static bool initialized = false;\n\n"
    "  if (initialized)\n"
    "    return props;\n\n"
    "  std::lock_guard<std::mutex> lock(mtx);\n"
    "  props.clear();\n"
    "  key_endpoint keylist;\n\n";
  static const char *sfmt =
    "  if (!streamer.start_struct(props))\n"
    "    return false;\n";

  const char *estr = NULL;
  switch (get_extensibility(node)) {
    case IDL_APPENDABLE:
      estr = "ext_appendable";
      break;
    case IDL_MUTABLE:
      estr = "ext_mutable";
      break;
    default:
      break;
  }

  if (multi_putf(streams, ALL, fmt, name)
   || putf(&streams->props, pfmt1, name, pfmt2)
   || idl_fprintf(streams->generator->header.handle, pfmt1, name, ";\n\n") < 0
   || (estr && putf(&streams->props, "  props.e_ext = extensibility::%1$s;\n", estr))
   || multi_putf(streams, ALL, sfmt))
    return IDL_RETCODE_NO_MEMORY;

  return IDL_RETCODE_OK;
}

static idl_retcode_t
print_switchbox_open(struct streams *streams)
{
  static const char *fmt =
    "  bool firstcall = true;\n"
    "  while (auto &prop = streamer.next_entity(props, firstcall)) {\n"
    "%1$s"
    "    switch (prop.m_id) {\n";
  static const char *skipfmt =
    "    if (prop.ignore) {\n"
    "      streamer.skip_entity(prop);\n"
    "      continue;\n"
    "    }\n";

  if (multi_putf(streams, CONST, fmt, "")
   || multi_putf(streams, READ, fmt, skipfmt))
    return IDL_RETCODE_NO_MEMORY;

  return IDL_RETCODE_OK;
}

static idl_retcode_t
print_constructed_type_close(
  struct streams *streams)
{
  const char *fmt =
    "  return streamer.finish_struct(props);\n"
    "}\n\n";
  static const char *pfmt =
    "  props.m_members_by_seq.push_back(final_entry());\n"
    "  props.m_keys.push_back(final_entry());\n"
    "  props.finish(keylist);\n"
    "  initialized = true;\n"
    "  return props;\n"
    "}\n\n";

  if (multi_putf(streams, ALL, fmt)
   || putf(&streams->props, pfmt))
    return IDL_RETCODE_NO_MEMORY;

  return IDL_RETCODE_OK;
}

static idl_retcode_t
print_switchbox_close(struct streams *streams)
{
  static const char *fmt =
    "    }\n"
    "  }\n";
  static const char *rfmt =
    "      default:\n"
    "      if (prop.must_understand_remote\n"
    "       && streamer.status(must_understand_fail))\n"
    "        return false;\n"
    "      else\n"
    "        streamer.skip_entity(prop);\n"
    "      break;\n";

  if (putf(&streams->read, rfmt)
   || multi_putf(streams, ALL, fmt))
    return IDL_RETCODE_NO_MEMORY;

  return IDL_RETCODE_OK;
}

static idl_retcode_t
print_entry_point_functions(
  struct streams *streams,
  char *fullname)
{
  static const char *fmt =
    "template<typename S, std::enable_if_t<std::is_base_of<cdr_stream, S>::value, bool> = true >\n"
    "bool {T}(S& str, {C}%1$s& instance, bool as_key) {\n"
    "  auto props = get_type_props<%1$s>();\n"
    "  str.set_mode(cdr_stream::stream_mode::{T}, as_key);\n"
    "  return {T}(str, instance, props); \n"
    "}\n\n";

  if (multi_putf(streams, ALL, fmt, fullname))
    return IDL_RETCODE_NO_MEMORY;

return IDL_RETCODE_OK;
}

static idl_retcode_t
process_struct_contents(
  const idl_pstate_t* pstate,
  const bool revisit,
  const idl_path_t* path,
  const idl_struct_t *_struct,
  struct streams *streams)
{
  idl_retcode_t ret = IDL_RETCODE_OK;
  bool keylist = (pstate->config.flags & IDL_FLAG_KEYLIST) && _struct->keylist;

  size_t to_unroll = 1;
  const idl_struct_t *base = _struct;
  while (base->inherit_spec) {
    base =  (const idl_struct_t *)(base->inherit_spec->base);
    to_unroll++;
  }

  do {
    size_t depth_to_go = --to_unroll;
    base = _struct;
    while (depth_to_go--)
      base =  (const idl_struct_t *)(base->inherit_spec->base);

    if (keylist
     && (ret = process_keylist(streams, base)))
      return ret;

    const idl_member_t *member = NULL;
    IDL_FOREACH(member, base->members) {
      if ((ret = process_member(pstate, revisit, path, member, streams)))
        return ret;
    }

  } while (to_unroll);

  return ret;
}

static idl_retcode_t
process_struct(
  const idl_pstate_t* pstate,
  const bool revisit,
  const idl_path_t* path,
  const void* node,
  void* user_data)
{
  (void)path;
  struct streams *streams = user_data;

  char *fullname = NULL;
  if (IDL_PRINTA(&fullname, get_cpp11_fully_scoped_name, node, streams->generator) < 0)
    return IDL_RETCODE_NO_MEMORY;

  if (revisit) {
    if (print_switchbox_close(user_data)
     || print_constructed_type_close(user_data)
     || (!is_nested(node) && print_entry_point_functions(streams, fullname))) /*only add entry point functions for non-nested (topic) types*/
      return IDL_RETCODE_NO_MEMORY;

    return flush(streams->generator, streams);
  } else {

    idl_retcode_t ret = IDL_RETCODE_OK;
    if ((ret = print_constructed_type_open(user_data, node))
     || (ret = print_switchbox_open(user_data))
     || (ret = process_struct_contents(pstate, revisit, path, node, streams)))
      return ret;

    return IDL_VISIT_REVISIT;
  }
}

static idl_retcode_t
process_switch_type_spec(
  const idl_pstate_t *pstate,
  bool revisit,
  const idl_path_t *path,
  const void *node,
  void *user_data)
{
  static const char *fmt =
    "  {C}auto d = instance._d();\n"
    "  if (!{T}(streamer, d))\n"
    "    return false;\n";
  static const char *mfmt =
    "  size_t union_max = streamer.position();\n"
    "  size_t alignment_max = streamer.alignment();\n";

  struct streams *streams = user_data;

  (void)pstate;
  (void)revisit;
  (void)path;
  (void)node;

  if (multi_putf(streams, ALL, fmt)
   || multi_putf(streams, MAX, mfmt))
    return IDL_RETCODE_NO_MEMORY;

  return IDL_RETCODE_OK;
}

static idl_retcode_t
process_union(
  const idl_pstate_t* pstate,
  const bool revisit,
  const idl_path_t* path,
  const void* node,
  void* user_data)
{
  struct streams *streams = user_data;

  (void)pstate;
  (void)path;

  static const char *pfmt =
    "  streamer.position(union_max);\n"
    "  streamer.alignment(alignment_max);\n";
  static const char *mfmt =
    "  props.is_present = true;\n";

  char *fullname = NULL;
  if (IDL_PRINTA(&fullname, get_cpp11_fully_scoped_name, node, streams->generator) < 0)
    return IDL_RETCODE_NO_MEMORY;

  if (revisit) {
    if (multi_putf(streams, MAX, pfmt)
     || multi_putf(streams, MOVE | MAX, mfmt)
     || print_constructed_type_close(user_data)
     || (!is_nested(node) && print_entry_point_functions(streams, fullname))) /*only add entry point functions for non-nested (topic) types*/
      return IDL_RETCODE_NO_MEMORY;

    return flush(streams->generator, streams);
  } else {
    if (print_constructed_type_open(user_data, node))
      return IDL_RETCODE_NO_MEMORY;
    return IDL_VISIT_REVISIT;
  }
}

static idl_retcode_t
process_case_label(
  const idl_pstate_t* pstate,
  const bool revisit,
  const idl_path_t* path,
  const void* node,
  void* user_data)
{
  struct streams *streams = user_data;
  const idl_literal_t *literal = ((const idl_case_label_t *)node)->const_expr;
  char *value = "";
  const char *casefmt;

  (void)pstate;
  (void)revisit;
  (void)path;

  if (idl_mask(node) == IDL_DEFAULT_CASE_LABEL) {
    casefmt = "    default:\n";
  } else {
    casefmt = "    case %s:\n";
    if (IDL_PRINTA(&value, get_cpp11_value, literal, streams->generator) < 0)
      return IDL_RETCODE_NO_MEMORY;
  }

  if (multi_putf(streams, NOMAX, casefmt, value))
    return IDL_RETCODE_NO_MEMORY;

  return IDL_RETCODE_OK;
}

static idl_retcode_t
process_typedef_decl(
  const idl_pstate_t* pstate,
  struct streams* streams,
  const idl_type_spec_t* type_spec,
  const idl_declarator_t* declarator)
{
  instance_location_t loc = { .parent = "instance", .type = TYPEDEF };

  static const char* fmt =
    "template<typename T, std::enable_if_t<std::is_base_of<cdr_stream, T>::value, bool> = true >\n"
    "bool {T}_%1$s(T& streamer, {C}%2$s& instance) {\n"
    "  (void)instance;\n";
  char* name = NULL;
  if (IDL_PRINTA(&name, get_cpp11_name_typedef, declarator, streams->generator) < 0)
    return IDL_RETCODE_NO_MEMORY;

  char* fullname = NULL;
  if (IDL_PRINTA(&fullname, get_cpp11_fully_scoped_name, declarator, streams->generator) < 0)
    return IDL_RETCODE_NO_MEMORY;

  const idl_type_spec_t* ts = type_spec;
  while (idl_is_sequence(ts)) {
    ts = ((const idl_sequence_t*)ts)->type_spec;
  }

  if (multi_putf(streams, ALL, fmt, name, fullname))
    return IDL_RETCODE_NO_MEMORY;

  if (!idl_is_base_type(ts) && !idl_is_string(ts) && !idl_is_alias(ts)) {
    char* unrolled_name = NULL;
    if (IDL_PRINTA(&unrolled_name, get_cpp11_fully_scoped_name, ts, streams->generator) < 0 ||
        multi_putf(streams, ALL, "  auto prop = get_type_props<%1$s>();\n  prop.is_present = true;\n", unrolled_name))
      return IDL_RETCODE_NO_MEMORY;
  }

  if (process_entity(pstate, streams, declarator, type_spec, loc))
    return IDL_RETCODE_NO_MEMORY;

  if (idl_is_base_type(ts) || idl_is_string(ts) || idl_is_alias(ts)) {
    if (multi_putf(streams, ALL, "  return true;\n}\n\n"))
      return IDL_RETCODE_NO_MEMORY;
  } else {
    if (multi_putf(streams, ALL, "  return prop.is_present && !streamer.abort_status();\n}\n\n"))
      return IDL_RETCODE_NO_MEMORY;
  }

  return flush(streams->generator, streams);
}

static idl_retcode_t
process_typedef(
  const idl_pstate_t* pstate,
  const bool revisit,
  const idl_path_t* path,
  const void* node,
  void* user_data)
{
  (void)revisit;
  (void)path;

  struct streams* streams = user_data;
  idl_typedef_t* td = (idl_typedef_t*)node;
  const idl_declarator_t* declarator;

  IDL_FOREACH(declarator, td->declarators) {
    if (process_typedef_decl(pstate, streams, td->type_spec, declarator))
     return IDL_RETCODE_NO_MEMORY;
  }

  return IDL_RETCODE_OK;
}

static idl_retcode_t
process_enum(
  const idl_pstate_t* pstate,
  const bool revisit,
  const idl_path_t* path,
  const void* node,
  void* user_data)
{
  struct streams *str = (struct streams*)user_data;
  struct generator *gen = str->generator;
  const idl_enum_t *_enum = (const idl_enum_t *)node;
  const idl_enumerator_t *enumerator;
  uint32_t value;
  const char *enum_name = NULL;
  idl_retcode_t ret = IDL_RETCODE_OK;
  uint32_t *already_encountered = NULL,
           n = 0;

  (void)pstate;
  (void)revisit;
  (void)path;

  char *fullname = NULL;
  if (IDL_PRINTA(&fullname, get_cpp11_fully_scoped_name, _enum, gen) < 0)
    return IDL_RETCODE_NO_MEMORY;

  static const char *fmt = "template<>\n"\
                    "%s enum_conversion<%s>(uint32_t in)%s";

  if (putf(&str->props, fmt, fullname, fullname, " {\n  switch (in) {\n")
   || idl_fprintf(gen->header.handle, fmt, fullname, fullname, ";\n\n") < 0)
    return IDL_RETCODE_NO_MEMORY;

  /*count the number of enumerators*/
  IDL_FOREACH(enumerator, _enum->enumerators) {
    n++;
  }

  if (n && !(already_encountered = malloc(sizeof(uint32_t)*n)))
    return IDL_RETCODE_NO_MEMORY;

  n = 0;

  /*go over all enumerators*/
  IDL_FOREACH(enumerator, _enum->enumerators) {
    enum_name = get_cpp11_name(enumerator);
    value = enumerator->value.value;
    bool already_present = false;
    /*check for duplicates*/
    for (uint32_t i = 0; i < n && !already_present; i++) {
      if (value == already_encountered[i])
        already_present = true;
    }
    if (already_present)
      continue;

    already_encountered[n++] = value;

    if (putf(&str->props, "    %scase %"PRIu32":\n"
                          "    return %s::%s;\n"
                          "    break;\n",
                          enumerator == _enum->default_enumerator ? "default:\n    " : "",
                          value,
                          fullname,
                          enum_name) < 0) {
      ret = IDL_RETCODE_NO_MEMORY;
      break;
    }
  }

  /*cleanup*/
  if (already_encountered)
    free(already_encountered);
  if (ret)
    return ret;

  if (putf(&str->props,"  }\n}\n\n"))
    return IDL_RETCODE_NO_MEMORY;

  return IDL_RETCODE_OK;
}

idl_retcode_t
generate_streamers(const idl_pstate_t* pstate, struct generator *gen)
{
  struct streams streams;
  idl_visitor_t visitor;
  const char *sources[] = { NULL, NULL };

  setup_streams(&streams, gen);

  memset(&visitor, 0, sizeof(visitor));

  assert(pstate->sources);
  sources[0] = pstate->sources->path->name;
  visitor.sources = sources;

  const char *fmt = "namespace org{\n"
                    "namespace eclipse{\n"
                    "namespace cyclonedds{\n"
                    "namespace core{\n"
                    "namespace cdr{\n\n";
  if (idl_fprintf(gen->header.handle, "%s", fmt) < 0
   || idl_fprintf(gen->impl.handle, "%s", fmt) < 0)
    return IDL_RETCODE_NO_MEMORY;

  visitor.visit = IDL_STRUCT | IDL_UNION | IDL_CASE | IDL_CASE_LABEL | IDL_SWITCH_TYPE_SPEC | IDL_TYPEDEF | IDL_ENUM;
  visitor.accept[IDL_ACCEPT_STRUCT] = &process_struct;
  visitor.accept[IDL_ACCEPT_UNION] = &process_union;
  visitor.accept[IDL_ACCEPT_CASE] = &process_case;
  visitor.accept[IDL_ACCEPT_CASE_LABEL] = &process_case_label;
  visitor.accept[IDL_ACCEPT_SWITCH_TYPE_SPEC] = &process_switch_type_spec;
  visitor.accept[IDL_ACCEPT_TYPEDEF] = &process_typedef;
  visitor.accept[IDL_ACCEPT_ENUM] = &process_enum;

  if (idl_visit(pstate, pstate->root, &visitor, &streams)
   || flush(gen, &streams))
    return IDL_RETCODE_NO_MEMORY;

  fmt = "} //namespace cdr\n"
        "} //namespace core\n"
        "} //namespace cyclonedds\n"
        "} //namespace eclipse\n"
        "} //namespace org\n\n";
  if (idl_fprintf(gen->header.handle, "%s", fmt) < 0
   || idl_fprintf(gen->impl.handle, "%s", fmt) < 0)
    return IDL_RETCODE_NO_MEMORY;

  cleanup_streams(&streams);

  return IDL_RETCODE_OK;
}
