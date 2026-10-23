#define _GNU_SOURCE
#include <adwaita.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include "analyze.h"
#include "bindings.h"
#include "config.h"
#include "lockscreen.h"
#include "runtime.h"
#include "stb_image.h"

#ifndef APP_ID
#define APP_ID "io.github.casper_rs.driftpaper"
#endif
#define SAVE_DELAY_MS 300
#define STATUS_INTERVAL_S 2
#define COLOR_SLOTS 4

static struct {
    GtkWindow *window;
    AdwToastOverlay *toasts;
    struct config cfg;
    struct analysis analysis;
    guint save_timeout;
    gboolean updating;
    GtkWidget *image_row, *image_group, *preset_row, *enabled_row, *start_button, *login_row;
    struct monitor {
        char connector[32];
        GtkWidget *tile, *picture, *reset, *show;
    } monitors[MAX_OUTPUTS_FILTER];
    int n_monitors;
} ui;

static void toast(const char *message) {
    adw_toast_overlay_add_toast(ui.toasts, adw_toast_new(message));
}

static char *sibling_executable(const char *name) {
    g_autofree char *self = g_file_read_link("/proc/self/exe", NULL);
    if (self) {
        g_autofree char *dir = g_path_get_dirname(self);
        char *path = g_build_filename(dir, name, NULL);
        if (g_file_test(path, G_FILE_TEST_IS_EXECUTABLE)) return path;
        g_free(path);
    }
    return g_strdup(name);
}

static void write_config(void) {
    const char *path = config_default_path();
    g_autofree char *dir = g_path_get_dirname(path);
    g_autofree char *temp = g_strconcat(path, ".tmp", NULL);
    g_mkdir_with_parents(dir, 0700);

    FILE *f = fopen(temp, "w");
    if (!f) {
        toast("Could not save the settings");
        return;
    }
    fprintf(f, "# Written by driftpaper settings. All options: examples/config.ini in the source.\n\n");
    config_print(&ui.cfg, f);
    // A full disk must not replace good settings with a cut-off file.
    if (ferror(f) | fclose(f) || rename(temp, path)) {
        g_unlink(temp);
        toast("Could not save the settings");
    }
}

static gboolean save_now(gpointer data) {
    ui.save_timeout = 0;
    write_config();
    return G_SOURCE_REMOVE;
}

static void schedule_save(void) {
    if (ui.save_timeout) g_source_remove(ui.save_timeout);
    ui.save_timeout = g_timeout_add(SAVE_DELAY_MS, save_now, NULL);
}

static void flush_save(void) {
    if (!ui.save_timeout) return;
    g_source_remove(ui.save_timeout);
    save_now(NULL);
}

static void use_detected_colors(void) {
    if (ui.cfg.colors_auto) {
        int n = ui.analysis.n_palette < COLOR_SLOTS ? ui.analysis.n_palette : COLOR_SLOTS;
        memcpy(ui.cfg.colors, ui.analysis.palette, sizeof ui.cfg.colors[0] * n);
        ui.cfg.n_colors = n;
    }
    if (ui.cfg.glow_auto) memcpy(ui.cfg.glow_color, ui.analysis.palette[0], sizeof ui.cfg.glow_color);
    if (ui.cfg.music_colors_auto) {
        // The same two colours the wallpaper takes from the glow.
        int second = ui.cfg.glow_auto && ui.analysis.n_palette > 1;
        memcpy(ui.cfg.music_colors[0], ui.cfg.glow_color, sizeof ui.cfg.music_colors[0]);
        memcpy(ui.cfg.music_colors[1], second ? ui.analysis.palette[1] : ui.cfg.glow_color, sizeof ui.cfg.music_colors[1]);
    }
}

static int monitor_shown(const char *connector) {
    if (ui.cfg.n_outputs == 0) return 1;
    for (int i = 0; i < ui.cfg.n_outputs; i++)
        if (!strcmp(ui.cfg.outputs[i], connector)) return 1;
    return 0;
}

static void refresh_monitor_tiles(void) {
    for (int i = 0; i < ui.n_monitors; i++) {
        struct monitor *m = &ui.monitors[i];
        const char *picture = ui.cfg.span ? ui.cfg.image : config_image_for(&ui.cfg, m->connector);
        gtk_picture_set_filename(GTK_PICTURE(m->picture), picture[0] ? picture : NULL);
        gtk_widget_set_visible(m->reset, picture != ui.cfg.image);
        gtk_widget_set_opacity(m->tile, monitor_shown(m->connector) ? 1 : 0.35);
    }
}

