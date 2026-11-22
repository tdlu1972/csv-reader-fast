// csv.c, built to csv.wasm by build.sh: reads /input/cases.json's bytes and writes answers.json, csv-reader
// 1.0.0's answer for every case, without ever decoding a string.
//
// A case's text is read straight from its JSON spelling. The only characters csv-reader cares about are the comma, the
// quote, LF, CR and a leading U+FEFF, and JSON.stringify spells them `,` `\"` `\n` `\r` and the raw BOM; every other byte
// (another escape included) is copied to the answer as it stands, since an answer string's JSON spelling may be any
// spelling of the same code units. One 16-byte SIMD compare per block finds the next byte that matters, and each block is
// stored to the output before it is looked at, so a run of plain bytes costs one load and one store per 16.
//
// Anything this reader does not expect (a \u escape spelling one of those characters, a case shape other than
// {id, input: {text, header}}, a text that is not a string) traps, and main.ts answers the file with solve.js instead.

typedef unsigned char u8;
typedef unsigned int u32;

#ifdef __wasm__
#include <wasm_simd128.h>

extern u8 __heap_base;
static u32 heap_top;

static u8 *bump(u32 size) {
  if (!heap_top) heap_top = ((u32)&__heap_base + 15) & ~15u;
  u32 p = heap_top;
  u32 need = p + size + 64;
  u32 have = (u32)__builtin_wasm_memory_size(0) * 65536u;
  if (need > have && __builtin_wasm_memory_grow(0, (need - have + 65535) / 65536) == (__SIZE_TYPE__)-1) __builtin_trap();
  heap_top = (p + size + 15) & ~15u;
  return (u8 *)p;
}

// Hands n bytes of the answers file, the next ones, to main.ts, which writes them out.
__attribute__((import_module("env"), import_name("flush"))) void flush(const u8 *p, u32 n);
#else
// A native build, for testing on a machine without a wasm linker: SSE2 in place of wasm's SIMD, malloc for memory.
#include <emmintrin.h>
#include <stdio.h>
#include <stdlib.h>
typedef __m128i v128_t;
#define wasm_v128_load(p) _mm_loadu_si128((const __m128i *)(p))
#define wasm_v128_store(p, v) _mm_storeu_si128((__m128i *)(p), (v))
#define wasm_i8x16_splat(c) _mm_set1_epi8((char)(c))
#define wasm_i8x16_eq _mm_cmpeq_epi8
#define wasm_v128_or _mm_or_si128
#define wasm_i8x16_bitmask(v) ((u32)_mm_movemask_epi8(v))
#define export_name(x) used

static u8 *bump(u32 size) {
  u8 *p = malloc(size + 64);
  if (!p) __builtin_trap();
  return p;
}

static void flush(const u8 *p, u32 n) { fwrite(p, 1, n, stdout); }
#endif

typedef struct {
  u8 *p;
  u32 len, cap;
} Buf;

// Every buffer keeps 32 bytes of slack past cap, so a 16-byte store at len never lands outside it.
static void grow(Buf *b, u32 extra) {
  u32 cap = b->cap * 2;
  if (cap < b->len + extra + 64) cap = b->len + extra + 64;
  u8 *p = bump(cap + 32);
  __builtin_memcpy(p, b->p, b->len);
  b->p = p;
  b->cap = cap;
}

#define ENSURE(b, k) \
  if ((b)->len + (k) > (b)->cap) grow((b), (k))

static void lit(Buf *b, const char *str, u32 n) {
  ENSURE(b, n);
  for (u32 k = 0; k < n; k++) b->p[b->len + k] = (u8)str[k];
  b->len += n;
}

static void num(Buf *b, u32 v) {
  u8 tmp[10];
  int k = 0;
  do {
    tmp[k++] = (u8)('0' + v % 10);
    v /= 10;
  } while (v);
  ENSURE(b, 10);
  while (k) b->p[b->len++] = tmp[--k];
}

static void append(Buf *b, const Buf *from) {
  ENSURE(b, from->len);
  __builtin_memcpy(b->p + b->len, from->p, from->len);
  b->len += from->len;
}

