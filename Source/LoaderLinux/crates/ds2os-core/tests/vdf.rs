use ds2os_core::vdf;

#[test]
fn reads_nested_blocks() {
    let parsed = vdf::parse(
        r#"
        "libraryfolders"
        {
            "0"
            {
                "path"     "/home/user/.steam/debian-installation"
                "apps"
                {
                    "335300"   "1234"
                }
            }
        }
        "#,
    )
    .expect("valid document");

    assert_eq!(
        parsed.path(["libraryfolders", "0", "path"]).and_then(vdf::Value::as_str),
        Some("/home/user/.steam/debian-installation")
    );
    assert_eq!(
        parsed.path(["libraryfolders", "0", "apps", "335300"]).and_then(vdf::Value::as_str),
        Some("1234")
    );
}

#[test]
fn skips_line_comments() {
    let parsed = vdf::parse("// leading\n\"a\" \"1\" // trailing\n\"b\" \"2\"").expect("valid");
    assert_eq!(parsed.get("a").and_then(vdf::Value::as_str), Some("1"));
    assert_eq!(parsed.get("b").and_then(vdf::Value::as_str), Some("2"));
}

#[test]
fn keeps_repeated_keys_in_order() {
    let parsed = vdf::parse(r#""root" { "x" "1" "x" "2" }"#).expect("valid");
    let root = parsed.get("root").expect("root");
    assert_eq!(root.entries().len(), 2);
    assert_eq!(root.get("x").and_then(vdf::Value::as_str), Some("1"));
}

#[test]
fn handles_escapes_and_unicode() {
    let parsed = vdf::parse(r#""k" "C:\\Program Files\\Jogo — ção""#).expect("valid");
    assert_eq!(
        parsed.get("k").and_then(vdf::Value::as_str),
        Some(r"C:\Program Files\Jogo — ção")
    );
}

#[test]
fn rejects_unterminated_block() {
    assert!(vdf::parse(r#""root" { "x" "1""#).is_err());
}

#[test]
fn rejects_stray_close_brace() {
    assert!(vdf::parse("}").is_err());
}