static void describe_image(void) {
    g_autofree char *name = ui.cfg.image[0] ? g_path_get_basename(ui.cfg.image) : g_strdup("No image chosen");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(ui.image_row), name);
    refresh_monitor_tiles();

    const char *summary = !ui.cfg.image[0] ? "Choose a picture to animate."
                          : ui.analysis.found_subject ? "Subject found: particles keep a clear space around it."
                                                      : "No single subject found: particles fill the whole screen.";
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(ui.image_group), summary);
}

static void analyze_current_image(void) {
    memset(&ui.analysis, 0, sizeof ui.analysis);
    ui.analysis.n_palette = 1;
    ui.analysis.palette[0][0] = ui.analysis.palette[0][1] = ui.analysis.palette[0][2] = 1;

    int w, h, channels;
    unsigned char *pixels = ui.cfg.image[0] ? stbi_load(ui.cfg.image, &w, &h, &channels, 4) : NULL;
    if (pixels) {
        analyze_image(pixels, w, h, ui.cfg.ellipse, &ui.analysis);
        stbi_image_free(pixels);
    }
    use_detected_colors();
    describe_image();
}

static void show_custom_preset(void) {
    ui.updating = TRUE;
    adw_combo_row_set_selected(ADW_COMBO_ROW(ui.preset_row), 0);
    ui.updating = FALSE;
}

static void on_setting_changed(void) {
    show_custom_preset();
    refresh_monitor_tiles();
    schedule_save();
}

// Browse buttons pass 0 for the main picture and 1 + index for a monitor's own picture.
static void on_image_chosen(GObject *source, GAsyncResult *result, gpointer data) {
    int target = GPOINTER_TO_INT(data);
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
    if (!file) return;
    g_autofree char *path = g_file_get_path(file);
    if (!path) {
        toast("That location can't be used, pick a local file");
        return;
    }
    if (target == 0) {
        snprintf(ui.cfg.image, sizeof ui.cfg.image, "%s", path);
        analyze_current_image();
        bindings_refresh();
    } else {
        config_set_output_image(&ui.cfg, ui.monitors[target - 1].connector, path);
        refresh_monitor_tiles();
    }
    schedule_save();
}

static GtkFileFilter *image_filter(void) {
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Images");
    const char *types[] = {"image/png", "image/jpeg", "image/bmp", "image/gif", "image/x-tga", "image/x-targa"};
    for (size_t i = 0; i < G_N_ELEMENTS(types); i++) gtk_file_filter_add_mime_type(filter, types[i]);
    return filter;
}

static void on_browse_clicked(GtkButton *button, gpointer data) {
    g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Choose a wallpaper image");
    g_autoptr(GtkFileFilter) filter = image_filter();
    gtk_file_dialog_set_default_filter(dialog, filter);

    g_autofree char *folder = ui.cfg.image[0] ? g_path_get_dirname(ui.cfg.image)
                                              : g_strdup(g_get_user_special_dir(G_USER_DIRECTORY_PICTURES));
    if (folder) {
        g_autoptr(GFile) initial = g_file_new_for_path(folder);
        gtk_file_dialog_set_initial_folder(dialog, initial);
    }
    gtk_file_dialog_open(dialog, ui.window, NULL, on_image_chosen, data);
}

static void on_monitor_tile_clicked(GtkButton *button, gpointer data) {
    on_browse_clicked(button, ui.cfg.span ? NULL : data);
}

static void on_monitor_picture_reset(GtkButton *button, gpointer data) {
    config_set_output_image(&ui.cfg, ui.monitors[GPOINTER_TO_INT(data)].connector, NULL);
    refresh_monitor_tiles();
    schedule_save();
}

static void on_monitor_toggled(GObject *toggle, GParamSpec *pspec, gpointer data) {
    if (ui.updating) return;
    int toggled = GPOINTER_TO_INT(data);
    gboolean shown[MAX_OUTPUTS_FILTER], all = TRUE, any = FALSE;
    for (int i = 0; i < ui.n_monitors; i++) {
        shown[i] = i == toggled ? gtk_switch_get_active(GTK_SWITCH(toggle)) : monitor_shown(ui.monitors[i].connector);
        all &= shown[i];
        any |= shown[i];
    }
    // An empty list means "every monitor", so the last one can't be switched off here.
    if (!any) {
        ui.updating = TRUE;
        gtk_switch_set_active(GTK_SWITCH(toggle), TRUE);
        ui.updating = FALSE;
        toast("To hide it everywhere, switch off Animated wallpaper");
        return;
    }
    ui.cfg.n_outputs = 0;
    for (int i = 0; i < ui.n_monitors && !all; i++)
        if (shown[i]) snprintf(ui.cfg.outputs[ui.cfg.n_outputs++], sizeof ui.cfg.outputs[0], "%s", ui.monitors[i].connector);
    refresh_monitor_tiles();
    schedule_save();
}

