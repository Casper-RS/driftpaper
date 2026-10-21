#include "bindings.h"
#include <math.h>
#include <stdlib.h>

enum kind { KIND_FLOAT, KIND_INT, KIND_BOOL, KIND_ENUM, KIND_INT_CHOICE, KIND_FLOAT_CHOICE, KIND_SHAPE, KIND_COLOR };

struct binding {
    enum kind kind;
    GtkWidget *widget;
    void *field;
    const int *int_values;
    const float *float_values;
    int count;
};

struct sensitivity {
    GtkWidget *widget;
    const int *flag;
    int value;
    gboolean enable_on_match;
};

static GPtrArray *bindings, *sensitivities;
static void (*changed_callback)(void);
static gboolean refreshing;

void bindings_init(void (*on_change)(void)) {
    bindings = g_ptr_array_new_with_free_func(g_free);
    sensitivities = g_ptr_array_new_with_free_func(g_free);
    changed_callback = on_change;
}

static gboolean condition_holds(const struct sensitivity *s) {
    gboolean matches = *s->flag == s->value;
    return s->enable_on_match ? matches : !matches;
}

// A widget with several conditions is enabled only while all of them hold.
static void update_sensitivity(void) {
    for (guint i = 0; i < sensitivities->len; i++) {
        struct sensitivity *s = g_ptr_array_index(sensitivities, i);
        gboolean enabled = TRUE;
        for (guint j = 0; j < sensitivities->len && enabled; j++) {
            const struct sensitivity *other = g_ptr_array_index(sensitivities, j);
            if (other->widget == s->widget) enabled = condition_holds(other);
        }
        gtk_widget_set_sensitive(s->widget, enabled);
    }
}

static void add_sensitivity(GtkWidget *widget, const int *flag, int value, gboolean enable_on_match) {
    struct sensitivity *s = g_new(struct sensitivity, 1);
    *s = (struct sensitivity){widget, flag, value, enable_on_match};
    g_ptr_array_add(sensitivities, s);
    update_sensitivity();
}

void bind_disable_when(GtkWidget *widget, const int *flag, int value) {
    add_sensitivity(widget, flag, value, FALSE);
}

void bind_enable_only_when(GtkWidget *widget, const int *flag, int value) {
    add_sensitivity(widget, flag, value, TRUE);
}

static int nearest_int_choice(const struct binding *b) {
    int value = *(int *)b->field, best = 0;
    for (int i = 1; i < b->count; i++)
        if (abs(b->int_values[i] - value) < abs(b->int_values[best] - value)) best = i;
    return best;
}

static int nearest_float_choice(const struct binding *b) {
    float value = *(float *)b->field;
    int best = 0;
    for (int i = 1; i < b->count; i++)
        if (fabsf(b->float_values[i] - value) < fabsf(b->float_values[best] - value)) best = i;
    return best;
}

static void show_value(const struct binding *b) {
    switch (b->kind) {
    case KIND_FLOAT: adw_spin_row_set_value(ADW_SPIN_ROW(b->widget), *(float *)b->field); break;
    case KIND_INT: adw_spin_row_set_value(ADW_SPIN_ROW(b->widget), *(int *)b->field); break;
    case KIND_BOOL: adw_switch_row_set_active(ADW_SWITCH_ROW(b->widget), *(int *)b->field); break;
    case KIND_ENUM: adw_combo_row_set_selected(ADW_COMBO_ROW(b->widget), *(int *)b->field); break;
    case KIND_INT_CHOICE: adw_combo_row_set_selected(ADW_COMBO_ROW(b->widget), nearest_int_choice(b)); break;
    case KIND_FLOAT_CHOICE: adw_combo_row_set_selected(ADW_COMBO_ROW(b->widget), nearest_float_choice(b)); break;
    case KIND_SHAPE: adw_switch_row_set_active(ADW_SWITCH_ROW(b->widget), *(float *)b->field > 0); break;
    case KIND_COLOR: {
        const float *rgb = b->field;
        GdkRGBA color = {rgb[0], rgb[1], rgb[2], 1};
        gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(b->widget), &color);
        break;
    }
    }
}

void bindings_refresh(void) {
    refreshing = TRUE;
    for (guint i = 0; i < bindings->len; i++) show_value(g_ptr_array_index(bindings, i));
    update_sensitivity();
    refreshing = FALSE;
}

