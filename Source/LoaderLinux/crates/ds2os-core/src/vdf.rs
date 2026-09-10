//! Minimal reader for Valve's KeyValues text format.
//!
//! Steam stores `libraryfolders.vdf` and `appmanifest_*.acf` in this format. We
//! only ever read it, and only need quoted keys, quoted values, nested blocks
//! and `//` comments, so this deliberately does not implement conditionals,
//! includes or unquoted tokens.

use std::fmt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Value {
    Str(String),
    /// Pairs rather than a map: Valve files may repeat a key, and order matters
    /// when a caller wants the first match.
    Obj(Vec<(String, Value)>),
}

impl Value {
    /// Looks up a direct child by key.
    pub fn get(&self, key: &str) -> Option<&Value> {
        match self {
            Value::Obj(pairs) => pairs
                .iter()
                .find(|(k, _)| k.eq_ignore_ascii_case(key))
                .map(|(_, v)| v),
            Value::Str(_) => None,
        }
    }

    /// Walks a chain of keys, e.g. `path(["libraryfolders", "0", "path"])`.
    pub fn path<'a, I>(&self, keys: I) -> Option<&Value>
    where
        I: IntoIterator<Item = &'a str>,
    {
        keys.into_iter().try_fold(self, |value, key| value.get(key))
    }

    pub fn as_str(&self) -> Option<&str> {
        match self {
            Value::Str(text) => Some(text),
            Value::Obj(_) => None,
        }
    }

    pub fn entries(&self) -> &[(String, Value)] {
        match self {
            Value::Obj(pairs) => pairs,
            Value::Str(_) => &[],
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ParseError {
    pub message: String,
    pub offset: usize,
}

impl fmt::Display for ParseError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{} at byte {}", self.message, self.offset)
    }
}

impl std::error::Error for ParseError {}

/// Parses a whole document. The result is an object holding every top level pair.
pub fn parse(input: &str) -> Result<Value, ParseError> {
    let mut parser = Parser { bytes: input.as_bytes(), pos: 0 };
    let pairs = parser.parse_pairs(true)?;
    Ok(Value::Obj(pairs))
}

struct Parser<'a> {
    bytes: &'a [u8],
    pos: usize,
}

impl<'a> Parser<'a> {
    fn parse_pairs(&mut self, top_level: bool) -> Result<Vec<(String, Value)>, ParseError> {
        let mut pairs = Vec::new();

        loop {
            self.skip_trivia();
            match self.peek() {
                None => {
                    if top_level {
                        return Ok(pairs);
                    }
                    return Err(self.error("unterminated block"));
                }
                Some(b'}') => {
                    if top_level {
                        return Err(self.error("unexpected '}'"));
                    }
                    self.pos += 1;
                    return Ok(pairs);
                }
                Some(b'"') => {}
                Some(other) => {
                    return Err(self.error(&format!("expected a quoted key, found {:?}", other as char)))
                }
            }

            let key = self.parse_quoted()?;
            self.skip_trivia();

            let value = match self.peek() {
                Some(b'"') => Value::Str(self.parse_quoted()?),
                Some(b'{') => {
                    self.pos += 1;
                    Value::Obj(self.parse_pairs(false)?)
                }
                _ => return Err(self.error("expected a value or '{' after a key")),
            };

            pairs.push((key, value));
        }
    }

    fn parse_quoted(&mut self) -> Result<String, ParseError> {
        debug_assert_eq!(self.peek(), Some(b'"'));
        self.pos += 1;

        let mut out = String::new();
        loop {
            match self.peek() {
                None => return Err(self.error("unterminated string")),
                Some(b'"') => {
                    self.pos += 1;
                    return Ok(out);
                }
                Some(b'\\') => {
                    self.pos += 1;
                    let escaped = self.peek().ok_or_else(|| self.error("unterminated escape"))?;
                    self.pos += 1;
                    out.push(match escaped {
                        b'n' => '\n',
                        b't' => '\t',
                        b'\\' => '\\',
                        b'"' => '"',
                        other => other as char,
                    });
                }
                Some(_) => {
                    // Copy one whole UTF-8 character so multi-byte paths survive.
                    let rest = &self.bytes[self.pos..];
                    let text = std::str::from_utf8(rest).map_err(|_| self.error("invalid UTF-8"))?;
                    let ch = text.chars().next().expect("rest is non-empty");
                    self.pos += ch.len_utf8();
                    out.push(ch);
                }
            }
        }
    }

    fn skip_trivia(&mut self) {
        loop {
            match self.peek() {
                Some(byte) if byte.is_ascii_whitespace() => self.pos += 1,
                Some(b'/') if self.bytes.get(self.pos + 1) == Some(&b'/') => {
                    while !matches!(self.peek(), None | Some(b'\n')) {
                        self.pos += 1;
                    }
                }
                _ => return,
            }
        }
    }

    fn peek(&self) -> Option<u8> {
        self.bytes.get(self.pos).copied()
    }

    fn error(&self, message: &str) -> ParseError {
        ParseError { message: message.to_owned(), offset: self.pos }
    }
}