static const u8 *s;  // the input, a raw '"' at its end and zeros after
static u32 n;
static Buf O, X, XS;  // the answers, this record's cut fields, this case's long rows' cut fields
static Buf *F;        // where the field being read goes

static u32 pos, line, record_line, record_rec;

enum { COMMA, LINE_BREAK, END };
static int ended;
static u32 added;

static int hexval(u8 c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  __builtin_trap();
}

static u32 hex4(u32 i) { return (u32)(hexval(s[i]) << 12 | hexval(s[i + 1]) << 8 | hexval(s[i + 2]) << 4 | hexval(s[i + 3])); }

// A \u escape at i: one spelling a character csv-reader reads is not handled here.
static void check_u(u32 i) {
  u32 v = hex4(i + 2);
  if (v == 0x0a || v == 0x0d || v == 0x22 || v == 0x2c) __builtin_trap();
}

// A case's errors are kept as records until its header flag is known, since the flag decides each one's row and the
// case's text is read before the flag (which may come after it).
typedef struct {
  const char *code;
  u32 code_len, line, rec;  // rec: the record's index among the case's records, empty ones not counted
  u32 kind, a, b;           // kind 1: short_row with a fields; 2: long_row, its extras XS[a, a + b)
} Err;
static Err *errs;
static u32 nerr, errcap;

static Err *push_error(const char *code, u32 len) {
  if (nerr == errcap) {
    Err *e = (Err *)bump(errcap * 2 * (u32)sizeof(Err));
    __builtin_memcpy(e, errs, nerr * sizeof(Err));
    errs = e;
    errcap *= 2;
  }
  Err *e = &errs[nerr++];
  e->code = code;
  e->code_len = len;
  e->line = record_line;
  e->rec = record_rec;
  e->kind = 0;
  return e;
}

static void error(const char *code, u32 len) { push_error(code, len); }

static inline u32 mask3(v128_t v) {
  return wasm_i8x16_bitmask(wasm_v128_or(wasm_v128_or(wasm_i8x16_eq(v, wasm_i8x16_splat(',')), wasm_i8x16_eq(v, wasm_i8x16_splat('\\'))),
                                         wasm_i8x16_eq(v, wasm_i8x16_splat('"'))));
}

static inline u32 mask2(v128_t v) {
  return wasm_i8x16_bitmask(wasm_v128_or(wasm_i8x16_eq(v, wasm_i8x16_splat('\\')), wasm_i8x16_eq(v, wasm_i8x16_splat('"'))));
}

static inline void copy2(u32 i) {
  F->p[F->len] = s[i];
  F->p[F->len + 1] = s[i + 1];
  F->len += 2;
}

// Unquoted characters from pos up to the next comma or line break, after whatever F already holds; a CR not followed by
// LF is dropped (stray_cr). Sets ended, and added: how many bytes it wrote.
__attribute__((noinline)) static void read_unquoted(void) {
  u32 i = pos;
  u32 first = F->len;
  for (;;) {
    ENSURE(F, 32);
    v128_t v = wasm_v128_load(s + i);
    wasm_v128_store(F->p + F->len, v);
    u32 m = mask3(v);
    if (!m) {
      F->len += 16;
      i += 16;
      continue;
    }
    u32 t = (u32)__builtin_ctz(m);
    F->len += t;
    i += t;
    u8 c = s[i];
    if (c == ',') {
      pos = i + 1;
      ended = COMMA;
      break;
    }
    if (c == '"') {
      pos = i;
      ended = END;
      break;
    }
    u8 d = s[i + 1];
    if (d == 'n') {
      pos = i + 2;
      line++;
      ended = LINE_BREAK;
      break;
    }
    if (d == 'r') {
      if (s[i + 2] == '\\' && s[i + 3] == 'n') {
        pos = i + 4;
        line++;
        ended = LINE_BREAK;
        break;
      }
      error("stray_cr", 8);
      i += 2;
      continue;
    }
    if (d == 'u') {
      check_u(i);
      for (u32 k = 0; k < 6; k++) F->p[F->len + k] = s[i + k];
      F->len += 6;
      i += 6;
      continue;
    }
    copy2(i);
    i += 2;
  }
  added = F->len - first;
}