static void on_preset_selected(GObject *row, GParamSpec *pspec, gpointer data) {
    guint index = adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    if (ui.updating || index == 0) return;

    struct config fresh, *current = &ui.cfg;
    config_defaults(&fresh);
    config_apply_preset(&fresh, config_preset_name(index - 1));
    memcpy(fresh.image, current->image, sizeof fresh.image);
    memcpy(fresh.outputs, current->outputs, sizeof fresh.outputs);
    fresh.n_outputs = current->n_outputs;
    fresh.enabled = current->enabled;
    fresh.renderer = current->renderer;
    fresh.fps = current->fps;
    fresh.render_scale = current->render_scale;
    memcpy(fresh.output_images, current->output_images, sizeof fresh.output_images);
    fresh.n_output_images = current->n_output_images;
    fresh.span = current->span;
    fresh.lock_screen_sync = current->lock_screen_sync;
    fresh.music = current->music;
    fresh.music_style = current->music_style;
    fresh.music_badge = current->music_badge;
    fresh.music_sensitivity = current->music_sensitivity;
    memcpy(fresh.music_source, current->music_source, sizeof fresh.music_source);
    fresh.music_size = current->music_size;
    fresh.music_brightness = current->music_brightness;
    fresh.music_shadow = current->music_shadow;
    fresh.music_bars = current->music_bars;
    fresh.music_colors_auto = current->music_colors_auto;
    memcpy(fresh.music_colors, current->music_colors, sizeof fresh.music_colors);
    ui.cfg = fresh;

    use_detected_colors();
    bindings_refresh();
    schedule_save();
}

static void start_wallpaper(void) {
    g_autofree char *wallpaper = sibling_executable("driftpaper");
    g_autoptr(GError) error = NULL;
    GSubprocess *process = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &error, wallpaper, NULL);
    if (!process) {
        toast("Could not start the wallpaper");
        g_warning("%s", error->message);
        return;
    }
    g_subprocess_wait_async(process, NULL, NULL, NULL);
    g_object_unref(process);
}

static gboolean refresh_status(gpointer data) {
    gboolean running = runtime_wallpaper_running();
    const char *status = running ? "Running" : ui.cfg.enabled ? "Not running" : "Off";
    adw_action_row_set_subtitle(ADW_ACTION_ROW(ui.enabled_row), status);
    gtk_widget_set_visible(ui.start_button, ui.cfg.enabled && !running);
    return G_SOURCE_CONTINUE;
}

// A wallpaper that was just switched off may still be exiting, so check again after it is gone.
static void start_if_still_stopped(gpointer data) {
    if (ui.cfg.enabled && !runtime_wallpaper_running()) start_wallpaper();
    refresh_status(NULL);
}

static void on_start_clicked(GtkButton *button, gpointer data) {
    start_wallpaper();
    g_timeout_add_seconds_once(STATUS_INTERVAL_S, start_if_still_stopped, NULL);
}

static void set_switch_quietly(GtkWidget *row, gboolean active) {
    ui.updating = TRUE;
    adw_switch_row_set_active(ADW_SWITCH_ROW(row), active);
    ui.updating = FALSE;
}

static void on_enabled_toggled(GObject *row, GParamSpec *pspec, gpointer data) {
    if (ui.updating) return;
    gboolean enable = adw_switch_row_get_active(ADW_SWITCH_ROW(row));
    if (enable && !ui.cfg.image[0]) {
        set_switch_quietly(ui.enabled_row, FALSE);
        toast("Choose a picture first");
        return;
    }
    ui.cfg.enabled = enable;
    flush_save();
    write_config();
    if (enable && !runtime_wallpaper_running()) start_wallpaper();
    g_timeout_add_seconds_once(STATUS_INTERVAL_S, start_if_still_stopped, NULL);
}

static char *autostart_path(void) {
    return g_build_filename(g_get_home_dir(), ".config", "autostart", "driftpaper.desktop", NULL);
}

static void set_start_on_login(gboolean enable) {
    g_autofree char *path = autostart_path();
    if (!enable) {
        g_unlink(path);
        return;
    }
    g_autofree char *exec = g_getenv("FLATPAK_ID") ? g_strdup("flatpak run --command=driftpaper " APP_ID)
                                                   : sibling_executable("driftpaper");
    g_autofree char *dir = g_path_get_dirname(path);
    g_autofree char *contents = g_strdup_printf(
        "[Desktop Entry]\nType=Application\nName=driftpaper\nComment=Animated particle wallpaper\n"
        "Exec=%s\nNoDisplay=true\nX-GNOME-Autostart-enabled=true\n", exec);
    g_mkdir_with_parents(dir, 0700);
    if (!g_file_set_contents(path, contents, -1, NULL)) toast("Could not create the login entry");
}

