//! Capturing what each game instance has on screen.
//!
//! Both instances run as ordinary X windows, so each is grabbed on its own
//! rather than photographing the whole desktop.
//!
//! Grabbing goes through XComposite. Reading a window's area straight off the
//! screen returns whatever is drawn on top of it, so a game behind a terminal
//! comes back as a picture of the terminal. Asking the server to redirect the
//! window into an offscreen pixmap first gives its real contents whether or not
//! anything covers it.

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
    /// The process that owns it, when the window says so.
    ///
    /// Two instances look identical from the outside, and their windows appear
    /// in whatever order the X server hands them over. The owning pid is the
    /// only thing that ties a window to the instance the harness started, so
    /// that "focus instance 2" means the account, not a position in a list.
    pub pid: Option<u32>,
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
            pid: window_pid(id),
            id: id.to_owned(),
            title: "DARK SOULS II".to_owned(),
            width,
            height,
        });
    }

    // xwininfo lists in stacking order, which changes the moment a window is
    // focused. Sorting by id gives each instance a stable index, so "window 1"
    // means the same window across calls.
    found.sort_by(|a, b| a.id.cmp(&b.id));
    Ok(found)
}

/// The pid in the window's _NET_WM_PID, when it has one.
fn window_pid(id: &str) -> Option<u32> {
    use x11rb::protocol::xproto::{AtomEnum, ConnectionExt as _};

    let id = u32::from_str_radix(id.trim_start_matches("0x"), 16).ok()?;
    let (conn, _) = x11rb::connect(None).ok()?;
    let atom = conn
        .intern_atom(false, b"_NET_WM_PID")
        .ok()?
        .reply()
        .ok()?
        .atom;
    let property = conn
        .get_property(false, id, atom, AtomEnum::CARDINAL, 0, 1)
        .ok()?
        .reply()
        .ok()?;
    let first = property.value32()?.next();
    first
}

/// Brings a window to the front and gives it keyboard focus.
///
/// The game ignores gamepad input while it is not the active window, so
/// anything that drives it has to focus the right instance first. This asks the
/// window manager through _NET_ACTIVE_WINDOW rather than forcing the stacking
/// order, so the manager stays in charge and the change sticks.
///
/// It then **checks** that the focus landed, and says so when it did not. A
/// request the manager quietly drops used to report success, and every input
/// sent afterwards went to whatever really held the focus: the game sat at the
/// same screen while the harness reported a menu walk it never performed.
pub fn focus(window: &GameWindow) -> Result<(), String> {
    use x11rb::connection::Connection;
    use x11rb::protocol::xproto::{
        self, ClientMessageEvent, ConnectionExt as _, EventMask, InputFocus, StackMode,
    };

    let id = u32::from_str_radix(window.id.trim_start_matches("0x"), 16)
        .map_err(|_| format!("id de janela inválido: {}", window.id))?;

    let (conn, screen_index) = x11rb::connect(None).map_err(|e| e.to_string())?;
    let root = conn.setup().roots[screen_index].root;

    let atom = conn
        .intern_atom(false, b"_NET_ACTIVE_WINDOW")
        .map_err(|e| e.to_string())?
        .reply()
        .map_err(|e| e.to_string())?
        .atom;

    // source 2 means "a pager", which window managers honour without the
    // focus-stealing prevention they apply to applications.
    let message = ClientMessageEvent::new(32, id, atom, [2, x11rb::CURRENT_TIME, 0, 0, 0]);
    conn.send_event(
        false,
        root,
        EventMask::SUBSTRUCTURE_NOTIFY | EventMask::SUBSTRUCTURE_REDIRECT,
        message,
    )
    .map_err(|e| e.to_string())?;

    let _ = conn.configure_window(
        id,
        &xproto::ConfigureWindowAux::new().stack_mode(StackMode::ABOVE),
    );
    conn.flush().map_err(|e| e.to_string())?;

    // The manager needs a moment before the window actually takes input, and
    // it may decide not to. Give it a few tries, then take the focus directly:
    // that bypasses the manager's focus-stealing prevention, which is exactly
    // what a harness wants and an application should not do.
    for attempt in 0..10 {
        std::thread::sleep(std::time::Duration::from_millis(150));
        match active_window(&conn, root) {
            Some(active) if active == id => return Ok(()),
            _ => {}
        }
        if attempt >= 2 {
            let _ = conn.set_input_focus(InputFocus::PARENT, id, x11rb::CURRENT_TIME);
            let _ = conn.flush();
        }
    }

    let holder = active_window(&conn, root)
        .map(|w| format!("0x{w:x}"))
        .unwrap_or_else(|| "nenhuma".to_owned());
    Err(format!(
        "a janela {} não ficou em foco; {holder} está com ele. \
         O jogo ignora o controle enquanto não está em foco",
        window.id
    ))
}