static void store_value(const struct binding *b) {
    switch (b->kind) {
    case KIND_FLOAT: *(float *)b->field = (float)adw_spin_row_get_value(ADW_SPIN_ROW(b->widget)); break;
    case KIND_INT: *(int *)b->field = (int)adw_spin_row_get_value(ADW_SPIN_ROW(b->widget)); break;
    case KIND_BOOL: *(int *)b->field = adw_switch_row_get_active(ADW_SWITCH_ROW(b->widget)); break;
    case KIND_ENUM: *(int *)b->field = (int)adw_combo_row_get_selected(ADW_COMBO_ROW(b->widget)); break;
    case KIND_INT_CHOICE: *(int *)b->field = b->int_values[adw_combo_row_get_selected(ADW_COMBO_ROW(b->widget))]; break;
    case KIND_FLOAT_CHOICE: *(float *)b->field = b->float_values[adw_combo_row_get_selected(ADW_COMBO_ROW(b->widget))]; break;
    case KIND_SHAPE: {
        float *weight = b->field;
        int active = adw_switch_row_get_active(ADW_SWITCH_ROW(b->widget));
        if (!active) *weight = 0;
        else if (*weight <= 0) *weight = 1;
        break;
    }
    case KIND_COLOR: {
        const GdkRGBA *color = gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(b->widget));
        float *rgb = b->field;
        rgb[0] = color->red;
        rgb[1] = color->green;
        rgb[2] = color->blue;
        break;
    }
    }
}

static void on_widget_changed(GObject *widget, GParamSpec *pspec, gpointer data) {
    if (refreshing) return;
    store_value(data);
    update_sensitivity();
    changed_callback();
}

static void add_binding(struct binding binding, const char *property) {
    struct binding *b = g_new(struct binding, 1);
    *b = binding;
    g_ptr_array_add(bindings, b);
    show_value(b);
    g_signal_connect(b->widget, property, G_CALLBACK(on_widget_changed), b);
}

static void set_titles(GtkWidget *row, const char *title, const char *subtitle) {
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle) adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
}

static GtkWidget *spin_row(AdwPreferencesGroup *group, const char *title, const char *subtitle,
                           double min, double max, double step, int digits) {
    GtkWidget *row = adw_spin_row_new_with_range(min, max, step);
    adw_spin_row_set_digits(ADW_SPIN_ROW(row), digits);
    set_titles(row, title, subtitle);
    adw_preferences_group_add(group, row);
    return row;
}

GtkWidget *bind_float(AdwPreferencesGroup *group, const char *title, const char *subtitle, float *field,
                      double min, double max, double step, int digits) {
    GtkWidget *row = spin_row(group, title, subtitle, min, max, step, digits);
    add_binding((struct binding){.kind = KIND_FLOAT, .widget = row, .field = field}, "notify::value");
    return row;
}

GtkWidget *bind_int(AdwPreferencesGroup *group, const char *title, const char *subtitle, int *field,
                    int min, int max, int step) {
    GtkWidget *row = spin_row(group, title, subtitle, min, max, step, 0);
    add_binding((struct binding){.kind = KIND_INT, .widget = row, .field = field}, "notify::value");
    return row;
}

GtkWidget *bind_bool(AdwPreferencesGroup *group, const char *title, const char *subtitle, int *field) {
    GtkWidget *row = adw_switch_row_new();
    set_titles(row, title, subtitle);
    adw_preferences_group_add(group, row);
    add_binding((struct binding){.kind = KIND_BOOL, .widget = row, .field = field}, "notify::active");
    return row;
}

static GtkWidget *combo_row(AdwPreferencesGroup *group, const char *title, const char *subtitle,
                            const char *const *labels) {
    GtkWidget *row = adw_combo_row_new();
    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(gtk_string_list_new(labels)));
    set_titles(row, title, subtitle);
    adw_preferences_group_add(group, row);
    return row;
}

GtkWidget *bind_enum(AdwPreferencesGroup *group, const char *title, const char *subtitle, int *field,
                     const char *const *labels) {
    GtkWidget *row = combo_row(group, title, subtitle, labels);
    add_binding((struct binding){.kind = KIND_ENUM, .widget = row, .field = field}, "notify::selected");
    return row;
}

GtkWidget *bind_int_choice(AdwPreferencesGroup *group, const char *title, const char *subtitle, int *field,
                           const int *values, const char *const *labels, int count) {
    GtkWidget *row = combo_row(group, title, subtitle, labels);
    add_binding((struct binding){.kind = KIND_INT_CHOICE, .widget = row, .field = field, .int_values = values, .count = count},
                "notify::selected");
    return row;
}

GtkWidget *bind_float_choice(AdwPreferencesGroup *group, const char *title, const char *subtitle, float *field,
                             const float *values, const char *const *labels, int count) {
    GtkWidget *row = combo_row(group, title, subtitle, labels);
    add_binding((struct binding){.kind = KIND_FLOAT_CHOICE, .widget = row, .field = field, .float_values = values, .count = count},
                "notify::selected");
    return row;
}

GtkWidget *bind_shape(GtkWidget *expander, const char *title, float *weight) {
    GtkWidget *row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_expander_row_add_row(ADW_EXPANDER_ROW(expander), row);
    add_binding((struct binding){.kind = KIND_SHAPE, .widget = row, .field = weight}, "notify::active");
    return row;
}

GtkWidget *bind_color(float *rgb) {
    GtkWidget *button = gtk_color_dialog_button_new(gtk_color_dialog_new());
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    add_binding((struct binding){.kind = KIND_COLOR, .widget = button, .field = rgb}, "notify::rgba");
    return button;
}
