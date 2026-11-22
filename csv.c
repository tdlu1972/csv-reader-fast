// csv.c, built to csv.wasm by build.sh: reads /input/cases.json's bytes and writes the whole answers.json, csv-reader
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
#else
// A native build, for testing on a machine without a wasm linker: SSE2 in place of wasm's SIMD, malloc for memory.
#include <emmintrin.h>
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
static Buf O, E, X;  // the answers, this case's errors, this record's cut fields
static Buf *F;       // where the field being read goes

static u32 pos, line, record_line, record_row;
static int record_row_null;

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

static void error_head(const char *code, u32 len) {
  if (E.len) lit(&E, ",", 1);
  lit(&E, "{\"code\":\"", 9);
  lit(&E, code, len);
  lit(&E, "\",\"line\":", 9);
  num(&E, record_line);
  lit(&E, ",\"row\":", 7);
  if (record_row_null) lit(&E, "null", 4);
  else num(&E, record_row);
}

static void error(const char *code, u32 len) {
  error_head(code, len);
  lit(&E, "}", 1);
}

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

// One case: its text's JSON spelling starts at p (just past the opening quote).
__attribute__((noinline)) static void answer(u32 p, int header) {
  pos = p;
  line = 1;
  E.len = 0;
  if (s[pos] == 0xEF && s[pos + 1] == 0xBB && s[pos + 2] == 0xBF) pos += 3;
  else if (s[pos] == '\\' && s[pos + 1] == 'u' && hex4(pos + 2) == 0xfeff) pos += 6;

  int columns_set = 0;
  int width = -1;
  u32 rows = 0;
  if (header) lit(&O, "{\"columns\":", 11);
  else lit(&O, "{\"columns\":null,\"rows\":[", 24);

  for (;;) {
    for (;;) {
      if (s[pos] == '\\' && s[pos + 1] == 'n') {
        pos += 2;
        line++;
      } else if (s[pos] == '\\' && s[pos + 1] == 'r' && s[pos + 2] == '\\' && s[pos + 3] == 'n') {
        pos += 4;
        line++;
      } else {
        break;
      }
    }
    if (s[pos] == '"') break;

    int is_header = header && !columns_set;
    record_line = line;
    record_row_null = is_header;
    record_row = rows;
    u32 mark = O.len;
    if (!is_header && rows > 0) lit(&O, ",", 1);
    lit(&O, "[", 1);
    X.len = 0;

    u32 k = 0;
    int quoted, empty;
    do {
      if (width >= 0 && k >= (u32)width) {
        F = &X;
        if (k > (u32)width) lit(F, ",", 1);
      } else {
        F = &O;
        if (k > 0) lit(F, ",", 1);
      }
      lit(F, "\"", 1);
      u32 start = F->len;
      quoted = s[pos] == '\\' && s[pos + 1] == '"';
      if (quoted) read_quoted();
      else read_unquoted();
      empty = F->len == start;
      lit(F, "\"", 1);
      k++;
    } while (ended == COMMA);

    if (k == 1 && empty && !quoted) {
      O.len = mark;
      continue;
    }
    if (width < 0) width = (int)k;
    if (is_header) {
      columns_set = 1;
      lit(&O, "],\"rows\":[", 10);
      continue;
    }
    if (k < (u32)width) {
      error_head("short_row", 9);
      lit(&E, ",\"fields\":", 10);
      num(&E, k);
      lit(&E, "}", 1);
      for (; k < (u32)width; k++) lit(&O, ",\"\"", 3);
    } else if (k > (u32)width) {
      error_head("long_row", 8);
      lit(&E, ",\"extra\":[", 10);
      append(&E, &X);
      lit(&E, "]}", 2);
    }
    lit(&O, "]", 1);
    rows++;
  }
  if (header && !columns_set) lit(&O, "[],\"rows\":[", 11);
  lit(&O, "],\"errors\":[", 12);
  append(&O, &E);
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

  O.cap = n + n / 2 + 4096;
  O.p = bump(O.cap + 32);
  O.len = 0;
  E.cap = 1 << 20;
  E.p = bump(E.cap + 32);
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
      u32 text = 0;
      int header = 0;
      OBJECT(i, {
        if (key_is(ka, kb, "input", 5)) {
          OBJECT(i, {
            if (key_is(ka, kb, "text", 4)) {
              expect(i, '"');
              if (text) __builtin_trap();
              text = i + 1;
              i = skip_string(i);
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
      if (!text) __builtin_trap();
      if (!first) lit(&O, ",", 1);
      answer(text, header);
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
