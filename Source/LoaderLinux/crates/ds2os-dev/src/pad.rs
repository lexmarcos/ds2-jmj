//! A virtual gamepad, so the harness can drive the game.
//!
//! This is a kernel level device rather than synthetic X11 events: the game
//! reads gamepads through evdev, and ignores anything faked at the window
//! system. It presents itself as an Xbox 360 pad, using that controller's real
//! vendor and product ids, because SDL ships a mapping for those and the button
//! layout then comes out right without a mapping string of our own.
//!
//! The device only exists while its process does, so it lives in a small daemon
//! and the CLI talks to it over a Unix socket. Creating and destroying a device
//! per keypress would look to the game like a controller being plugged in and
//! pulled out several times a second.

use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::PathBuf;
use std::time::Duration;

use evdev::uinput::VirtualDevice;
use evdev::{
    AbsInfo, AbsoluteAxisCode, AttributeSet, EventType, InputEvent, InputId, KeyCode,
    UinputAbsSetup,
};

use crate::paths;

const VENDOR_MICROSOFT: u16 = 0x045e;
const PRODUCT_XBOX360: u16 = 0x028e;
const STICK_RANGE: i32 = 32767;
const TRIGGER_RANGE: i32 = 255;

pub fn socket_path(index: u8) -> PathBuf {
    paths::state_dir().join(format!("pad-{index}.sock"))
}

/// Buttons, named the way someone holding the controller would name them.
fn key_for(name: &str) -> Option<KeyCode> {
    Some(match name {
        "a" => KeyCode::BTN_SOUTH,
        "b" => KeyCode::BTN_EAST,
        "x" => KeyCode::BTN_NORTH,
        "y" => KeyCode::BTN_WEST,
        "lb" => KeyCode::BTN_TL,
        "rb" => KeyCode::BTN_TR,
        "back" | "select" => KeyCode::BTN_SELECT,
        "start" => KeyCode::BTN_START,
        "guide" | "home" => KeyCode::BTN_MODE,
        "l3" => KeyCode::BTN_THUMBL,
        "r3" => KeyCode::BTN_THUMBR,
        _ => return None,
    })
}

fn all_keys() -> AttributeSet<KeyCode> {
    let mut keys = AttributeSet::<KeyCode>::new();
    for key in [
        KeyCode::BTN_SOUTH,
        KeyCode::BTN_EAST,
        KeyCode::BTN_NORTH,
        KeyCode::BTN_WEST,
        KeyCode::BTN_TL,
        KeyCode::BTN_TR,
        KeyCode::BTN_SELECT,
        KeyCode::BTN_START,
        KeyCode::BTN_MODE,
        KeyCode::BTN_THUMBL,
        KeyCode::BTN_THUMBR,
    ] {
        keys.insert(key);
    }
    keys
}

fn build_device(index: u8) -> Result<VirtualDevice, String> {
    let stick = AbsInfo::new(0, -STICK_RANGE, STICK_RANGE, 16, 128, 0);
    let trigger = AbsInfo::new(0, 0, TRIGGER_RANGE, 0, 0, 0);
    let hat = AbsInfo::new(0, -1, 1, 0, 0, 0);

    let name = format!("ds2os virtual pad {index}");
    let mut builder = VirtualDevice::builder()
        .map_err(|e| format!("não consegui abrir /dev/uinput: {e}"))?
        .name(&name)
        .input_id(InputId::new(evdev::BusType::BUS_USB, VENDOR_MICROSOFT, PRODUCT_XBOX360, 0x0114))
        .with_keys(&all_keys())
        .map_err(|e| e.to_string())?;

    for (axis, info) in [
        (AbsoluteAxisCode::ABS_X, stick),
        (AbsoluteAxisCode::ABS_Y, stick),
        (AbsoluteAxisCode::ABS_RX, stick),
        (AbsoluteAxisCode::ABS_RY, stick),
        (AbsoluteAxisCode::ABS_Z, trigger),
        (AbsoluteAxisCode::ABS_RZ, trigger),
        (AbsoluteAxisCode::ABS_HAT0X, hat),
        (AbsoluteAxisCode::ABS_HAT0Y, hat),
    ] {
        builder = builder
            .with_absolute_axis(&UinputAbsSetup::new(axis, info))
            .map_err(|e| e.to_string())?;
    }

    builder.build().map_err(|e| format!("não consegui criar o dispositivo: {e}"))
}

struct Pad {
    device: VirtualDevice,
}

impl Pad {
    fn emit(&mut self, events: &[InputEvent]) -> Result<(), String> {
        self.device.emit(events).map_err(|e| e.to_string())
    }

    fn button(&mut self, name: &str, hold: Duration) -> Result<(), String> {
        let key = key_for(name).ok_or_else(|| format!("botão desconhecido: {name}"))?;
        self.emit(&[InputEvent::new(EventType::KEY.0, key.0, 1)])?;
        std::thread::sleep(hold);
        self.emit(&[InputEvent::new(EventType::KEY.0, key.0, 0)])
    }

    fn dpad(&mut self, name: &str, hold: Duration) -> Result<(), String> {
        let (axis, value) = match name {
            "up" => (AbsoluteAxisCode::ABS_HAT0Y, -1),
            "down" => (AbsoluteAxisCode::ABS_HAT0Y, 1),
            "left" => (AbsoluteAxisCode::ABS_HAT0X, -1),
            "right" => (AbsoluteAxisCode::ABS_HAT0X, 1),
            _ => return Err(format!("direção desconhecida: {name}")),
        };
        self.emit(&[InputEvent::new(EventType::ABSOLUTE.0, axis.0, value)])?;
        std::thread::sleep(hold);
        self.emit(&[InputEvent::new(EventType::ABSOLUTE.0, axis.0, 0)])
    }