// A quoted field whose opening quote (\") is at pos.
__attribute__((noinline)) static void read_quoted(void) {
  u32 open = pos, open_line = line, mark = F->len;
  u32 i = pos + 2;
  int closed = 0;
  for (;;) {
    ENSURE(F, 32);
    v128_t v = wasm_v128_load(s + i);
    wasm_v128_store(F->p + F->len, v);
    u32 m = mask2(v);
    if (!m) {
      F->len += 16;
      i += 16;
      continue;
    }
    u32 t = (u32)__builtin_ctz(m);
    F->len += t;
    i += t;
    if (s[i] == '"') break;
    u8 d = s[i + 1];
    if (d == '"') {
      if (s[i + 2] == '\\' && s[i + 3] == '"') {
        copy2(i);
        i += 4;
        continue;
      }
      i += 2;
      closed = 1;
      break;
    }
    if (d == 'r') {
      F->p[F->len] = '\\';
      F->p[F->len + 1] = 'n';
      F->len += 2;
      line++;
      i += 2;
      if (s[i] == '\\' && s[i + 1] == 'n') i += 2;
      continue;
    }
    if (d == 'n') line++;
    if (d == 'u') {
      check_u(i);
      for (u32 k = 0; k < 6; k++) F->p[F->len + k] = s[i + k];
      F->len += 6;
      i += 6;
      continue;
    }
    copy2(i);
    i += 2;
  }
  if (!closed) {
    pos = open + 2;
    line = open_line;
    F->len = mark;
    error("unterminated_quote", 18);
    lit(F, "\\\"", 2);
    read_unquoted();
    return;
  }
  pos = i;
  u8 c = s[i];
  if (c == '"') {
    ended = END;
    return;
  }
  if (c == ',') {
    pos = i + 1;
    ended = COMMA;
    return;
  }
  if (c == '\\') {
    if (s[i + 1] == 'n') {
      pos = i + 2;
      line++;
      ended = LINE_BREAK;
      return;
    }
    if (s[i + 1] == 'r' && s[i + 2] == '\\' && s[i + 3] == 'n') {
      pos = i + 4;
      line++;
      ended = LINE_BREAK;
      return;
    }
  }
  read_unquoted();
  if (added > 0) error("quote_junk", 10);
}

// Room for k more bytes at o, o and olim being answer()'s cursor into O and O's end.
#define OROOM(k)              \
  if (o + (k) > olim) {       \
    O.len = (u32)(o - O.p);   \
    grow(&O, (k));            \
    o = O.p + O.len;          \
    olim = O.p + O.cap;       \
  }

static u32 case_start, first_end, case_recs;

