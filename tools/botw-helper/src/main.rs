mod cover;
mod design;
mod i18n;

use eframe::egui;
use i18n::text as t;
use serde::{Deserialize, Serialize};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::mpsc::{Receiver, channel};

#[derive(Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
struct Preferences {
    auto_launch: bool,
    fps: i32,
    volume: i32,
    button_layout: i32,
    wallpaper: i32,
    render_scale_percent: i32,
    upscaler: i32,
    fsr_quality: i32,
    frame_generation: i32,
}
impl Default for Preferences {
    fn default() -> Self {
        Self {
            auto_launch: false,
            fps: 60,
            volume: 100,
            button_layout: 1,
            wallpaper: -1,
            render_scale_percent: 0,
            upscaler: 0,
            fsr_quality: 0,
            frame_generation: 0,
        }
    }
}

#[derive(Clone, Default, Serialize, Deserialize)]
#[serde(default)]
struct Connection {
    host: String,
    user: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    password: Option<String>,
}
fn connection_path() -> PathBuf {
    dirs::config_dir()
        .unwrap_or_else(std::env::temp_dir)
        .join("BotWHelper/connection.json")
}
fn save_connection(connection: &Connection) -> std::io::Result<()> {
    let path = connection_path();
    let parent = path.parent().unwrap();
    std::fs::create_dir_all(parent)?;
    let mut file = tempfile::NamedTempFile::new_in(parent)?;
    // NamedTempFile is owner-only on Unix. Replace atomically to remove old passwords.
    file.write_all(&serde_json::to_vec_pretty(connection)?)?;
    file.persist(path).map_err(|error| error.error)?;
    Ok(())
}

struct Cover {
    // The temporary directory owns all generated artwork; nothing enters the repo.
    directory: tempfile::TempDir,
    square: egui::TextureHandle,
    wide: egui::TextureHandle,
}
impl Cover {
    fn load(
        ctx: &egui::Context,
        path: &Path,
        game: bool,
    ) -> Result<Self, Box<dyn std::error::Error>> {
        let (icon, wide) = if game {
            (
                cover::decode_tga(&std::fs::read(path.join("meta/iconTex.tga"))?)?,
                cover::decode_tga(&std::fs::read(path.join("meta/bootTvTex.tga"))?)?,
            )
        } else {
            let image = image::open(path)?;
            (image.clone(), image)
        };
        let directory = tempfile::tempdir()?;
        cover::write_covers(&icon, &wide, directory.path())?;
        let texture = |name: &str| -> Result<_, image::ImageError> {
            let image = image::open(directory.path().join(name))?.to_rgba8();
            Ok(ctx.load_texture(
                name,
                egui::ColorImage::from_rgba_unmultiplied(
                    [image.width() as usize, image.height() as usize],
                    image.as_raw(),
                ),
                egui::TextureOptions::LINEAR,
            ))
        };
        let square = texture("cover.png")?;
        let wide = texture("cover-wide.png")?;
        Ok(Self {
            directory,
            square,
            wide,
        })
    }
}

fn run_cli(
    command: &str,
    input: Option<&Path>,
    connection: &Connection,
    password: &str,
) -> Result<(), String> {
    let cli = std::env::var_os("BOTW_HELPER_CLI")
        .map(PathBuf::from)
        .unwrap_or_else(|| Path::new(env!("CARGO_MANIFEST_DIR")).join("cli.ts"));
    let mut process = Command::new("bun");
    process.arg(cli).arg(command);
    if let Some(path) = input {
        process.arg(path);
    }
    process
        .env("XBDEV_HOST", connection.host.trim())
        .env("XBDEV_USER", &connection.user)
        .env("XBDEV_PASS", password)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    let mut child = process.spawn().map_err(|error| error.to_string())?;
    let start = std::time::Instant::now();
    loop {
        if child
            .try_wait()
            .map_err(|error| error.to_string())?
            .is_some()
        {
            break;
        }
        if start.elapsed() > std::time::Duration::from_secs(60) {
            let _ = child.kill();
            let _ = child.wait();
            return Err("Portal operation timed out after 60 seconds".into());
        }
        std::thread::sleep(std::time::Duration::from_millis(50));
    }
    let output = child
        .wait_with_output()
        .map_err(|error| error.to_string())?;
    if output.status.success() {
        Ok(())
    } else {
        Err(String::from_utf8_lossy(&output.stderr).trim().to_owned())
    }
}

