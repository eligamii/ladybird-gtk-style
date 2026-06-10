/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Gtk/Events.h>
#include <UI/Gtk/WebContentView.h>

#include <adwaita.h>
#include <fmt/base.h>
#include <gdk/gdk.h>

// Scroll constants and types

static constexpr double ANIMATION_DURATION_IN_MSEC = 2000;
// To mimic the exact scroll speed of GtkScrolledWindow
static constexpr double SCROLL_SPEED_MULTIPLIER = 2.3;

struct KineticScrollState {
    double x_velocity;
    double y_velocity;
    double last_tick_fraction;
    Ladybird::GObjectPtr<AdwAnimation> animation;

    void end_kinetic_scroll() const
    {
        adw_animation_pause(animation.ptr());
    }
};

#define LADYBIRD_WEB_VIEW(obj) (reinterpret_cast<LadybirdWebView*>(obj))
#define LADYBIRD_TYPE_WEB_VIEW (ladybird_web_view_get_type())

struct LadybirdWebView {
    GtkWidget parent_instance;
    Ladybird::WebContentView* impl { nullptr };
    double last_mouse_x { 0 };
    double last_mouse_y { 0 };

    Optional<KineticScrollState> kinetic_scroll_state;

    double last_scale_delta { 1 };
};

struct LadybirdWebViewClass {
    GtkWidgetClass parent_class;
};

G_DEFINE_FINAL_TYPE(LadybirdWebView, ladybird_web_view, GTK_TYPE_WIDGET)

// GObject vfunc implementations

static void ladybird_web_view_finalize(GObject* object)
{
    auto* self = LADYBIRD_WEB_VIEW(object);
    // Don't delete impl - it's owned by Tab's OwnPtr<WebContentView>.
    // Just tell the C++ side the widget is gone.
    if (self->impl)
        self->impl->set_widget(nullptr);
    self->impl = nullptr;
    G_OBJECT_CLASS(ladybird_web_view_parent_class)->finalize(object);
}

static void ladybird_web_view_snapshot(GtkWidget* widget, GtkSnapshot* snapshot)
{
    auto* self = LADYBIRD_WEB_VIEW(widget);
    if (self->impl)
        self->impl->paint(snapshot);
}

static void ladybird_web_view_measure(GtkWidget*, GtkOrientation orientation, int, int* minimum, int* natural, int* minimum_baseline, int* natural_baseline)
{
    if (orientation == GTK_ORIENTATION_HORIZONTAL) {
        *minimum = 100;
        *natural = 800;
    } else {
        *minimum = 100;
        *natural = 600;
    }
    *minimum_baseline = -1;
    *natural_baseline = -1;
}

static void ladybird_web_view_size_allocate(GtkWidget* widget, int width, int height, int)
{
    auto* self = LADYBIRD_WEB_VIEW(widget);
    if (self->impl)
        self->impl->update_viewport_size(width, height);
}

static void ladybird_web_view_class_init(LadybirdWebViewClass* klass)
{
    auto* widget_class = GTK_WIDGET_CLASS(klass);
    widget_class->snapshot = ladybird_web_view_snapshot;
    widget_class->measure = ladybird_web_view_measure;
    widget_class->size_allocate = ladybird_web_view_size_allocate;

    auto* object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = ladybird_web_view_finalize;

    gtk_widget_class_set_css_name(widget_class, "ladybird-web-view");
}

// Input event callbacks

static gboolean on_key_pressed(GtkEventControllerKey*, guint keyval, guint, GdkModifierType state, gpointer user_data)
{
    auto* self = LADYBIRD_WEB_VIEW(user_data);
    if (!self->impl)
        return GDK_EVENT_PROPAGATE;

    if (self->impl->is_node_picker_active()) {
        if (keyval == GDK_KEY_Escape)
            self->impl->node_picker_cancel();
        return GDK_EVENT_STOP;
    }

    self->impl->enqueue_native_event(Web::KeyEvent::Type::KeyDown, keyval, state);
    return GDK_EVENT_STOP;
}

static void on_key_released(GtkEventControllerKey*, guint keyval, guint, GdkModifierType state, gpointer user_data)
{
    auto* self = LADYBIRD_WEB_VIEW(user_data);
    if (!self->impl)
        return;

    if (self->impl->is_node_picker_active())
        return;

    self->impl->enqueue_native_event(Web::KeyEvent::Type::KeyUp, keyval, state);
}