    /// Holds a stick at a position, then centres it. Values are -1.0 to 1.0,
    /// with negative y meaning up, the way the hardware reports it.
    fn stick(&mut self, side: &str, x: f32, y: f32, hold: Duration) -> Result<(), String> {
        let (ax, ay) = match side {
            "l" | "left" => (AbsoluteAxisCode::ABS_X, AbsoluteAxisCode::ABS_Y),
            "r" | "right" => (AbsoluteAxisCode::ABS_RX, AbsoluteAxisCode::ABS_RY),
            _ => return Err(format!("analógico desconhecido: {side}")),
        };
        let scale = |value: f32| (value.clamp(-1.0, 1.0) * STICK_RANGE as f32) as i32;

        self.emit(&[
            InputEvent::new(EventType::ABSOLUTE.0, ax.0, scale(x)),
            InputEvent::new(EventType::ABSOLUTE.0, ay.0, scale(y)),
        ])?;
        std::thread::sleep(hold);
        self.emit(&[
            InputEvent::new(EventType::ABSOLUTE.0, ax.0, 0),
            InputEvent::new(EventType::ABSOLUTE.0, ay.0, 0),
        ])
    }

    fn trigger(&mut self, side: &str, hold: Duration) -> Result<(), String> {
        let axis = match side {
            "lt" => AbsoluteAxisCode::ABS_Z,
            "rt" => AbsoluteAxisCode::ABS_RZ,
            _ => return Err(format!("gatilho desconhecido: {side}")),
        };
        self.emit(&[InputEvent::new(EventType::ABSOLUTE.0, axis.0, TRIGGER_RANGE)])?;
        std::thread::sleep(hold);
        self.emit(&[InputEvent::new(EventType::ABSOLUTE.0, axis.0, 0)])
    }
}

/// Runs the daemon: creates the device and serves commands until told to stop.
pub fn serve(index: u8) -> Result<(), String> {
    let socket = socket_path(index);
    let _ = std::fs::remove_file(&socket);
    std::fs::create_dir_all(paths::state_dir()).map_err(|e| e.to_string())?;

    let mut pad = Pad { device: build_device(index)? };

    // The game only notices a new controller some milliseconds after it
    // appears, so settle before accepting commands.
    std::thread::sleep(Duration::from_millis(400));

    let listener = UnixListener::bind(&socket)
        .map_err(|e| format!("não consegui abrir {}: {e}", socket.display()))?;
    println!("pad {index} pronto em {}", socket.display());

    for stream in listener.incoming() {
        let Ok(stream) = stream else { continue };
        match handle(&mut pad, stream) {
            Ok(true) => break,
            Ok(false) => {}
            Err(error) => eprintln!("comando falhou: {error}"),
        }
    }

    let _ = std::fs::remove_file(&socket);
    Ok(())
}

/// Handles one connection. Returns true when asked to shut down.
fn handle(pad: &mut Pad, stream: UnixStream) -> Result<bool, String> {
    let mut writer = stream.try_clone().map_err(|e| e.to_string())?;
    let reader = BufReader::new(stream);

    for line in reader.lines() {
        let line = line.map_err(|e| e.to_string())?;
        let parts: Vec<&str> = line.split_whitespace().collect();
        let ms = |at: usize| -> Duration {
            Duration::from_millis(parts.get(at).and_then(|v| v.parse().ok()).unwrap_or(90))
        };

        let result = match parts.as_slice() {
            ["quit", ..] => {
                let _ = writeln!(writer, "ok");
                return Ok(true);
            }
            ["ping", ..] => Ok(()),
            ["press", button, ..] => pad.button(button, ms(2)),
            ["dpad", direction, ..] => pad.dpad(direction, ms(2)),
            ["trigger", side, ..] => pad.trigger(side, ms(2)),
            ["stick", side, x, y, ..] => {
                let x: f32 = x.parse().map_err(|_| format!("x inválido: {x}"))?;
                let y: f32 = y.parse().map_err(|_| format!("y inválido: {y}"))?;
                pad.stick(side, x, y, ms(4))
            }
            [] => Ok(()),
            _ => Err(format!("comando desconhecido: {line}")),
        };

        match result {
            Ok(()) => {
                let _ = writeln!(writer, "ok");
            }
            Err(error) => {
                let _ = writeln!(writer, "erro {error}");
            }
        }
    }
    Ok(false)
}

/// Sends one command to a running daemon.
pub fn send(index: u8, command: &str) -> Result<String, String> {
    let socket = socket_path(index);
    let stream = UnixStream::connect(&socket).map_err(|_| {
        format!("o pad {index} não está rodando; inicie com `ds2os-dev pad start`")
    })?;

    let mut writer = stream.try_clone().map_err(|e| e.to_string())?;
    writeln!(writer, "{command}").map_err(|e| e.to_string())?;
    writer.flush().map_err(|e| e.to_string())?;

    let mut reply = String::new();
    BufReader::new(stream).read_line(&mut reply).map_err(|e| e.to_string())?;

    let reply = reply.trim().to_owned();
    if let Some(error) = reply.strip_prefix("erro ") {
        return Err(error.to_owned());
    }
    Ok(reply)
}

pub fn running(index: u8) -> bool {
    send(index, "ping").is_ok()
}