struct App {
    connection: Connection,
    password: String,
    remember: bool,
    preferences: Preferences,
    cover: Option<Cover>,
    pending: Option<Receiver<Result<&'static str, String>>>,
    status: String,
}
impl Default for App {
    fn default() -> Self {
        let connection: Connection = std::fs::read(connection_path())
            .ok()
            .and_then(|bytes| serde_json::from_slice(&bytes).ok())
            .unwrap_or_default();
        Self {
            password: connection.password.clone().unwrap_or_default(),
            remember: connection.password.is_some(),
            connection,
            preferences: Preferences::default(),
            cover: None,
            pending: None,
            status: String::new(),
        }
    }
}
impl App {
    fn start(&mut self, launch: bool) {
        let connection = self.connection.clone();
        let password = self.password.clone();
        let preferences = self.preferences.clone();
        // Copy the cover before spawning so changing selection cannot remove in-flight files.
        let staged = (|| -> Result<_, Box<dyn std::error::Error>> {
            let directory = tempfile::tempdir()?;
            std::fs::write(
                directory.path().join("helper.json"),
                serde_json::to_vec_pretty(&preferences)?,
            )?;
            if !launch && let Some(cover) = &self.cover {
                for name in ["cover.png", "cover-wide.png"] {
                    std::fs::copy(
                        cover.directory.path().join(name),
                        directory.path().join(name),
                    )?;
                }
            }
            Ok(directory)
        })();
        let directory = match staged {
            Ok(directory) => directory,
            Err(error) => {
                self.status = format!("{}\n{error}", t("failed"));
                return;
            }
        };
        let send_cover = self.cover.is_some();
        let (tx, rx) = channel();
        self.pending = Some(rx);
        self.status.clear();
        std::thread::spawn(move || {
            let result = (|| {
                if launch {
                    run_cli("launch", None, &connection, &password)?;
                    return Ok("launched");
                }
                // Upload artwork first so a cover failure does not consume a new config.
                if send_cover {
                    run_cli("push-cover", Some(directory.path()), &connection, &password)?;
                }
                run_cli(
                    "push-config",
                    Some(&directory.path().join("helper.json")),
                    &connection,
                    &password,
                )?;
                Ok("sent")
            })();
            let _ = tx.send(result);
        });
    }

    fn choose_cover(&mut self, ctx: &egui::Context, game: bool) {
        let dialog = rfd::FileDialog::new();
        let selected = if game {
            dialog.pick_folder()
        } else {
            dialog
                .add_filter("PNG / JPG", &["png", "jpg", "jpeg"])
                .pick_file()
        };
        if let Some(path) = selected {
            match Cover::load(ctx, &path, game) {
                Ok(cover) => {
                    self.cover = Some(cover);
                    self.status.clear();
                }
                Err(error) => self.status = format!("{}\n{error}", t("cover_failed")),
            }
        }
    }