static void on_login_toggled(GObject *row, GParamSpec *pspec, gpointer data) {
    set_start_on_login(adw_switch_row_get_active(ADW_SWITCH_ROW(row)));
}

static void largest_monitor_size(int *width, int *height) {
    *width = 2560;
    *height = 1440;
    GListModel *monitors = gdk_display_get_monitors(gdk_display_get_default());
    long best = 0;
    for (guint i = 0; i < g_list_model_get_n_items(monitors); i++) {
        g_autoptr(GdkMonitor) monitor = g_list_model_get_item(monitors, i);
        GdkRectangle geometry;
        gdk_monitor_get_geometry(monitor, &geometry);
        int scale = gdk_monitor_get_scale_factor(monitor);
        long pixels = (long)geometry.width * scale * geometry.height * scale;
        if (pixels > best) {
            best = pixels;
            *width = geometry.width * scale;
            *height = geometry.height * scale;
        }
    }
}

static void on_snapshot_finished(GObject *source, GAsyncResult *result, gpointer data) {
    g_autofree char *path = data;
    gboolean ok = g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, NULL);
    g_autofree char *message = ok ? g_strdup_printf("Saved %s", path) : g_strdup("Exporting the image failed");
    toast(message);
    g_object_unref(source);
}

static void on_export_target_chosen(GObject *source, GAsyncResult *result, gpointer data) {
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, NULL);
    if (!file) return;
    char *path = g_file_get_path(file);
    if (!path) return;

    flush_save();
    write_config();
    int width, height;
    largest_monitor_size(&width, &height);
    g_autofree char *size = g_strdup_printf("%dx%d", width, height);
    g_autofree char *wallpaper = sibling_executable("driftpaper");
    GSubprocess *process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE, NULL, wallpaper,
                                            "--snapshot", path, "--size", size, NULL);
    if (!process) {
        toast("Exporting the image failed");
        g_free(path);
        return;
    }
    g_subprocess_wait_check_async(process, NULL, on_snapshot_finished, path);
}

static void on_export_clicked(GtkButton *button, gpointer data) {
    g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Export lock screen image");
    gtk_file_dialog_set_initial_name(dialog, "driftpaper-lockscreen.png");
    const char *pictures = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    if (pictures) {
        g_autoptr(GFile) folder = g_file_new_for_path(pictures);
        gtk_file_dialog_set_initial_folder(dialog, folder);
    }
    gtk_file_dialog_save(dialog, ui.window, NULL, on_export_target_chosen, NULL);
}

static AdwPreferencesGroup *add_group(AdwPreferencesPage *page, const char *title, const char *description) {
    AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, title);
    if (description) adw_preferences_group_set_description(group, description);
    adw_preferences_page_add(page, group);
    return group;
}

static GtkWidget *button_row(AdwPreferencesGroup *group, const char *title, const char *subtitle,
                             const char *label, GCallback on_click, gpointer data) {
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle) adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    GtkWidget *button = gtk_button_new_with_label(label);
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    g_signal_connect(button, "clicked", on_click, data);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), button);
    adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), button);
    adw_preferences_group_add(group, row);
    return row;
}

static GtkWidget *switch_row(AdwPreferencesGroup *group, const char *title, gboolean active, GCallback on_toggle) {
    GtkWidget *row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_switch_row_set_active(ADW_SWITCH_ROW(row), active);
    g_signal_connect(row, "notify::active", on_toggle, NULL);
    adw_preferences_group_add(group, row);
    return row;
}

static GtkWidget *flat_button(const char *icon, const char *tooltip, GCallback on_click, gpointer data) {
    GtkWidget *button = gtk_button_new_from_icon_name(icon);
    gtk_widget_set_tooltip_text(button, tooltip);
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(button, "flat");
    g_signal_connect(button, "clicked", on_click, data);
    return button;
}

static GtkWidget *monitor_labels(GdkMonitor *monitor, const char *connector) {
    const char *model = gdk_monitor_get_model(monitor);
    GtkWidget *name = gtk_label_new(model ? model : connector);
    gtk_widget_add_css_class(name, "heading");
    GtkWidget *port = gtk_label_new(connector);
    gtk_widget_add_css_class(port, "dim-label");
    gtk_widget_add_css_class(port, "caption");
    GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(labels, TRUE);
    gtk_widget_set_halign(name, GTK_ALIGN_START);
    gtk_widget_set_halign(port, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(labels), name);
    gtk_box_append(GTK_BOX(labels), port);
    return labels;
}

