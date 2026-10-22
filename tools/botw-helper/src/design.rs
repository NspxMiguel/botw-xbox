// Adapted from NxbHelper's shared visual language.
use eframe::egui::{self, Color32, CornerRadius, FontId, Stroke, TextStyle};

pub const ACCENT: Color32 = Color32::from_rgb(61, 125, 255);
pub const ACCENT_LIGHT: Color32 = Color32::from_rgb(10, 85, 205);
pub const DARK_BORDER: Color32 = Color32::from_rgba_premultiplied(15, 15, 15, 15);
pub const BORDER: Color32 = Color32::from_rgba_premultiplied(15, 15, 15, 15);
pub const INK: Color32 = Color32::from_rgb(12, 12, 12);
pub const LIGHT_BG: Color32 = Color32::from_rgb(242, 242, 244);
pub const DARK_CARD: Color32 = Color32::from_rgb(16, 16, 18);
pub const SECONDARY_DARK: Color32 = Color32::from_gray(180);
pub const SECONDARY_LIGHT: Color32 = Color32::from_gray(90);

pub fn reduced_motion() -> bool {
    std::env::var("NXBOX_REDUCED_MOTION").as_deref() == Ok("1")
}
#[allow(dead_code)]
pub fn motion(ctx: &egui::Context, id: egui::Id, target: bool, seconds: f32) -> f32 {
    if reduced_motion() {
        return if target { 1.0 } else { 0.0 };
    }
    ctx.animate_bool_with_time_and_easing(id, target, seconds, egui::emath::easing::cubic_out)
}
pub fn background(ui: &egui::Ui) -> Color32 {
    if ui.visuals().dark_mode {
        Color32::BLACK
    } else {
        LIGHT_BG
    }
}
pub fn surface(ui: &egui::Ui) -> Color32 {
    if ui.visuals().dark_mode {
        DARK_CARD
    } else {
        Color32::WHITE
    }
}
pub fn secondary(ui: &egui::Ui) -> Color32 {
    if ui.visuals().dark_mode {
        SECONDARY_DARK
    } else {
        SECONDARY_LIGHT
    }
}
pub fn configure(ctx: &egui::Context) {
    for theme in [egui::Theme::Dark, egui::Theme::Light] {
        ctx.style_mut_of(theme, |style| {
            style
                .text_styles
                .insert(TextStyle::Heading, FontId::proportional(34.0));
            style
                .text_styles
                .insert(TextStyle::Body, FontId::proportional(17.0));
            style
                .text_styles
                .insert(TextStyle::Button, FontId::proportional(17.0));
            style
                .text_styles
                .insert(TextStyle::Small, FontId::proportional(15.0));
            style
                .text_styles
                .insert(TextStyle::Monospace, FontId::monospace(15.0));
            style.spacing.item_spacing = egui::vec2(10.0, 12.0);
            style.spacing.button_padding = egui::vec2(20.0, 12.0);
            style.spacing.interact_size.y = 44.0;
            style.animation_time = if reduced_motion() { 0.0 } else { 0.15 };
            let v = &mut style.visuals;
            v.window_shadow = egui::epaint::Shadow::NONE;
            v.popup_shadow = egui::epaint::Shadow::NONE;
            v.panel_fill = if theme == egui::Theme::Dark {
                Color32::BLACK
            } else {
                LIGHT_BG
            };
            v.window_fill = if theme == egui::Theme::Dark {
                DARK_CARD
            } else {
                Color32::WHITE
            };
            v.override_text_color = Some(if theme == egui::Theme::Dark {
                Color32::from_gray(238)
            } else {
                INK
            });
            v.widgets.noninteractive.bg_stroke = Stroke::NONE;
            v.selection.bg_fill = ACCENT_LIGHT;
            v.selection.stroke = Stroke::new(1.0, Color32::WHITE);
            v.widgets.inactive.corner_radius = CornerRadius::same(255);
            v.widgets.hovered.corner_radius = CornerRadius::same(255);
            v.widgets.active.corner_radius = CornerRadius::same(255);
            v.widgets.inactive.bg_stroke = Stroke::new(
                1.0,
                if theme == egui::Theme::Dark {
                    DARK_BORDER
                } else {
                    BORDER
                },
            );
            v.widgets.hovered.bg_stroke = Stroke::new(1.0, ACCENT);
            v.widgets.active.bg_stroke = Stroke::new(1.0, ACCENT);
        });
    }
}
pub fn primary(text: &str) -> egui::Button<'_> {
    egui::Button::new(egui::RichText::new(text).strong().color(Color32::WHITE))
        .fill(ACCENT_LIGHT)
        .stroke(Stroke::NONE)
        .corner_radius(255)
        .min_size(egui::vec2(120.0, 48.0))
}
pub fn centered(ui: &mut egui::Ui, body: impl FnOnce(&mut egui::Ui)) {
    let mut rect = ui.available_rect_before_wrap();
    let width = rect.width().min(560.0);
    rect.min.x += ((rect.width() - width) / 2.0).max(0.0);
    rect.max.x = rect.min.x + width;
    rect.max.y = rect.min.y;
    ui.scope_builder(
        egui::UiBuilder::new()
            .max_rect(rect)
            .layout(egui::Layout::top_down(egui::Align::Min)),
        body,
    );
}
pub fn heading_font(ctx: &egui::Context, size: f32) -> FontId {
    let available = ctx.fonts(|fonts| {
        fonts
            .families()
            .contains(&egui::FontFamily::Name("system-bold".into()))
    });
    if available {
        FontId::new(size, egui::FontFamily::Name("system-bold".into()))
    } else {
        FontId::proportional(size)
    }
}
pub fn configure_fonts(ctx: &egui::Context) {
    let mut fonts = egui::FontDefinitions::default();
    for path in [
        "C:/Windows/Fonts/segoeuib.ttf",
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    ] {
        if let Ok(bytes) = std::fs::read(path) {
            fonts.font_data.insert(
                "system-bold".into(),
                egui::FontData::from_owned(bytes).into(),
            );
            fonts.families.insert(
                egui::FontFamily::Name("system-bold".into()),
                vec!["system-bold".into()],
            );
            ctx.data_mut(|data| data.insert_temp(egui::Id::new("system-bold"), true));
            break;
        }
    }
    ctx.set_fonts(fonts);
}

pub fn text_field(ui: &mut egui::Ui, value: &mut String, password: bool) -> egui::Response {
    ui.scope(|ui| {
        let widgets = &mut ui.visuals_mut().widgets;
        widgets.inactive.corner_radius = CornerRadius::same(16);
        widgets.hovered.corner_radius = CornerRadius::same(16);
        widgets.active.corner_radius = CornerRadius::same(16);
        ui.add(
            egui::TextEdit::singleline(value)
                .password(password)
                .min_size(egui::vec2(0.0, 52.0))
                .margin(egui::Margin::symmetric(14, 12))
                .desired_width(f32::INFINITY),
        )
    })
    .inner
}
