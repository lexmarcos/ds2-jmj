//! Capturing what each game instance has on screen.
//!
//! Both instances run as ordinary X windows, so each can be grabbed on its own
//! rather than photographing the whole desktop. `xwd` does the grabbing; the
//! decoding is here because nothing on a plain Debian box converts XWD to
//! anything useful, and shelling out to ImageMagick would be another dependency
//! to install.

use std::path::{Path, PathBuf};
use std::process::Command;

use serde::Serialize;

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct GameWindow {
    /// X window id, as `xwd -id` wants it.
    pub id: String,
    pub title: String,
    pub width: u32,
    pub height: u32,
}

/// Every Dark Souls II game window currently mapped.
///
/// The injector's console and Proton's helper windows carry the same class, so
/// they are filtered out by title: only the game itself is called DARK SOULS II.
pub fn windows() -> Result<Vec<GameWindow>, String> {
    let output = Command::new("xwininfo")
        .args(["-root", "-children"])
        .output()
        .map_err(|e| format!("não consegui rodar xwininfo: {e}"))?;

    let text = String::from_utf8_lossy(&output.stdout);
    let mut found = Vec::new();

    for line in text.lines() {
        let line = line.trim();
        if !line.starts_with("0x") || !line.contains("\"DARK SOULS II\"") {
            continue;
        }

        let Some(id) = line.split_whitespace().next() else {
            continue;
        };
        // Geometry looks like 1034x612+0+0; take the first such token.
        let geometry = line
            .split_whitespace()
            .find(|token| token.contains('x') && token.contains('+'));
        let (width, height) = geometry
            .and_then(|g| {
                let size = g.split('+').next()?;
                let (w, h) = size.split_once('x')?;
                Some((w.parse().ok()?, h.parse().ok()?))
            })
            .unwrap_or((0, 0));

        found.push(GameWindow {
            id: id.to_owned(),
            title: "DARK SOULS II".to_owned(),
            width,
            height,
        });
    }

    Ok(found)
}

/// Grabs one window and writes it as a PNG.
pub fn capture(window: &GameWindow, out: &Path) -> Result<PathBuf, String> {
    let dump = Command::new("xwd")
        .args(["-silent", "-id", &window.id])
        .output()
        .map_err(|e| format!("não consegui rodar xwd: {e}"))?;

    if !dump.status.success() || dump.stdout.is_empty() {
        return Err(format!(
            "xwd não capturou a janela {}: {}",
            window.id,
            String::from_utf8_lossy(&dump.stderr).trim()
        ));
    }

    let image = decode_xwd(&dump.stdout)?;
    write_png(&image, out)?;
    Ok(out.to_path_buf())
}

struct Image {
    width: u32,
    height: u32,
    /// Tightly packed RGBA.
    pixels: Vec<u8>,
}

/// Decodes an X11 window dump.
///
/// The header is a run of big-endian u32s; the fields used here are the header
/// size, the dimensions, the stride and the colormap length. Pixels come out as
/// BGRA with padding at the end of each row.
fn decode_xwd(bytes: &[u8]) -> Result<Image, String> {
    if bytes.len() < 100 {
        return Err("dump XWD truncado".into());
    }

    let field = |index: usize| -> u32 {
        let at = index * 4;
        u32::from_be_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]])
    };

    let header_size = field(0) as usize;
    let width = field(4);
    let height = field(5);
    let bits_per_pixel = field(11);
    let bytes_per_line = field(12) as usize;
    let colours = field(19) as usize;

    if width == 0 || height == 0 {
        return Err("a janela não tem tamanho; ela está minimizada?".into());
    }
    if bits_per_pixel != 24 && bits_per_pixel != 32 {
        return Err(format!("profundidade de cor não suportada: {bits_per_pixel}"));
    }

    let start = header_size + colours * 12;
    let needed = start + bytes_per_line * height as usize;
    if bytes.len() < needed {
        return Err("dump XWD menor do que o cabeçalho promete".into());
    }

    let stride_per_pixel = bytes_per_line / width as usize;
    let mut pixels = Vec::with_capacity((width * height * 4) as usize);

    for row in 0..height as usize {
        let line = start + row * bytes_per_line;
        for column in 0..width as usize {
            let at = line + column * stride_per_pixel;
            // BGRA in the dump, RGBA in the PNG.
            pixels.push(bytes[at + 2]);
            pixels.push(bytes[at + 1]);
            pixels.push(bytes[at]);
            pixels.push(255);
        }
    }

    Ok(Image { width, height, pixels })
}

fn write_png(image: &Image, out: &Path) -> Result<(), String> {
    if let Some(parent) = out.parent() {
        std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    }
    let file = std::fs::File::create(out)
        .map_err(|e| format!("não consegui criar {}: {e}", out.display()))?;

    let mut encoder = png::Encoder::new(std::io::BufWriter::new(file), image.width, image.height);
    encoder.set_color(png::ColorType::Rgba);
    encoder.set_depth(png::BitDepth::Eight);

    encoder
        .write_header()
        .map_err(|e| e.to_string())?
        .write_image_data(&image.pixels)
        .map_err(|e| format!("não consegui escrever o PNG: {e}"))
}