// One case: its text's JSON spelling starts at p0 (just past the opening quote). Written as if its header flag were
// false; finish() then fixes the few bytes that differ when it is true (see there). Leaves pos at the text's closing
// quote.
//
// Fields are read here with the cursors in locals; read_quoted() and read_unquoted() are the slow path: a quoted field
// left unterminated is read again with read_quoted() from its start, and a long row's fields past the width go to X
// through them.
__attribute__((noinline)) static void answer(u32 p0) {
  const u8 *p = s + p0;
  if (p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) p += 3;
  else if (p[0] == '\\' && p[1] == 'u' && hex4((u32)(p - s) + 2) == 0xfeff) p += 6;
  nerr = 0;
  XS.len = 0;

  u8 *o = O.p + O.len, *olim = O.p + O.cap;
  OROOM(64);
  case_start = (u32)(o - O.p);
  __builtin_memcpy(o, "{\"columns\":null,\"rows\":[", 24);
  o += 24;

  u32 ln = 1, recs = 0, width = 0;  // width 0: not set yet
  for (;;) {
    for (;;) {
      if (p[0] == '\\' && p[1] == 'n') {
        p += 2;
        ln++;
      } else if (p[0] == '\\' && p[1] == 'r' && p[2] == '\\' && p[3] == 'n') {
        p += 4;
        ln++;
      } else {
        break;
      }
    }
    if (*p == '"') break;

    record_line = ln;
    record_rec = recs;
    OROOM(64);
    u32 mark = (u32)(o - O.p);
    if (recs) *o++ = ',';
    *o++ = '[';

    u32 k = 0;
    int quoted, empty = 0, en;
    for (;;) {
      OROOM(64);
      *o++ = '"';
      quoted = p[0] == '\\' && p[1] == '"';
      int junk = 0;
      if (quoted) {
        u32 fo = (u32)(o - O.p), fl = ln;
        const u8 *fp = p;
        int ok = 0;
        p += 2;
        for (;;) {
          OROOM(64);
          v128_t v = wasm_v128_load(p);
          wasm_v128_store(o, v);
          u32 m = mask2(v);
          if (!m) {
            p += 16;
            o += 16;
            continue;
          }
          u32 t = (u32)__builtin_ctz(m);
          p += t;
          o += t;
          if (*p == '"') break;
          u8 d = p[1];
          if (d == '"') {
            if (p[2] == '\\' && p[3] == '"') {
              o[0] = '\\';
              o[1] = '"';
              o += 2;
              p += 4;
              continue;
            }
            p += 2;
            ok = 1;
            break;
          }
          if (d == 'r') {
            o[0] = '\\';
            o[1] = 'n';
            o += 2;
            ln++;
            p += 2;
            if (p[0] == '\\' && p[1] == 'n') p += 2;
            continue;
          }
          if (d == 'n') {
            ln++;
          } else if (d == 'u') {
            check_u((u32)(p - s));
            for (u32 j = 0; j < 6; j++) o[j] = p[j];
            o += 6;
            p += 6;
            continue;
          }
          o[0] = p[0];
          o[1] = p[1];
          o += 2;
          p += 2;
        }
        if (ok) {
          if (*p == ',') {
            p++;
            en = COMMA;
            goto field_end;
          }
          if (*p == '"') {
            en = END;
            goto field_end;
          }
          if (p[0] == '\\' && p[1] == 'n') {
            p += 2;
            ln++;
            en = LINE_BREAK;
            goto field_end;
          }
          if (p[0] == '\\' && p[1] == 'r' && p[2] == '\\' && p[3] == 'n') {
            p += 4;
            ln++;
            en = LINE_BREAK;
            goto field_end;
          }
          junk = 1;  // what follows the closing quote is read as unquoted, onto the field
        } else {
          // A quote that never closes: read_quoted() reads the field again from its start.
          O.len = fo;
          pos = (u32)(fp - s);
          line = fl;
          F = &O;
          read_quoted();
          o = O.p + O.len;
          olim = O.p + O.cap;
          p = s + pos;
          ln = line;
          en = ended;
          goto field_end;
        }
      }
      u32 start = (u32)(o - O.p);
      for (;;) {
        OROOM(64);
        v128_t v = wasm_v128_load(p);
        wasm_v128_store(o, v);
        u32 m = mask3(v);
        if (!m) {
          p += 16;
          o += 16;
          continue;
        }
        u32 t = (u32)__builtin_ctz(m);
        p += t;
        o += t;
        u8 c = *p;
        if (c == ',') {
          p++;
          en = COMMA;
          break;
        }
        if (c == '"') {
          en = END;
          break;
        }
        u8 d = p[1];
        if (d == 'n') {
          p += 2;
          ln++;
          en = LINE_BREAK;
          break;
        }
        if (d == 'r') {
          if (p[2] == '\\' && p[3] == 'n') {
            p += 4;
            ln++;
            en = LINE_BREAK;
            break;
          }
          error("stray_cr", 8);
          p += 2;
          continue;
        }
        if (d == 'u') {
          check_u((u32)(p - s));
          for (u32 j = 0; j < 6; j++) o[j] = p[j];
          o += 6;
          p += 6;
          continue;
        }
        o[0] = p[0];
        o[1] = p[1];
        o += 2;
        p += 2;
      }
      if (junk) {
        if ((u32)(o - O.p) != start) error("quote_junk", 10);
      } else {
        empty = (u32)(o - O.p) == start;
      }
    field_end:
      *o++ = '"';
      k++;
      if (en != COMMA) break;
      if (width && k >= width) {
        // A long row: the rest of its fields are cut to X, and from there to the error's extras.
        O.len = (u32)(o - O.p);
        pos = (u32)(p - s);
        line = ln;
        X.len = 0;
        do {
          F = &X;
          if (k > width) lit(F, ",", 1);
          lit(F, "\"", 1);
          if (s[pos] == '\\' && s[pos + 1] == '"') read_quoted();
          else read_unquoted();
          lit(F, "\"", 1);
          k++;
        } while (ended == COMMA);
        Err *e = push_error("long_row", 8);
        e->kind = 2;
        e->a = XS.len;
        e->b = X.len;
        append(&XS, &X);
        o = O.p + O.len;
        olim = O.p + O.cap;
        p = s + pos;
        ln = line;
        break;
      }
      *o++ = ',';
    }

    if (k == 1 && empty && !quoted) {
      o = O.p + mark;
      continue;
    }
    if (!width) {
      width = k;
    } else if (k < width) {
      Err *e = push_error("short_row", 9);
      e->kind = 1;
      e->a = k;
      OROOM(3 * (width - k) + 64);
      for (; k < width; k++) {
        o[0] = ',';
        o[1] = '"';
        o[2] = '"';
        o += 3;
      }
    }
    OROOM(64);
    *o++ = ']';
    if (!recs) {
      // Room for finish() to turn the separator after the first record into `,"rows":[`.
      first_end = (u32)(o - O.p);
      for (u32 j = 0; j < 9; j++) o[j] = ' ';
      o += 9;
    }
    recs++;
  }
  OROOM(64);
  *o++ = ']';
  O.len = (u32)(o - O.p);
  pos = (u32)(p - s);
  case_recs = recs;
}

