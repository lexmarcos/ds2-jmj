//! The window.
//!
//! One screen: the wordmark, one button, one line of status. Nothing about the
//! server appears anywhere, because the player has no decision to make about
//! it — the address and key are compiled in and there is only one server.
//!
//! Settings exist only as a way out of a check that failed, so the panel holds
//! three paths and nothing else. No patch toggles: the gameplay changes are
//! what the server *is*, and the three area hooks cannot be separated anyway.

use std::process::Child;
use std::time::{Duration, Instant};

use iced::widget::{button, column, container, image, row, text, text_input, Space};
use iced::{Alignment, Element, Length, Subscription, Task};

use crate::launch;
use crate::paths;
use crate::preflight::{self, Problem};
use crate::settings::Settings;
use crate::theme;
use crate::watch::{Signal, Watcher};

/// How long the button's border stays lit after a press.
const IGNITION: Duration = Duration::from_millis(400);

/// How long to wait for the injector to confirm before saying so.
const CONFIRMATION_TIMEOUT: Duration = Duration::from_secs(90);

/// How often anything is looked at. Fast enough to follow a launch closely.
const TICK: Duration = Duration::from_millis(400);

/// The checks are cheap but not free, so while idle they run every fifth tick
/// rather than every one.
const CHECK_EVERY: u64 = 5;

pub struct App {
    settings: Settings,
    phase: Phase,
    child: Option<Child>,
    watcher: Watcher,
    ignited_at: Option<Instant>,
    panel_open: bool,
    fields: Fields,
    /// Built once. `Handle::from_bytes` stamps a fresh id on every call, so
    /// building it inside `view` makes the renderer treat each frame's logo as
    /// a new image: decode the PNG again, upload a new texture, drop the last
    /// one. That reads on screen as the logo flickering and vanishing.
    logo: image::Handle,
    /// Ticks since boot, so the checks can run less often than the frame that
    /// follows the game.
    ticks: u64,
}

#[derive(Default)]
struct Fields {
    steam_root: String,
    game_dir: String,
    proton_dir: String,
}

enum Phase {
    /// Something has to be fixed before the game can start.
    Blocked(Problem),
    Ready,
    /// Started, but the injector has not confirmed yet.
    Starting { since: Instant },
    Playing,
    /// Started and confirmed, but against the retail servers.
    Astray,
    Failed(String),
}

#[derive(Debug, Clone)]
pub enum Message {
    Tick,
    Play,
    TogglePanel,
    SteamRoot(String),
    GameDir(String),
    ProtonDir(String),
    SaveSettings,
    OpenLogs,
}

impl App {
    pub fn boot() -> (Self, Task<Message>) {
        let settings = Settings::load();
        let fields = Fields {
            steam_root: shown(&settings.steam_root),
            game_dir: shown(&settings.game_dir),
            proton_dir: shown(&settings.proton_dir),
        };

        let mut app = Self {
            phase: Phase::Ready,
            child: None,
            watcher: Watcher::new(),
            ignited_at: None,
            panel_open: false,
            settings,
            fields,
            logo: image::Handle::from_bytes(theme::LOGO),
            ticks: 0,
        };
        app.recheck();

        (app, Task::none())
    }

    pub fn title(&self) -> String {
        "Dark Souls 2 Jamuja Edition".to_owned()
    }

    pub fn theme(&self) -> iced::Theme {
        iced::Theme::custom(
            "Jamuja".to_owned(),
            iced::theme::Palette {
                background: theme::VOID,
                text: theme::BONE,
                primary: theme::EMBER,
                success: theme::EMBER,
                warning: theme::EMBER,
                danger: theme::BLOOD,
            },
        )
    }

    /// A steady tick drives everything: re-checking while idle so that opening
    /// Steam enables the button on its own, and following the game while it
    /// runs. Nothing here blocks — the fingerprint is cached and the logs are
    /// read from where they were left.
    pub fn subscription(&self) -> Subscription<Message> {
        // One period, always. Returning a different duration per phase makes
        // iced tear the subscription down and build a new one every time the
        // phase changes, which is churn for no gain.
        iced::time::every(TICK).map(|_| Message::Tick)
    }