static void on_mouse_pressed(GtkGestureClick* gesture, gint n_press, gdouble x, gdouble y, gpointer user_data)
{
    auto* self = LADYBIRD_WEB_VIEW(user_data);
    if (!self->impl)
        return;

    gtk_widget_grab_focus(GTK_WIDGET(self));

    auto button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    auto state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
    if (self->impl->is_node_picker_active()) {
        if (button == GDK_BUTTON_PRIMARY) {
            auto position = Web::DevicePixelPoint { static_cast<int>(x * self->impl->device_pixel_ratio()), static_cast<int>(y * self->impl->device_pixel_ratio()) };
            if (state & GDK_CONTROL_MASK)
                self->impl->node_picker_preview(position);
            else
                self->impl->node_picker_pick(position);
        }
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
        return;
    }

    self->impl->enqueue_native_event(Web::MouseEvent::Type::MouseDown, x, y, button, state, n_press);
}

static void on_mouse_released(GtkGestureClick* gesture, gint n_press, gdouble x, gdouble y, gpointer user_data)
{
    auto* self = LADYBIRD_WEB_VIEW(user_data);
    if (!self->impl)
        return;

    if (self->impl->is_node_picker_active()) {
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
        return;
    }

    auto button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    auto state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
    self->impl->enqueue_native_event(Web::MouseEvent::Type::MouseUp, x, y, button, state, n_press);
}

static void on_mouse_motion(GtkEventControllerMotion* controller, gdouble x, gdouble y, gpointer user_data)
{
    auto* self = LADYBIRD_WEB_VIEW(user_data);
    self->last_mouse_x = x;
    self->last_mouse_y = y;
    if (!self->impl)
        return;

    if (self->impl->is_node_picker_active()) {
        auto position = Web::DevicePixelPoint { static_cast<int>(x * self->impl->device_pixel_ratio()), static_cast<int>(y * self->impl->device_pixel_ratio()) };
        self->impl->node_picker_hover(position);
        return;
    }

    auto state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));
    self->impl->enqueue_native_event(Web::MouseEvent::Type::MouseMove, x, y, 0, state, 0);
}

static void on_mouse_leave(GtkEventControllerMotion*, gpointer user_data)
{
    auto* self = LADYBIRD_WEB_VIEW(user_data);
    if (!self->impl)
        return;

    if (self->impl->is_node_picker_active()) {
        self->impl->clear_node_picker();
        return;
    }

    self->impl->enqueue_native_event(Web::MouseEvent::Type::MouseLeave, 0, 0, 0, static_cast<GdkModifierType>(0), 0);
}

static void kinetic_scroll_callback(double time, gpointer user_data)
{
    static constexpr double friction = 4;

    auto* self = LADYBIRD_WEB_VIEW(user_data);
    if (!self->kinetic_scroll_state.has_value())
        return;

    auto& kinetic_scroll_state = self->kinetic_scroll_state.value();

    double dt = time - kinetic_scroll_state.last_tick_fraction;
    kinetic_scroll_state.last_tick_fraction = time;

    double dx = kinetic_scroll_state.x_velocity * dt;
    double dy = kinetic_scroll_state.y_velocity * dt;

    kinetic_scroll_state.x_velocity *= exp(-friction * dt);
    kinetic_scroll_state.y_velocity *= exp(-friction * dt);

    auto device_pixel_ratio = self->impl->device_pixel_ratio();

    double wheel_delta_x = dx * device_pixel_ratio;
    double wheel_delta_y = dy * device_pixel_ratio;

    auto position = Web::DevicePixelPoint { static_cast<int>(self->last_mouse_x * device_pixel_ratio), static_cast<int>(self->last_mouse_y * device_pixel_ratio) };

    Web::MouseEvent event {
        .type = Web::MouseEvent::Type::MouseWheel,
        .position = position,
        // FIXME: This should be absolute screen coordinates, but Wayland does not expose window positions to applications.
        .screen_position = position,
        .button = Web::UIEvents::MouseButton::None,
        .buttons = Web::UIEvents::MouseButton::None,
        .modifiers = Web::UIEvents::Mod_None,
        .wheel_delta_x = wheel_delta_x,
        .wheel_delta_y = wheel_delta_y,
        .click_count = 0,
        .browser_data = { },
    };
    self->impl->enqueue_input_event(move(event));
}