static GtkWidget *monitor_tile(GdkMonitor *monitor, int index, int tile_height) {
    struct monitor *m = &ui.monitors[index];
    snprintf(m->connector, sizeof m->connector, "%s", gdk_monitor_get_connector(monitor));

    m->picture = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(m->picture), GTK_CONTENT_FIT_COVER);
    gtk_widget_set_size_request(m->picture, -1, tile_height);
    GtkWidget *button = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(button), m->picture);
    gtk_widget_add_css_class(button, "card");
    gtk_widget_set_overflow(button, GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_tooltip_text(button, "Choose a picture for this monitor");
    g_signal_connect(button, "clicked", G_CALLBACK(on_monitor_tile_clicked), GINT_TO_POINTER(index + 1));

    m->reset = flat_button("edit-undo-symbolic", "Use the picture for all monitors",
                           G_CALLBACK(on_monitor_picture_reset), GINT_TO_POINTER(index));
    m->show = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(m->show), monitor_shown(m->connector));
    gtk_widget_set_valign(m->show, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(m->show, "Show the wallpaper on this monitor");
    g_signal_connect(m->show, "notify::active", G_CALLBACK(on_monitor_toggled), GINT_TO_POINTER(index));

    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(footer), monitor_labels(monitor, m->connector));
    gtk_box_append(GTK_BOX(footer), m->reset);
    gtk_box_append(GTK_BOX(footer), m->show);

    m->tile = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_append(GTK_BOX(m->tile), button);
    gtk_box_append(GTK_BOX(m->tile), footer);
    return m->tile;
}

static int compare_monitor_position(const void *a, const void *b) {
    GdkRectangle ga, gb;
    gdk_monitor_get_geometry(*(GdkMonitor *const *)a, &ga);
    gdk_monitor_get_geometry(*(GdkMonitor *const *)b, &gb);
    return ga.x != gb.x ? ga.x - gb.x : ga.y - gb.y;
}

static void add_monitor_tiles(AdwPreferencesGroup *group) {
    GListModel *list = gdk_display_get_monitors(gdk_display_get_default());
    GdkMonitor *monitors[MAX_OUTPUTS_FILTER];
    int count = 0;
    for (guint i = 0; i < g_list_model_get_n_items(list) && count < MAX_OUTPUTS_FILTER; i++) {
        GdkMonitor *monitor = g_list_model_get_item(list, i);
        if (gdk_monitor_get_connector(monitor)) monitors[count++] = monitor;
        else g_object_unref(monitor);
    }
    qsort(monitors, count, sizeof monitors[0], compare_monitor_position);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_set_homogeneous(GTK_BOX(row), TRUE);
    gtk_widget_set_margin_top(row, 12);
    int tile_height = count <= 1 ? 260 : count == 2 ? 140 : 90;
    for (int i = 0; i < count; i++) {
        gtk_box_append(GTK_BOX(row), monitor_tile(monitors[i], i, tile_height));
        g_object_unref(monitors[i]);
    }
    ui.n_monitors = count;
    adw_preferences_group_add(group, row);
}