// Ends the case answer() wrote, now that its header flag is known. With a header the first record is the columns:
// `{"columns":null,"rows":[` becomes `{"columns":` and spaces, the 9 spaces after the first record become `,"rows":[`,
// and the comma before the second record a space (with no record at all, the columns are []).
static void finish(int header) {
  if (header) {
    u8 *c = O.p + case_start;
    if (!case_recs) {
      __builtin_memcpy(c, "{\"columns\":[],\"rows\":[  ", 24);
    } else {
      __builtin_memcpy(c, "{\"columns\":             ", 24);
      __builtin_memcpy(O.p + first_end, ",\"rows\":[", 9);
      if (case_recs > 1) O.p[first_end + 9] = ' ';
    }
  }
  lit(&O, ",\"errors\":[", 11);
  for (u32 j = 0; j < nerr; j++) {
    Err *e = &errs[j];
    if (j) lit(&O, ",", 1);
    lit(&O, "{\"code\":\"", 9);
    lit(&O, e->code, e->code_len);
    lit(&O, "\",\"line\":", 9);
    num(&O, e->line);
    lit(&O, ",\"row\":", 7);
    if (header && !e->rec) lit(&O, "null", 4);
    else num(&O, e->rec - (u32)header);
    if (e->kind == 1) {
      lit(&O, ",\"fields\":", 10);
      num(&O, e->a);
    } else if (e->kind == 2) {
      lit(&O, ",\"extra\":[", 10);
      ENSURE(&O, e->b);
      __builtin_memcpy(O.p + O.len, XS.p + e->a, e->b);
      O.len += e->b;
      lit(&O, "]", 1);
    }
    lit(&O, "}", 1);
  }
  lit(&O, "]}", 2);
}

// The case list around the texts.