    fn render(&mut self, ui: &mut egui::Ui) {
        let ctx = ui.ctx().clone();
        ui.add_space(20.0);
        ui.label(egui::RichText::new("BotW Helper").font(design::heading_font(&ctx, 34.0)));
        ui.label(egui::RichText::new(t("intro")).color(design::secondary(ui)));
        ui.add_space(20.0);
        ui.label(egui::RichText::new(t("connection")).size(22.0).strong());
        ui.label(t("connection_hint"));
        let mut changed = false;
        for (key, value) in [
            ("host", &mut self.connection.host),
            ("user", &mut self.connection.user),
        ] {
            ui.label(t(key));
            changed |= design::text_field(ui, value, false).changed();
        }
        ui.label(t("password"));
        changed |= design::text_field(ui, &mut self.password, true).changed();
        changed |= ui.checkbox(&mut self.remember, t("remember")).changed();
        if changed {
            self.connection.password = self.remember.then(|| self.password.clone());
            if save_connection(&self.connection).is_err() {
                self.status = t("save_failed").into();
            }
        }
        ui.add_space(20.0);
        ui.label(egui::RichText::new(t("preferences")).size(22.0).strong());
        ui.checkbox(&mut self.preferences.auto_launch, t("auto_launch"));
        ui.small(t("setup_hint"));
        ui.horizontal_wrapped(|ui| {
            ui.label(t("fps"));
            ui.selectable_value(&mut self.preferences.fps, 30, "30");
            ui.selectable_value(&mut self.preferences.fps, 60, "60");
        });
        ui.add(egui::Slider::new(&mut self.preferences.volume, 0..=100).text(t("volume")));
        ui.horizontal_wrapped(|ui| {
            ui.label(t("buttons"));
            ui.selectable_value(&mut self.preferences.button_layout, 1, "Xbox");
            ui.selectable_value(&mut self.preferences.button_layout, 0, "Nintendo");
        });
        ui.collapsing(t("more"), |ui| {
            ui.horizontal_wrapped(|ui| {
                ui.label(t("wallpaper"));
                ui.selectable_value(&mut self.preferences.wallpaper, -1, t("default"));
                for index in 0..=3 {
                    ui.selectable_value(&mut self.preferences.wallpaper, index, index.to_string());
                }
            });
            for (key, value, maximum) in [
                (
                    "render_scale",
                    &mut self.preferences.render_scale_percent,
                    300,
                ),
                ("upscaler", &mut self.preferences.upscaler, 3),
                ("fsr_quality", &mut self.preferences.fsr_quality, 4),
                (
                    "frame_generation",
                    &mut self.preferences.frame_generation,
                    2,
                ),
            ] {
                ui.label(t(key));
                ui.add(egui::Slider::new(value, 0..=maximum));
            }
        });
        ui.add_space(20.0);
        ui.label(egui::RichText::new(t("cover")).size(22.0).strong());
        if let Some(cover) = &self.cover {
            ui.horizontal(|ui| {
                let width = ((ui.available_width() - 12.0) / 3.07).min(150.0);
                ui.add(
                    egui::Image::new(&cover.square)
                        .fit_to_exact_size(egui::vec2(width, width))
                        .corner_radius(22),
                );
                ui.add(
                    egui::Image::new(&cover.wide)
                        .fit_to_exact_size(egui::vec2(width * 620.0 / 300.0, width))
                        .corner_radius(22),
                );
            });
        } else {
            egui::Frame::new()
                .fill(design::surface(ui))
                .corner_radius(24)
                .inner_margin(24)
                .show(ui, |ui| {
                    ui.set_min_height(90.0);
                    ui.label(t("cover_hint"));
                });
        }
        ui.horizontal_wrapped(|ui| {
            if ui.button(t("choose_image")).clicked() {
                self.choose_cover(&ctx, false);
            }
            if ui.button(t("own_game")).clicked() {
                self.choose_cover(&ctx, true);
            }
            if ui.button(t("keep_default")).clicked() {
                self.cover = None;
            }
        });
        if self.cover.is_none() {
            ui.small(t("default_hint"));
        }
        ui.add_space(20.0);
        ui.add_enabled_ui(
            !self.connection.host.trim().is_empty() && self.pending.is_none(),
            |ui| {
                ui.horizontal_wrapped(|ui| {
                    if ui.add(design::primary(t("send"))).clicked() {
                        self.start(false);
                    }
                    if ui.button(t("launch")).clicked() {
                        self.start(true);
                    }
                });
            },
        );
        if self.pending.is_some() {
            ui.horizontal(|ui| {
                ui.spinner();
                ui.label(t("busy"));
            });
        }
        if !self.status.is_empty() {
            ui.label(&self.status);
        }
        ui.add_space(24.0);
        ui.label(
            egui::RichText::new("© 2026 NSPX")
                .small()
                .color(design::secondary(ui)),
        );
    }
}
impl eframe::App for App {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        if let Some(receiver) = &self.pending {
            match receiver.try_recv() {
                Ok(result) => {
                    self.status = match result {
                        Ok(key) => t(key).into(),
                        Err(error) => format!("{}\n{error}", t("failed")),
                    };
                    self.pending = None;
                }
                Err(std::sync::mpsc::TryRecvError::Disconnected) => {
                    self.status = t("failed").into();
                    self.pending = None;
                }
                Err(std::sync::mpsc::TryRecvError::Empty) => {}
            }
        }
        if self.pending.is_some() {
            ui.ctx()
                .request_repaint_after(std::time::Duration::from_millis(100));
        }
        egui::Frame::new()
            .fill(design::background(ui))
            .show(ui, |ui| {
                ui.set_min_size(ui.available_size());
                egui::ScrollArea::vertical().show(ui, |ui| {
                    design::centered(ui, |ui| self.render(ui));
                });
            });
    }
}
fn main() -> eframe::Result {
    eframe::run_native(
        "BotW Helper",
        eframe::NativeOptions {
            viewport: egui::ViewportBuilder::default()
                .with_inner_size([700.0, 900.0])
                .with_min_inner_size([560.0, 560.0]),
            ..Default::default()
        },
        Box::new(|cc| {
            cc.egui_ctx.set_theme(egui::ThemePreference::System);
            design::configure(&cc.egui_ctx);
            design::configure_fonts(&cc.egui_ctx);
            Ok(Box::<App>::default())
        }),
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn helper_json_has_exact_console_keys_and_defaults() {
        let value = serde_json::to_value(Preferences::default()).unwrap();
        assert_eq!(
            value,
            serde_json::json!({
                "autoLaunch": false, "fps": 60, "volume": 100, "buttonLayout": 1,
                "wallpaper": -1, "renderScalePercent": 0, "upscaler": 0,
                "fsrQuality": 0, "frameGeneration": 0
            })
        );
    }
    #[test]
    fn unsaved_password_is_absent_from_connection_json() {
        let value = serde_json::to_value(Connection {
            host: "xbox".into(),
            user: "user".into(),
            password: None,
        })
        .unwrap();
        assert!(value.get("password").is_none());
    }
}