static GtkWidget *build_wallpaper_page(void) {
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    struct config *c = &ui.cfg;

    AdwPreferencesGroup *wallpaper = add_group(page, "Wallpaper", NULL);
    ui.enabled_row = switch_row(wallpaper, "Animated wallpaper", c->enabled, G_CALLBACK(on_enabled_toggled));
    ui.start_button = gtk_button_new_with_label("Start");
    gtk_widget_set_valign(ui.start_button, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(ui.start_button, FALSE);
    g_signal_connect(ui.start_button, "clicked", G_CALLBACK(on_start_clicked), NULL);
    adw_action_row_add_suffix(ADW_ACTION_ROW(ui.enabled_row), ui.start_button);
    g_autofree char *autostart = autostart_path();
    ui.login_row = switch_row(wallpaper, "Start on login", g_file_test(autostart, G_FILE_TEST_EXISTS),
                              G_CALLBACK(on_login_toggled));

    AdwPreferencesGroup *image = add_group(page, "Image", NULL);
    ui.image_group = GTK_WIDGET(image);
    ui.image_row = button_row(image, "Picture for all monitors", NULL, "Browse…", G_CALLBACK(on_browse_clicked), NULL);
    bind_bool(image, "Stretch across all monitors", "Show the picture as one image over every monitor", &ui.cfg.span);

    const char *preset_labels[32] = {"Custom"};
    for (int i = 0; i < config_preset_count() && i < 30; i++) preset_labels[i + 1] = config_preset_name(i);
    ui.preset_row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(ui.preset_row), "Preset");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(ui.preset_row), "A ready-made look; every setting can still be changed");
    adw_combo_row_set_model(ADW_COMBO_ROW(ui.preset_row), G_LIST_MODEL(gtk_string_list_new(preset_labels)));
    g_signal_connect(ui.preset_row, "notify::selected", G_CALLBACK(on_preset_selected), NULL);
    adw_preferences_group_add(image, ui.preset_row);
    add_monitor_tiles(image);

    AdwPreferencesGroup *lock = add_group(page, "Lock screen", "Most lock screens show a still image.");
    if (lockscreen_supported())
        bind_bool(lock, "Keep the lock screen in sync", "Updates COSMIC's lock and login background to match",
                  &c->lock_screen_sync);
    button_row(lock, "Matching still image", "Export the current look as a PNG", "Export…", G_CALLBACK(on_export_clicked), NULL);

    AdwPreferencesGroup *performance = add_group(page, "Performance", "Renderer changes apply after restarting the wallpaper.");
    static const char *renderers[] = {"Automatic", "Graphics card", "Processor (software)", NULL};
    bind_enum(performance, "Renderer", NULL, (int *)&c->renderer, renderers);
    static const int fps_values[] = {0, 15, 24, 30, 60};
    static const char *fps_labels[] = {"Automatic", "15", "24", "30", "60", NULL};
    bind_int_choice(performance, "Frame rate", "Frames per second", &c->fps, fps_values, fps_labels, 5);
    static const float scale_values[] = {0, 1, 0.75f, 0.5f, 0.35f, 0.25f};
    static const char *scale_labels[] = {"Automatic", "100%", "75%", "50%", "35%", "25%", NULL};
    bind_float_choice(performance, "Resolution", "Lower values use less power", &c->render_scale, scale_values,
                      scale_labels, 6);
    return GTK_WIDGET(page);
}

static GtkWidget *build_particles_page(void) {
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    struct config *c = &ui.cfg;

    AdwPreferencesGroup *particles = add_group(page, "Particles", NULL);
    bind_int(particles, "Amount", NULL, &c->count, 0, 3000, 10);
    GtkWidget *shapes = adw_expander_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(shapes), "Shapes");
    adw_preferences_group_add(particles, shapes);
    static const char *shape_titles[SHAPE_COUNT] = {"Triangle", "Square", "Spark", "Circle", "Ring", "Diamond", "Star", "Plus"};
    for (int i = 0; i < SHAPE_COUNT; i++) bind_shape(shapes, shape_titles[i], &c->shape_weight[i]);
    bind_float(particles, "Smallest size", "Pixels on a 1440p screen", &c->size_min, 1, 100, 1, 0);
    bind_float(particles, "Largest size", NULL, &c->size_max, 1, 100, 1, 0);
    bind_float(particles, "Least visible", NULL, &c->opacity_min, 0, 1, 0.05, 2);
    bind_float(particles, "Most visible", NULL, &c->opacity_max, 0, 1, 0.05, 2);
    bind_float(particles, "Twinkle", "0 is steady, 1 flickers strongly", &c->twinkle, 0, 1, 0.05, 2);

    AdwPreferencesGroup *motion = add_group(page, "Motion", NULL);
    static const char *motions[] = {"Orbit around the subject", "Rise", "Fall", "Drift", NULL};
    bind_enum(motion, "Movement", NULL, (int *)&c->motion, motions);
    bind_float(motion, "Speed", NULL, &c->speed, 0, 10, 0.1, 2);
    GtkWidget *falloff = bind_float(motion, "Outer speed-up", "Outermost particles orbit this many times faster",
                                    &c->speed_falloff, 0.1, 20, 0.5, 1);
    bind_float(motion, "Spin", NULL, &c->spin, 0, 10, 0.1, 2);
    GtkWidget *breathe = bind_float(motion, "Breathing", "Slow in-and-out drift in pixels", &c->breathe, 0, 200, 5, 0);
    static const char *distributions[] = {"Even", "More further out", "More near the subject", NULL};
    GtkWidget *distribution = bind_enum(motion, "Spread", NULL, (int *)&c->distribution, distributions);
    static const char *directions[] = {"Both ways", "Clockwise", "Counter-clockwise", NULL};
    GtkWidget *direction = bind_enum(motion, "Direction", NULL, (int *)&c->direction, directions);
    GtkWidget *orbit_only[] = {falloff, breathe, distribution, direction};
    for (size_t i = 0; i < G_N_ELEMENTS(orbit_only); i++) bind_enable_only_when(orbit_only[i], (int *)&c->motion, MOTION_ORBIT);

    AdwPreferencesGroup *placement = add_group(page, "Placement", "Distances are fractions of the image height.");
    bind_bool(placement, "Detect the subject", "Centre and clear zone from the image", &c->center_auto);
    GtkWidget *cx = bind_float(placement, "Centre across", NULL, &c->center_x, 0, 1, 0.01, 3);
    GtkWidget *cy = bind_float(placement, "Centre down", NULL, &c->center_y, 0, 1, 0.01, 3);
    bind_disable_when(cx, &c->center_auto, 1);
    bind_disable_when(cy, &c->center_auto, 1);
    bind_bool(placement, "Detect the clear zone", NULL, &c->inner_auto);
    GtkWidget *inner = bind_float(placement, "Clear zone", "Empty space around the subject", &c->inner_radius, 0, 2, 0.01, 3);
    bind_disable_when(inner, &c->inner_auto, 1);
    bind_float(placement, "Field size", "How far out particles go", &c->outer_radius, 0.05, 3, 0.01, 2);
    bind_float(placement, "Width stretch", "1 is a circle", &c->ellipse, 0.2, 4, 0.05, 2);

    AdwPreferencesGroup *colors = add_group(page, "Colours", NULL);
    bind_bool(colors, "Colours from the image", NULL, &c->colors_auto);
    GtkWidget *count = bind_int(colors, "Colours used", NULL, &c->n_colors, 1, COLOR_SLOTS, 1);
    GtkWidget *swatches = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(swatches), "Palette");
    for (int i = 0; i < COLOR_SLOTS; i++) adw_action_row_add_suffix(ADW_ACTION_ROW(swatches), bind_color(c->colors[i]));
    adw_preferences_group_add(colors, swatches);
    bind_disable_when(count, &c->colors_auto, 1);
    bind_disable_when(swatches, &c->colors_auto, 1);
    bind_float(colors, "White particles", "Share of particles drawn near-white", &c->white, 0, 1, 0.05, 2);
    return GTK_WIDGET(page);
}