static u32 ws(u32 i) {
  while (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t') i++;
  return i;
}

static void expect(u32 i, u8 c) {
  if (i >= n || s[i] != c) __builtin_trap();
}

// Past the string whose opening quote is at i.
static u32 skip_string(u32 i) {
  i++;
  for (;;) {
    u32 m = mask2(wasm_v128_load(s + i));
    if (!m) {
      i += 16;
      continue;
    }
    i += (u32)__builtin_ctz(m);
    if (i >= n) __builtin_trap();
    if (s[i] == '"') return i + 1;
    i += 2;
  }
}

static u32 skip_value(u32 i) {
  i = ws(i);
  if (i >= n) __builtin_trap();
  u8 c = s[i];
  if (c == '"') return skip_string(i);
  if (c == '{' || c == '[') {
    int depth = 0;
    for (;;) {
      if (i >= n) __builtin_trap();
      c = s[i];
      if (c == '"') {
        i = skip_string(i);
        continue;
      }
      if (c == '{' || c == '[') depth++;
      else if (c == '}' || c == ']') {
        if (--depth == 0) return i + 1;
      }
      i++;
    }
  }
  while (i < n && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != ' ' && s[i] != '\n' && s[i] != '\r' && s[i] != '\t') i++;
  return i;
}

static int key_is(u32 a, u32 b, const char *k, u32 len) {
  if (b - a != len) return 0;
  for (u32 j = 0; j < len; j++)
    if (s[a + j] != (u8)k[j]) return 0;
  return 1;
}

// Calls f on each key of the object at i (its value starts at the returned position); returns past the object.
#define OBJECT(i, BODY)                    \
  do {                                     \
    i = ws(i);                             \
    expect(i, '{');                        \
    i = ws(i + 1);                         \
    if (s[i] == '}') {                     \
      i++;                                 \
      break;                               \
    }                                      \
    for (;;) {                             \
      expect(i, '"');                      \
      u32 ka = i + 1;                      \
      i = skip_string(i);                  \
      u32 kb = i - 1;                      \
      i = ws(i);                           \
      expect(i, ':');                      \
      i = ws(i + 1);                       \
      BODY;                                \
      i = ws(i);                           \
      if (s[i] == ',') {                   \
        i = ws(i + 1);                     \
        continue;                          \
      }                                    \
      expect(i, '}');                      \
      i++;                                 \
      break;                               \
    }                                      \
  } while (0)

__attribute__((export_name("input"))) u8 *input(u32 size) {
  n = size;
  u8 *p = bump(size + 64);
  s = p;
  return p;
}

__attribute__((export_name("run"))) int run(void) {
  // The bytes past the file: a raw quote, which ends any scan, then zeros.
  u8 *t = (u8 *)s;
  for (u32 k = 0; k < 64; k++) t[n + k] = 0;
  t[n] = '"';

  // Each case's answer is handed to flush() once it is whole, so O holds one case at a time.
  O.cap = 1 << 22;
  O.p = bump(O.cap + 32);
  O.len = 0;
  errcap = 4096;
  errs = (Err *)bump(errcap * (u32)sizeof(Err));
  XS.cap = 1 << 16;
  XS.p = bump(XS.cap + 32);
  X.cap = 1 << 16;
  X.p = bump(X.cap + 32);

  u32 i = ws(0);
  expect(i, '[');
  i = ws(i + 1);
  lit(&O, "[", 1);
  if (s[i] == ']') {
    i++;
  } else {
    for (int first = 1;; first = 0) {
      if (!first) lit(&O, ",", 1);
      int got = 0, header = 0;
      OBJECT(i, {
        if (key_is(ka, kb, "input", 5)) {
          OBJECT(i, {
            if (key_is(ka, kb, "text", 4)) {
              expect(i, '"');
              if (got) __builtin_trap();
              got = 1;
              answer(i + 1);
              i = pos + 1;
            } else if (key_is(ka, kb, "header", 6)) {
              header = i + 4 <= n && s[i] == 't' && s[i + 1] == 'r' && s[i + 2] == 'u' && s[i + 3] == 'e';
              i = skip_value(i);
            } else {
              i = skip_value(i);
            }
          });
        } else {
          i = skip_value(i);
        }
      });
      if (!got) __builtin_trap();
      finish(header);
      flush(O.p, O.len);
      O.len = 0;
      i = ws(i);
      if (s[i] == ',') {
        i = ws(i + 1);
        continue;
      }
      expect(i, ']');
      i++;
      break;
    }
  }
  if (ws(i) != n) __builtin_trap();
  lit(&O, "]", 1);
  return 0;
}

__attribute__((export_name("out_ptr"))) u8 *out_ptr(void) { return O.p; }
__attribute__((export_name("out_len"))) u32 out_len(void) { return O.len; }