/// Which window the manager currently reports as active.
fn active_window<C: x11rb::connection::Connection>(conn: &C, root: u32) -> Option<u32> {
    use x11rb::protocol::xproto::{AtomEnum, ConnectionExt as _};

    let atom = conn
        .intern_atom(false, b"_NET_ACTIVE_WINDOW")
        .ok()?
        .reply()
        .ok()?
        .atom;
    let property = conn
        .get_property(false, root, atom, AtomEnum::WINDOW, 0, 1)
        .ok()?
        .reply()
        .ok()?;
    let first = property.value32()?.next();
    first
}

/// Grabs one window and writes it as a PNG.
pub fn capture(window: &GameWindow, out: &Path) -> Result<PathBuf, String> {
    let id = u32::from_str_radix(window.id.trim_start_matches("0x"), 16)
        .map_err(|_| format!("id de janela inválido: {}", window.id))?;

    let image = grab(id)?;
    write_png(&image, out)?;
    Ok(out.to_path_buf())
}

fn grab(window: u32) -> Result<Image, String> {
    use x11rb::connection::Connection;
    use x11rb::protocol::composite::{self, Redirect};
    use x11rb::protocol::xproto::{self, ConnectionExt as _, ImageFormat};

    let (conn, _) = x11rb::connect(None).map_err(|e| format!("sem conexão com o X: {e}"))?;

    let geometry = conn
        .get_geometry(window)
        .map_err(|e| e.to_string())?
        .reply()
        .map_err(|_| "a janela sumiu antes da captura".to_string())?;

    if geometry.width == 0 || geometry.height == 0 {
        return Err("a janela não tem tamanho; ela está minimizada?".into());
    }

    // Redirecting is idempotent from our side: if a compositor already did it,
    // the request fails and the existing redirection still serves us.
    let _ = composite::redirect_window(&conn, window, Redirect::AUTOMATIC)
        .map(|cookie| cookie.check());

    // Prefer the offscreen pixmap; fall back to the window itself, which is
    // still correct as long as nothing covers it.
    let pixmap = conn.generate_id().map_err(|e| e.to_string())?;
    let drawable = match composite::name_window_pixmap(&conn, window, pixmap)
        .map_err(|e| e.to_string())?
        .check()
    {
        Ok(()) => pixmap,
        Err(_) => window,
    };

    let reply = xproto::get_image(
        &conn,
        ImageFormat::Z_PIXMAP,
        drawable,
        0,
        0,
        geometry.width,
        geometry.height,
        !0,
    )
    .map_err(|e| e.to_string())?
    .reply()
    .map_err(|e| format!("não consegui ler a janela: {e}"))?;

    if drawable == pixmap {
        let _ = conn.free_pixmap(pixmap);
    }

    // The server pads each pixel out to whole bytes; find out how many.
    let bits_per_pixel = conn
        .setup()
        .pixmap_formats
        .iter()
        .find(|format| format.depth == reply.depth)
        .map(|format| format.bits_per_pixel)
        .unwrap_or(32) as usize;
    let bytes_per_pixel = bits_per_pixel / 8;
    if bytes_per_pixel < 3 {
        return Err(format!("profundidade de cor não suportada: {}", reply.depth));
    }

    let width = geometry.width as usize;
    let height = geometry.height as usize;
    let stride = width * bytes_per_pixel;
    let mut pixels = Vec::with_capacity(width * height * 4);

    for row in 0..height {
        for column in 0..width {
            let at = row * stride + column * bytes_per_pixel;
            match reply.data.get(at + 2) {
                // BGRX on the wire, RGBA in the PNG.
                Some(red) => {
                    pixels.push(*red);
                    pixels.push(reply.data[at + 1]);
                    pixels.push(reply.data[at]);
                    pixels.push(255);
                }
                None => return Err("a imagem veio menor do que a janela".into()),
            }
        }
    }

    Ok(Image { width: geometry.width as u32, height: geometry.height as u32, pixels })
}

struct Image {
    width: u32,
    height: u32,
    /// Tightly packed RGBA.
    pixels: Vec<u8>,
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
