#pragma once
#include <adwaita.h>

// Rows that edit a config field in place. Every user edit calls the change callback;
// bindings_refresh() pushes field values back into the widgets without triggering it.

void bindings_init(void (*on_change)(void));
void bindings_refresh(void);

GtkWidget *bind_float(AdwPreferencesGroup *group, const char *title, const char *subtitle, float *field,
                      double min, double max, double step, int digits);
GtkWidget *bind_int(AdwPreferencesGroup *group, const char *title, const char *subtitle, int *field,
                    int min, int max, int step);
GtkWidget *bind_bool(AdwPreferencesGroup *group, const char *title, const char *subtitle, int *field);
GtkWidget *bind_enum(AdwPreferencesGroup *group, const char *title, const char *subtitle, int *field,
                     const char *const *labels);
GtkWidget *bind_int_choice(AdwPreferencesGroup *group, const char *title, const char *subtitle, int *field,
                           const int *values, const char *const *labels, int count);
GtkWidget *bind_float_choice(AdwPreferencesGroup *group, const char *title, const char *subtitle, float *field,
                             const float *values, const char *const *labels, int count);
GtkWidget *bind_shape(GtkWidget *expander, const char *title, float *weight);
GtkWidget *bind_color(float *rgb);

// Makes `widget` insensitive while *flag equals `value`, or, for the second form, unless it does.
void bind_disable_when(GtkWidget *widget, const int *flag, int value);
void bind_enable_only_when(GtkWidget *widget, const int *flag, int value);
