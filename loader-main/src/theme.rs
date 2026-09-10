//! The look, taken from the logo rather than invented.
//!
//! The wordmark is eroded stone with molten cracks and a bone-white legend on
//! a barbed bar. It is detailed and loud, so it gets to be the only loud thing
//! here: everything else is quiet, flat, and drawn from colours sampled out of
//! that same image.
//!
//! Ember is the accent because it is what glows in the cracks. It appears on
//! the one thing the player can press and nowhere else. Blood is for failure
//! and is never decoration.

use iced::{Color, Font};

/// Sampled from the logo's stone, with a warm cast so it sits under the
/// artwork rather than fighting it with a blue-black.
pub const VOID: Color = rgb(0x0E, 0x0D, 0x0C);
/// Raised surfaces: the settings panel.
pub const STONE: Color = rgb(0x1A, 0x18, 0x16);
/// Borders and dividers.
pub const IRON: Color = rgb(0x2E, 0x2A, 0x27);
/// Secondary text. The logo's mid-tone stone.
pub const ASH: Color = rgb(0x6A, 0x63, 0x5F);
/// Primary text. The lightest pixel in the logo, its "JAMUJA EDITION" legend.
pub const BONE: Color = rgb(0xD5, 0xC4, 0xBB);
/// The accent, from the molten cracks.
pub const EMBER: Color = rgb(0xF0, 0x6D, 0x1F);
/// The white-hot centre of a crack. Used once, when the button ignites.
pub const EMBER_HOT: Color = rgb(0xFF, 0xFF, 0xB7);
/// Failure. Sampled from the logo's deepest rust, then lifted: at the sampled
/// value it is legible as a fill and not as 13px text on near-black, which is
/// the only place it is actually used.
pub const BLOOD: Color = rgb(0xC4, 0x55, 0x3F);

pub const SPECTRAL: Font = Font::with_name("Spectral");
pub const SPECTRAL_LIGHT: Font = Font {
    weight: iced::font::Weight::Light,
    ..Font::with_name("Spectral")
};
pub const SPECTRAL_MEDIUM: Font = Font {
    weight: iced::font::Weight::Medium,
    ..Font::with_name("Spectral")
};

pub const FONT_REGULAR: &[u8] = include_bytes!("../assets/fonts/Spectral-Regular.ttf");
pub const FONT_LIGHT: &[u8] = include_bytes!("../assets/fonts/Spectral-Light.ttf");
pub const FONT_MEDIUM: &[u8] = include_bytes!("../assets/fonts/Spectral-Medium.ttf");

pub const LOGO: &[u8] = include_bytes!("../assets/logo.png");

const fn rgb(r: u8, g: u8, b: u8) -> Color {
    Color {
        r: r as f32 / 255.0,
        g: g as f32 / 255.0,
        b: b as f32 / 255.0,
        a: 1.0,
    }
}

pub const fn alpha(color: Color, a: f32) -> Color {
    Color { a, ..color }
}

/// Mixes towards `to`, for the one animated moment in the interface.
pub fn mix(from: Color, to: Color, amount: f32) -> Color {
    let amount = amount.clamp(0.0, 1.0);
    Color {
        r: from.r + (to.r - from.r) * amount,
        g: from.g + (to.g - from.g) * amount,
        b: from.b + (to.b - from.b) * amount,
        a: from.a + (to.a - from.a) * amount,
    }
}

/// The window's own background, so the logo's transparency lands on stone.
pub fn background(_theme: &iced::Theme) -> iced::widget::container::Style {
    iced::widget::container::Style {
        background: Some(VOID.into()),
        ..Default::default()
    }
}

/// The settings panel: raised a little, framed, still flat.
pub fn panel(_theme: &iced::Theme) -> iced::widget::container::Style {
    iced::widget::container::Style {
        background: Some(STONE.into()),
        border: iced::Border {
            color: IRON,
            width: 1.0,
            radius: 0.0.into(),
        },
        ..Default::default()
    }
}

/// The one thing to press.
///
/// A flat framed slab, not a rounded card: the series' own menus are drawn
/// that way, and a soft shadow under a pill would belong to a different
/// product entirely. `glow` carries the ignition, from nothing to the hot
/// centre of a crack and back, once, when it is pressed.
pub fn play(glow: f32) -> impl Fn(&iced::Theme, iced::widget::button::Status) -> iced::widget::button::Style {
    move |_theme, status| {
        use iced::widget::button::Status;

        let border = mix(EMBER, EMBER_HOT, glow);
        let (background, text) = match status {
            Status::Hovered => (Some(alpha(EMBER, 0.12).into()), BONE),
            Status::Pressed => (Some(EMBER.into()), VOID),
            Status::Disabled => (None, ASH),
            Status::Active => (None, BONE),
        };

        iced::widget::button::Style {
            background,
            text_color: text,
            border: iced::Border {
                color: if matches!(status, Status::Disabled) {
                    // Bright enough to read as a button that is off, rather
                    // than as an empty rectangle.
                    mix(IRON, ASH, 0.5)
                } else {
                    border
                },
                width: 1.0,
                radius: 0.0.into(),
            },
            ..Default::default()
        }
    }
}

/// The settings gear and anything else that must not draw the eye.
pub fn quiet(_theme: &iced::Theme, status: iced::widget::button::Status) -> iced::widget::button::Style {
    use iced::widget::button::Status;

    iced::widget::button::Style {
        background: None,
        text_color: match status {
            Status::Hovered | Status::Pressed => BONE,
            _ => ASH,
        },
        border: iced::Border::default(),
        ..Default::default()
    }
}