static void on_decelerate(GtkEventControllerScroll*, gdouble vel_x, gdouble vel_y, gpointer user_data)
{
    if (vel_x == 0 && vel_y == 0)
        return;

    auto* self = LADYBIRD_WEB_VIEW(user_data);

    AdwAnimationTarget* callback_animation = adw_callback_animation_target_new(
        kinetic_scroll_callback,
        user_data,
        nullptr);

    AdwAnimation* animation = adw_timed_animation_new(
        GTK_WIDGET(self),
        0,
        ANIMATION_DURATION_IN_MSEC / 1000,
        ANIMATION_DURATION_IN_MSEC,
        callback_animation);

    adw_timed_animation_set_easing(ADW_TIMED_ANIMATION(animation), ADW_LINEAR);

    if (self->kinetic_scroll_state.has_value())
        self->kinetic_scroll_state->end_kinetic_scroll();

    adw_animation_play(animation);

    self->kinetic_scroll_state = KineticScrollState {
        .x_velocity = vel_x * SCROLL_SPEED_MULTIPLIER,
        .y_velocity = vel_y * SCROLL_SPEED_MULTIPLIER,
        .last_tick_fraction = 0,
        .animation = Ladybird::GObjectPtr { animation },
    };
}

static gboolean on_scroll(GtkEventControllerScroll* controller, gdouble dx, gdouble dy, gpointer user_data)
{
    auto* self = LADYBIRD_WEB_VIEW(user_data);
    if (!self->impl)
        return GDK_EVENT_PROPAGATE;

    if (self->impl->is_node_picker_active())
        return GDK_EVENT_STOP;

    if (self->kinetic_scroll_state.has_value())
        self->kinetic_scroll_state->end_kinetic_scroll();

    auto state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));

    // Ctrl+scroll = zoom
    if (state & GDK_CONTROL_MASK) {
        if (dy < 0)
            self->impl->zoom_in();
        else if (dy > 0)
            self->impl->zoom_out();
        return GDK_EVENT_STOP;
    }

    auto device_pixel_ratio = self->impl->device_pixel_ratio();

    double wheel_delta_x = 0;
    double wheel_delta_y = 0;

    auto* gdk_event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
    auto unit = gdk_scroll_event_get_unit(gdk_event);

    if (unit == GDK_SCROLL_UNIT_SURFACE) {
        wheel_delta_x = dx * device_pixel_ratio * SCROLL_SPEED_MULTIPLIER;
        wheel_delta_y = dy * device_pixel_ratio * SCROLL_SPEED_MULTIPLIER;
    } else {
        static constexpr double scroll_lines = 3.0;
        static constexpr double scroll_step_size = 40.0;
        wheel_delta_x = dx * scroll_lines * scroll_step_size * device_pixel_ratio;
        wheel_delta_y = dy * scroll_lines * scroll_step_size * device_pixel_ratio;
    }

    // GDK scroll events on Wayland do not carry reliable pointer coordinates, so we
    // use the last position recorded by the motion controller instead.
    auto position = Web::DevicePixelPoint { static_cast<int>(self->last_mouse_x * device_pixel_ratio), static_cast<int>(self->last_mouse_y * device_pixel_ratio) };

    Web::MouseEvent event {
        .type = Web::MouseEvent::Type::MouseWheel,
        .position = position,
        // FIXME: This should be absolute screen coordinates, but Wayland does not expose window positions to applications.
        .screen_position = position,
        .button = Web::UIEvents::MouseButton::None,
        .buttons = Web::UIEvents::MouseButton::None,
        .modifiers = Ladybird::gdk_modifier_to_web(state),
        .wheel_delta_x = wheel_delta_x,
        .wheel_delta_y = wheel_delta_y,
        .click_count = 0,
        .browser_data = {},
    };
    self->impl->enqueue_input_event(move(event));
    return GDK_EVENT_STOP;
}

