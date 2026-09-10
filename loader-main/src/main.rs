// No console window behind the loader on Windows. Kept in debug builds so
// --doctor still prints somewhere.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod app;
mod launch;
mod paths;
mod pinned;
mod preflight;
mod settings;
mod theme;
mod watch;

fn main() -> iced::Result {
    // A report anyone can paste, without needing the window to come up.
    if std::env::args().any(|arg| arg == "--doctor") {
        preflight::doctor(&settings::Settings::load());
        return Ok(());
    }

    iced::application(app::App::boot, app::App::update, app::App::view)
        .title(app::App::title)
        .theme(app::App::theme)
        .subscription(app::App::subscription)
        .window_size((760.0, 440.0))
        .resizable(false)
        .centered()
        .font(theme::FONT_REGULAR)
        .font(theme::FONT_LIGHT)
        .font(theme::FONT_MEDIUM)
        .run()
}