static GtkWidget *build_background_page(void) {
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    struct config *c = &ui.cfg;

    AdwPreferencesGroup *image = add_group(page, "Image", NULL);
    static const char *scales[] = {"Fill the screen", "Fit inside", "Stretch", NULL};
    bind_enum(image, "Scaling", NULL, (int *)&c->scale, scales);
    GtkWidget *letterbox = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(letterbox), "Border colour");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(letterbox), "Shown around the image when it fits inside");
    adw_action_row_add_suffix(ADW_ACTION_ROW(letterbox), bind_color(c->bg_color));
    adw_preferences_group_add(image, letterbox);
    bind_float(image, "Brightness", NULL, &c->brightness, -1, 1, 0.02, 2);
    bind_float(image, "Contrast", NULL, &c->contrast, 0, 3, 0.05, 2);
    bind_float(image, "Saturation", NULL, &c->saturation, 0, 3, 0.05, 2);

    AdwPreferencesGroup *effects = add_group(page, "Effects", NULL);
    bind_float(effects, "Colour fog", "0 turns it off", &c->fog, 0, 3, 0.02, 2);
    bind_float(effects, "Glow", "0 turns it off", &c->glow, 0, 5, 0.1, 2);
    bind_float(effects, "Glow pulse speed", NULL, &c->pulse, 0, 10, 0.1, 2);
    bind_bool(effects, "Glow colour from the image", NULL, &c->glow_auto);
    GtkWidget *glow = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(glow), "Glow colour");
    adw_action_row_add_suffix(ADW_ACTION_ROW(glow), bind_color(c->glow_color));
    adw_preferences_group_add(effects, glow);
    bind_disable_when(glow, &c->glow_auto, 1);

    AdwPreferencesGroup *hue = add_group(page, "Colour range", "Lighten or darken one range of colours in the image.");
    bind_float(hue, "Hue", "Degrees: 0 red, 120 green, 220 blue", &c->hue_center, 0, 360, 5, 0);
    bind_float(hue, "Range", "Degrees around the hue", &c->hue_width, 1, 180, 5, 0);
    bind_float(hue, "Lightness change", "0 is off, -0.2 is 20% darker", &c->hue_lightness, -1, 1, 0.05, 2);
    return GTK_WIDGET(page);
}