    pub fn update(&mut self, message: Message) {
        match message {
            Message::Tick => self.tick(),
            Message::Play => self.play(),
            Message::TogglePanel => self.panel_open = !self.panel_open,
            Message::SteamRoot(value) => self.fields.steam_root = value,
            Message::GameDir(value) => self.fields.game_dir = value,
            Message::ProtonDir(value) => self.fields.proton_dir = value,
            Message::SaveSettings => self.save_settings(),
            Message::OpenLogs => open_folder(&paths::injector_dir()),
        }
    }

    fn tick(&mut self) {
        self.ticks = self.ticks.wrapping_add(1);

        if matches!(self.phase, Phase::Starting { .. } | Phase::Playing | Phase::Astray) {
            self.follow_game();
        } else if self.ticks % CHECK_EVERY == 0 {
            self.recheck();
        }
    }

    /// Re-runs the checks, keeping any failure the player is already looking at
    /// from flickering when it has not changed.
    fn recheck(&mut self) {
        match preflight::run(&self.settings) {
            Ok(_) => self.phase = Phase::Ready,
            Err(problem) => self.phase = Phase::Blocked(problem),
        }
    }

    fn play(&mut self) {
        let ready = match preflight::run(&self.settings) {
            Ok(ready) => ready,
            Err(problem) => {
                self.phase = Phase::Blocked(problem);
                return;
            }
        };

        self.ignited_at = Some(Instant::now());
        // From the current end of the files, so nothing left over can be read
        // as if it belonged to this run.
        self.watcher = Watcher::new();

        match launch::start(&ready) {
            Ok(child) => {
                self.child = Some(child);
                self.phase = Phase::Starting { since: Instant::now() };
            }
            Err(error) => self.phase = Phase::Failed(error.to_string()),
        }
    }

    fn follow_game(&mut self) {
        for signal in self.watcher.poll() {
            match signal {
                // The one that means the hooks are really in: reaching a later
                // hook proves the redirect hook returned rather than spinning.
                Signal::HooksInstalled => self.phase = Phase::Playing,
                Signal::NoInjector => self.phase = Phase::Astray,
                Signal::CouldNotStart => {
                    self.phase = Phase::Failed("o injector não conseguiu abrir o jogo".to_owned())
                }
                // Neither of these is success, whatever they read like.
                Signal::GameStarted | Signal::DllLoaded => {}
            }
        }

        let exited = match self.child.as_mut().map(Child::try_wait) {
            Some(Ok(Some(_))) => true,
            Some(Err(_)) => true,
            _ => false,
        };

        if exited {
            self.child = None;
            self.phase = Phase::Ready;
            self.recheck();
            return;
        }

        if let Phase::Starting { since } = self.phase {
            if since.elapsed() > CONFIRMATION_TIMEOUT {
                self.phase = Phase::Failed(
                    "o jogo abriu mas o injector não confirmou. Veja os logs".to_owned(),
                );
            }
        }
    }

    fn save_settings(&mut self) {
        self.settings.steam_root = taken(&self.fields.steam_root);
        self.settings.game_dir = taken(&self.fields.game_dir);
        self.settings.proton_dir = taken(&self.fields.proton_dir);
        let _ = self.settings.save();
        self.panel_open = false;
        self.recheck();
    }

    /// How far through the ignition the button is, 0 to 1 and back to 0.
    fn glow(&self) -> f32 {
        let Some(at) = self.ignited_at else {
            return 0.0;
        };
        let elapsed = at.elapsed();
        if elapsed > IGNITION {
            return 0.0;
        }
        let through = elapsed.as_secs_f32() / IGNITION.as_secs_f32();
        // Up and back down, so it reads as a flare rather than a fade.
        (through * std::f32::consts::PI).sin()
    }

    pub fn view(&self) -> Element<'_, Message> {
        let body = if self.panel_open {
            self.panel()
        } else {
            self.home()
        };