static void on_pinch_to_zoom(GtkGestureZoom* zoom_gesture, gdouble scale, gpointer user_data)
{
    auto* self = LADYBIRD_WEB_VIEW(user_data);

    auto state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(zoom_gesture));

    Web::PinchEvent pinch_event;
    pinch_event.modifiers = Ladybird::gdk_modifier_to_web(state);
    pinch_event.scale_delta = scale - self->last_scale_delta;
    pinch_event.position = { self->last_mouse_x, self->last_mouse_y };
    self->impl->enqueue_input_event(pinch_event);

    self->last_scale_delta = scale;
}

static void on_map(GtkWidget* widget, gpointer)
{
    auto* self = LADYBIRD_WEB_VIEW(widget);
    if (!self->impl)
        return;
    self->impl->set_system_visibility_state(Web::HTML::VisibilityState::Visible);
    self->impl->update_viewport_size();
    gtk_widget_queue_draw(widget);
}

static void on_unmap(GtkWidget* widget, gpointer)
{
    auto* self = LADYBIRD_WEB_VIEW(widget);
    if (!self->impl)
        return;
    self->impl->set_system_visibility_state(Web::HTML::VisibilityState::Hidden);
}

static void ladybird_web_view_init(LadybirdWebView* self)
{
    self->impl = nullptr;

    gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);

    g_signal_connect(GTK_WIDGET(self), "map", G_CALLBACK(on_map), nullptr);
    g_signal_connect(GTK_WIDGET(self), "unmap", G_CALLBACK(on_unmap), nullptr);

    auto* focus_controller = gtk_event_controller_focus_new();
    g_signal_connect_swapped(focus_controller, "enter", G_CALLBACK(+[](LadybirdWebView* self, GtkEventControllerFocus*) {
        if (self->impl)
            self->impl->set_has_focus(true);
    }),
        self);
    g_signal_connect_swapped(focus_controller, "leave", G_CALLBACK(+[](LadybirdWebView* self, GtkEventControllerFocus*) {
        if (self->impl)
            self->impl->set_has_focus(false);
    }),
        self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(focus_controller));

    auto* key_controller = gtk_event_controller_key_new();
    g_signal_connect(key_controller, "key-pressed", G_CALLBACK(on_key_pressed), self);
    g_signal_connect(key_controller, "key-released", G_CALLBACK(on_key_released), self);
    gtk_widget_add_controller(GTK_WIDGET(self), key_controller);

    auto* click_gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click_gesture), 0);
    g_signal_connect(click_gesture, "pressed", G_CALLBACK(on_mouse_pressed), self);
    g_signal_connect(click_gesture, "released", G_CALLBACK(on_mouse_released), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(click_gesture));

    auto* motion_controller = gtk_event_controller_motion_new();
    g_signal_connect(motion_controller, "motion", G_CALLBACK(on_mouse_motion), self);
    g_signal_connect(motion_controller, "leave", G_CALLBACK(on_mouse_leave), self);
    gtk_widget_add_controller(GTK_WIDGET(self), motion_controller);

    auto* scroll_controller = gtk_event_controller_scroll_new(static_cast<GtkEventControllerScrollFlags>(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES | GTK_EVENT_CONTROLLER_SCROLL_KINETIC));
    g_signal_connect(scroll_controller, "scroll", G_CALLBACK(on_scroll), self);
    g_signal_connect(scroll_controller, "decelerate", G_CALLBACK(on_decelerate), self);
    gtk_widget_add_controller(GTK_WIDGET(self), scroll_controller);

    auto* pinch_to_zoom_gesture = gtk_gesture_zoom_new();
    g_signal_connect(pinch_to_zoom_gesture, "scale-changed", G_CALLBACK(on_pinch_to_zoom), self);
    g_signal_connect_swapped(pinch_to_zoom_gesture, "begin", G_CALLBACK(+[](LadybirdWebView* self) { self->last_scale_delta = 1; }), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(pinch_to_zoom_gesture));
}

// Public API

LadybirdWebView* ladybird_web_view_new()
{
    return LADYBIRD_WEB_VIEW(g_object_new(LADYBIRD_TYPE_WEB_VIEW, nullptr));
}

Ladybird::WebContentView* ladybird_web_view_get_impl(LadybirdWebView* self)
{
    return self->impl;
}

void ladybird_web_view_set_impl(LadybirdWebView* self, Ladybird::WebContentView* impl)
{
    self->impl = impl;
}