static GtkWidget *build_music_page(void) {
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    struct config *c = &ui.cfg;

    AdwPreferencesGroup *music = add_group(page, "Music", "The wallpaper moves to whatever your speakers play.");
    bind_bool(music, "React to music", "Glow and particles pulse on the beat", &c->music);
    static const char *styles[] = {"Equaliser ring", "Liquid halo", "Beat ripples", "Only glow and particles", NULL};
    GtkWidget *style = bind_enum(music, "Visualiser", "Drawn in the middle of the screen", (int *)&c->music_style, styles);
    static const char *badges[] = {"None", "Equaliser bars", "Spectrum dial", "Vinyl record", NULL};
    GtkWidget *badge = bind_enum(music, "Corner badge", "A small round badge in the bottom-right corner", (int *)&c->music_badge,
                                 badges);
    GtkWidget *sensitivity = bind_float(music, "Sensitivity", "Higher values react more strongly", &c->music_sensitivity,
                                        0.1, 5, 0.1, 1);
    GtkWidget *music_only[] = {style, badge, sensitivity};
    for (size_t i = 0; i < G_N_ELEMENTS(music_only); i++) bind_enable_only_when(music_only[i], &c->music, 1);

    AdwPreferencesGroup *look = add_group(page, "Look", "How the visualiser and badge are drawn.");
    GtkWidget *size = bind_float(look, "Size", "Size of the visualiser", &c->music_size, 0.5, 2, 0.05, 2);
    GtkWidget *brightness = bind_float(look, "Brightness", NULL, &c->music_brightness, 0, 3, 0.05, 2);
    GtkWidget *bars = bind_int(look, "Bars", "Number of bars around the ring", &c->music_bars, 24, 144, 4);
    GtkWidget *shadow = bind_float(look, "Shadow", "Keeps the visualiser readable on bright pictures",
                                   &c->music_shadow, 0, 1, 0.05, 2);
    GtkWidget *colors_auto = bind_bool(look, "Colours from the glow", NULL, &c->music_colors_auto);
    GtkWidget *colors = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(colors), "Colours");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(colors), "Bass, then treble");
    for (int i = 0; i < 2; i++) adw_action_row_add_suffix(ADW_ACTION_ROW(colors), bind_color(c->music_colors[i]));
    adw_preferences_group_add(look, colors);
    bind_disable_when(colors, &c->music_colors_auto, 1);
    GtkWidget *look_rows[] = {size, brightness, bars, shadow, colors_auto, colors};
    for (size_t i = 0; i < G_N_ELEMENTS(look_rows); i++) bind_enable_only_when(look_rows[i], &c->music, 1);
    bind_enable_only_when(bars, (int *)&c->music_style, MUSIC_RING);
    bind_disable_when(shadow, (int *)&c->music_style, MUSIC_GLOW);
    return GTK_WIDGET(page);
}

static gboolean on_close_request(GtkWindow *window, gpointer data) {
    flush_save();
    return FALSE;
}

static void on_activate(GtkApplication *app, gpointer data) {
    if (ui.window) {
        gtk_window_present(ui.window);
        return;
    }
    config_load(&ui.cfg, g_file_test(config_default_path(), G_FILE_TEST_EXISTS) ? config_default_path() : NULL, NULL);
    bindings_init(on_setting_changed);

    ui.window = GTK_WINDOW(adw_application_window_new(app));
    gtk_window_set_title(ui.window, "driftpaper");
    gtk_window_set_default_size(ui.window, 640, 820);
    g_signal_connect(ui.window, "close-request", G_CALLBACK(on_close_request), NULL);

    AdwViewStack *stack = ADW_VIEW_STACK(adw_view_stack_new());
    adw_view_stack_add_titled_with_icon(stack, build_wallpaper_page(), "wallpaper", "Wallpaper", "image-x-generic-symbolic");
    adw_view_stack_add_titled_with_icon(stack, build_particles_page(), "particles", "Particles", "starred-symbolic");
    adw_view_stack_add_titled_with_icon(stack, build_background_page(), "background", "Background", "applications-graphics-symbolic");
    adw_view_stack_add_titled_with_icon(stack, build_music_page(), "music", "Music", "audio-x-generic-symbolic");

    GtkWidget *switcher = adw_view_switcher_new();
    adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(switcher), stack);
    adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(switcher), ADW_VIEW_SWITCHER_POLICY_WIDE);
    GtkWidget *header = adw_header_bar_new();
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), switcher);

    ui.toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(ui.toasts, GTK_WIDGET(stack));
    GtkWidget *view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), GTK_WIDGET(ui.toasts));
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(ui.window), view);

    analyze_current_image();
    bindings_refresh();
    refresh_status(NULL);
    g_timeout_add_seconds(STATUS_INTERVAL_S, refresh_status, NULL);
    gtk_window_present(ui.window);
}

int main(int argc, char **argv) {
    runtime_find_wayland_display();
    g_autoptr(AdwApplication) app = adw_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    return g_application_run(G_APPLICATION(app), argc, argv);
}
