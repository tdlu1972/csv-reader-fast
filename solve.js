// A faster drop-in for csv-reader 1.0.0: run(input) returns the same {columns, rows, errors} for every {text, header}.
//
// Same single left-to-right pass and the same broken-row policy as the reference, but each field is cut out of the text
// with one slice() instead of being built a character at a time, the loop works on char codes, and no closure or
// per-field result object is made. Slow paths (a stray CR, a doubled quote, a CR inside quotes) fall back to joining
// slices, so their answers stay exactly the reference's.

const COMMA = 0;
const LINE_BREAK = 1;
const END = 2;

export function run(input) {
  const text = String(input.text);
  const header = input.header === true;
  const n = text.length;
  const errors = [];
  const rows = [];
  let columns = null;
  let width = -1;

  let pos = 0;
  let line = 1;
  if (n > 0 && text.charCodeAt(0) === 0xfeff) pos = 1;

  let recordLine = 1;
  let recordRow = null;

  // The last field read: its value, how it ended, and (for an unquoted read) how many characters it kept.
  let value = '';
  let ended = END;
  let added = 0;

  // Reads unquoted characters after `prefix` up to the next comma or line break; a CR not followed by LF is dropped
  // (stray_cr). Sets value, ended and added.
  function readUnquoted(prefix) {
    let start = pos;
    let i = pos;
    let acc = prefix;
    let kept = 0;
    while (i < n) {
      const c = text.charCodeAt(i);
      if (c === 44) {
        kept += i - start;
        value = acc + text.slice(start, i);
        pos = i + 1;
        ended = COMMA;
        added = kept;
        return;
      }
      if (c === 10) {
        kept += i - start;
        value = acc + text.slice(start, i);
        pos = i + 1;
        line += 1;
        ended = LINE_BREAK;
        added = kept;
        return;
      }
      if (c === 13) {
        if (i + 1 < n && text.charCodeAt(i + 1) === 10) {
          kept += i - start;
          value = acc + text.slice(start, i);
          pos = i + 2;
          line += 1;
          ended = LINE_BREAK;
          added = kept;
          return;
        }
        errors.push({ code: 'stray_cr', line: recordLine, row: recordRow });
        kept += i - start;
        acc += text.slice(start, i);
        i += 1;
        start = i;
        continue;
      }
      i += 1;
    }
    kept += i - start;
    value = acc + text.slice(start, i);
    pos = i;
    ended = END;
    added = kept;
  }

  // Reads a quoted field whose opening quote is at pos. Sets value and ended.
  function readQuoted() {
    const open = pos;
    const openLine = line;
    let i = pos + 1;
    let start = i;
    let acc = '';
    let closed = false;
    while (i < n) {
      const c = text.charCodeAt(i);
      if (c === 34) {
        if (i + 1 < n && text.charCodeAt(i + 1) === 34) {
          acc += text.slice(start, i + 1);
          i += 2;
          start = i;
          continue;
        }
        acc += text.slice(start, i);
        i += 1;
        closed = true;
        break;
      }
      if (c === 13) {
        acc += text.slice(start, i) + '\n';
        line += 1;
        i += i + 1 < n && text.charCodeAt(i + 1) === 10 ? 2 : 1;
        start = i;
        continue;
      }
      if (c === 10) line += 1;
      i += 1;
    }
    if (!closed) {
      pos = open + 1;
      line = openLine;
      errors.push({ code: 'unterminated_quote', line: recordLine, row: recordRow });
      readUnquoted('"');
      return;
    }
    pos = i;
    if (i >= n) {
      value = acc;
      ended = END;
      return;
    }
    const c = text.charCodeAt(i);
    if (c === 44) {
      value = acc;
      pos = i + 1;
      ended = COMMA;
      return;
    }
    if (c === 10) {
      value = acc;
      pos = i + 1;
      line += 1;
      ended = LINE_BREAK;
      return;
    }
    if (c === 13 && i + 1 < n && text.charCodeAt(i + 1) === 10) {
      value = acc;
      pos = i + 2;
      line += 1;
      ended = LINE_BREAK;
      return;
    }
    readUnquoted(acc);
    if (added > 0) errors.push({ code: 'quote_junk', line: recordLine, row: recordRow });
  }

  while (true) {
    while (pos < n) {
      const c = text.charCodeAt(pos);
      if (c === 10) {
        pos += 1;
        line += 1;
      } else if (c === 13 && pos + 1 < n && text.charCodeAt(pos + 1) === 10) {
        pos += 2;
        line += 1;
      } else {
        break;
      }
    }
    if (pos >= n) break;

    recordLine = line;
    recordRow = header && columns === null ? null : rows.length;
    const fields = [];
    let quoted;
    do {
      quoted = pos < n && text.charCodeAt(pos) === 34;
      if (quoted) readQuoted();
      else readUnquoted('');
      fields.push(value);
    } while (ended === COMMA);

    if (fields.length === 1 && fields[0] === '' && !quoted) continue;

    if (width < 0) width = fields.length;
    if (header && columns === null) {
      columns = fields;
      continue;
    }
    const len = fields.length;
    if (len < width) {
      errors.push({ code: 'short_row', line: recordLine, row: recordRow, fields: len });
      while (fields.length < width) fields.push('');
    } else if (len > width) {
      errors.push({ code: 'long_row', line: recordLine, row: recordRow, extra: fields.splice(width) });
    }
    rows.push(fields);
  }
  return { columns: header ? (columns ?? []) : null, rows, errors };
}
