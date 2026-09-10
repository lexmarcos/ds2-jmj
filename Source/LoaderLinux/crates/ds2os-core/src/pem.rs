//! Normalising an RSA public key into the exact bytes the server produced.
//!
//! The server compares the key it receives against the one it generated, so a
//! key that differs by a line ending or a missing final newline is rejected and
//! the login is dropped without explanation. Anything that reaches the injector
//! config has to go through here first.

/// Rebuilds a PEM public key in canonical form: LF line endings, base64 wrapped
/// at 64 columns, and a trailing newline.
///
/// This also repairs a key whose newlines were flattened into spaces, which is
/// what a single-line text input does to pasted content.
pub fn normalize_public_key(input: &str) -> Option<String> {
    let text = input.replace("\r\n", "\n").replace('\r', "\n");

    let begin = text.find("-----BEGIN")?;
    let begin_end = text[begin..].find("-----\n").map(|i| begin + i + 6).or_else(|| {
        // The header may be followed by a space rather than a newline.
        text[begin + 5..].find("-----").map(|i| begin + 5 + i + 5)
    })?;

    let end = text.find("-----END")?;
    if end < begin_end {
        return None;
    }

    let header = text[begin..begin_end].trim().to_owned();
    let footer = text[end..].trim().to_owned();
    if !footer.ends_with("-----") {
        return None;
    }

    // Whatever separated the base64 - newlines, spaces, both - is discarded and
    // rebuilt, so a flattened key comes back correct.
    let body: String = text[begin_end..end].chars().filter(|c| !c.is_whitespace()).collect();
    if body.is_empty() || !body.chars().all(|c| c.is_ascii_alphanumeric() || c == '+' || c == '/' || c == '=') {
        return None;
    }

    let mut out = String::with_capacity(body.len() + header.len() + footer.len() + 16);
    out.push_str(&header);
    out.push('\n');
    for chunk in body.as_bytes().chunks(64) {
        out.push_str(std::str::from_utf8(chunk).expect("base64 is ascii"));
        out.push('\n');
    }
    out.push_str(&footer);
    out.push('\n');
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    const CANONICAL: &str = "-----BEGIN RSA PUBLIC KEY-----\n\
        MIIBCgKCAQEArFigb9/XmfSwKi2BVeulb6UAaRk0XbA+cg9Ywv4ds2bc463rnwrj\n\
        nl7oBSzVCVedpQEqDhnEf6HnN/wHMFiib3NX7ltBjy2ZmwySWKc42JE87BarW59s\n\
        -----END RSA PUBLIC KEY-----\n";

    #[test]
    fn leaves_a_canonical_key_alone() {
        assert_eq!(normalize_public_key(CANONICAL).as_deref(), Some(CANONICAL));
    }

    #[test]
    fn adds_the_trailing_newline() {
        assert_eq!(
            normalize_public_key(CANONICAL.trim_end()).as_deref(),
            Some(CANONICAL)
        );
    }

    #[test]
    fn converts_crlf() {
        let crlf = CANONICAL.replace('\n', "\r\n");
        assert_eq!(normalize_public_key(&crlf).as_deref(), Some(CANONICAL));
    }

    #[test]
    fn repairs_a_key_flattened_into_one_line() {
        // Exactly what a single-line input does to a pasted PEM key.
        let flattened = CANONICAL.trim_end().replace('\n', " ");
        assert_eq!(normalize_public_key(&flattened).as_deref(), Some(CANONICAL));
    }

    #[test]
    fn rejects_text_that_is_not_a_key() {
        assert!(normalize_public_key("").is_none());
        assert!(normalize_public_key("hello").is_none());
        assert!(normalize_public_key("-----BEGIN RSA PUBLIC KEY-----\n\n").is_none());
    }
}