        container(body)
            .width(Length::Fill)
            .height(Length::Fill)
            .padding(28)
            .style(theme::background)
            .into()
    }

    fn home(&self) -> Element<'_, Message> {
        let logo = image(self.logo.clone())
            .width(Length::Fixed(600.0))
            .content_fit(iced::ContentFit::Contain);

        let can_play = matches!(self.phase, Phase::Ready | Phase::Failed(_));
        let label = match self.phase {
            Phase::Starting { .. } => "Iniciando…",
            Phase::Playing | Phase::Astray => "Jogando",
            _ => "Jogar",
        };

        let mut play = button(
            text(label)
                .font(theme::SPECTRAL_MEDIUM)
                .size(20)
                .align_x(Alignment::Center)
                .width(Length::Fill),
        )
        .width(Length::Fixed(200.0))
        .padding([12, 24])
        .style(theme::play(self.glow()));

        if can_play {
            play = play.on_press(Message::Play);
        }

        column![
            Space::new().height(Length::Fixed(8.0)),
            logo,
            Space::new().height(Length::Fill),
            play,
            Space::new().height(Length::Fill),
            self.status_row(),
        ]
        .align_x(Alignment::Center)
        .spacing(0)
        .into()
    }

    fn status_row(&self) -> Element<'_, Message> {
        let (message, colour) = self.status();

        let line = text(message)
            .font(theme::SPECTRAL_LIGHT)
            .size(13)
            .color(colour);

        let gear = button(text("configurações").font(theme::SPECTRAL).size(13))
            .style(theme::quiet)
            .padding(0)
            .on_press(Message::TogglePanel);

        row![line, Space::new().width(Length::Fill), gear]
            .align_y(Alignment::Center)
            .into()
    }

    fn status(&self) -> (String, iced::Color) {
        match &self.phase {
            Phase::Blocked(problem) => (problem.message(), theme::BLOOD),
            Phase::Ready => ("pronto para jogar".to_owned(), theme::ASH),
            Phase::Starting { .. } => ("iniciando o jogo…".to_owned(), theme::ASH),
            Phase::Playing => (
                "jogando — feche o jogo para voltar".to_owned(),
                theme::EMBER,
            ),
            Phase::Astray => (
                "o jogo abriu SEM o servidor privado".to_owned(),
                theme::BLOOD,
            ),
            Phase::Failed(why) => (why.clone(), theme::BLOOD),
        }
    }

    fn panel(&self) -> Element<'_, Message> {
        let field = |label: &'static str, hint: &'static str, value: &str, on_change: fn(String) -> Message| {
            column![
                text(label).font(theme::SPECTRAL).size(14).color(theme::BONE),
                text_input(hint, value)
                    .on_input(on_change)
                    .font(theme::SPECTRAL)
                    .size(14)
                    .padding(8),
            ]
            .spacing(6)
        };

        let inner = column![
            text("Configurações")
                .font(theme::SPECTRAL_MEDIUM)
                .size(20)
                .color(theme::BONE),
            text("Só preencha o que o loader não achou sozinho.")
                .font(theme::SPECTRAL_LIGHT)
                .size(13)
                .color(theme::ASH),
            Space::new().height(Length::Fixed(8.0)),
            field("Pasta da Steam", "detectada automaticamente", &self.fields.steam_root, Message::SteamRoot),
            field("Pasta do jogo", "detectada automaticamente", &self.fields.game_dir, Message::GameDir),
            field("Proton", "detectado pelo prefixo do jogo", &self.fields.proton_dir, Message::ProtonDir),
            Space::new().height(Length::Fill),
            row![
                button(text("Abrir pasta de logs").font(theme::SPECTRAL).size(13))
                    .style(theme::quiet)
                    .padding(0)
                    .on_press(Message::OpenLogs),
                Space::new().width(Length::Fill),
                button(text("Voltar").font(theme::SPECTRAL).size(14))
                    .style(theme::play(0.0))
                    .padding([8, 20])
                    .on_press(Message::SaveSettings),
            ]
            .align_y(Alignment::Center),
        ]
        .spacing(10);

        container(inner)
            .width(Length::Fill)
            .height(Length::Fill)
            .padding(24)
            .style(theme::panel)
            .into()
    }
}

fn shown(path: &Option<std::path::PathBuf>) -> String {
    path.as_ref()
        .map(|p| p.display().to_string())
        .unwrap_or_default()
}

fn taken(value: &str) -> Option<std::path::PathBuf> {
    let value = value.trim();
    (!value.is_empty()).then(|| std::path::PathBuf::from(value))
}

/// Best effort. A file manager that is not there is not worth an error.
fn open_folder(path: &std::path::Path) {
    let opener = if cfg!(windows) { "explorer" } else { "xdg-open" };
    let _ = std::process::Command::new(opener).arg(path).spawn();
}
