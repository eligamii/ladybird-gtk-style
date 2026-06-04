/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWebView/Application.h>
#include <LibWebView/Autocomplete.h>
#include <LibWebView/URL.h>
#include <UI/Gtk/GLibPtr.h>
#include <UI/Gtk/Widgets/Builder.h>
#include <UI/Gtk/Widgets/LadybirdLocationEntry.h>

struct LocationEntryState {
    NonnullOwnPtr<WebView::Autocomplete> autocomplete;
    Vector<WebView::AutocompleteSuggestion> suggestions;
    Ladybird::GObjectPtr<GdkPaintable> favicon;
    int selected_index { -1 };
    String user_text;
    bool is_focused { false };
    bool is_loading { false };
    bool updating_text { false };
    guint loading_pulse_source_id { 0 };
    Function<void(String)> on_navigate;
};

#define LADYBIRD_LOCATION_ENTRY(obj) (reinterpret_cast<LadybirdLocationEntry*>(obj))
#define LADYBIRD_TYPE_LOCATION_ENTRY (ladybird_location_entry_get_type())

struct LadybirdLocationEntry {
    GtkEntry parent_instance;

    GtkPopover* popover { nullptr };
    GtkListBox* list_box { nullptr };
    // GObject allocates this struct with g_malloc0, which zero-fills without
    // calling C++ constructors. OwnPtr is safe here because zero-initialized
    // OwnPtr is equivalent to nullptr (empty state).
    OwnPtr<LocationEntryState> state;
};

struct LadybirdLocationEntryClass {
    GtkEntryClass parent_class;
};

G_DEFINE_FINAL_TYPE(LadybirdLocationEntry, ladybird_location_entry, GTK_TYPE_ENTRY)

static void ladybird_location_entry_update_display_attributes(LadybirdLocationEntry* self);
static void ladybird_location_entry_show_completions(LadybirdLocationEntry* self);
static void ladybird_location_entry_hide_completions(LadybirdLocationEntry* self);
static void ladybird_location_entry_navigate(LadybirdLocationEntry* self);
static void ladybird_location_entry_move_selection(LadybirdLocationEntry* self, int delta);
static void ladybird_location_entry_apply_selected_suggestion(LadybirdLocationEntry* self);
static void ladybird_location_entry_update_leading_icon(LadybirdLocationEntry* self);

static void ladybird_location_entry_completion_popover_header_func(GtkListBoxRow* row, GtkListBoxRow* before, gpointer)
{
    char const* row_title = static_cast<char const*>(g_object_get_data(G_OBJECT(row), "section-name"));
    char const* before_title = nullptr;

    if (before) {
        before_title = static_cast<char const*>(g_object_get_data(G_OBJECT(before), "section-name"));
    }

    if (row_title != before_title && row_title) {
        GtkWidget* label = gtk_label_new(row_title);

        if (before)
            gtk_widget_add_css_class(label, "first");

        gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0);
        gtk_widget_add_css_class(label, "ladybird-completion-popover-header");
        gtk_widget_add_css_class(label, "dimmed");
        gtk_list_box_row_set_header(row, label);
    }
}

static void set_entry_text_suppressed(LadybirdLocationEntry* self, char const* text, bool move_cursor_to_end = false)
{
    self->state->updating_text = true;
    gtk_editable_set_text(GTK_EDITABLE(self), text);
    if (move_cursor_to_end)
        gtk_editable_set_position(GTK_EDITABLE(self), -1);
    self->state->updating_text = false;
}

static void ladybird_location_entry_finalize(GObject* object)
{
    auto* self = LADYBIRD_LOCATION_ENTRY(object);
    if (self->state && self->state->loading_pulse_source_id != 0) {
        g_source_remove(self->state->loading_pulse_source_id);
        self->state->loading_pulse_source_id = 0;
    }
    if (self->popover) {
        gtk_popover_popdown(self->popover);
        gtk_widget_unparent(GTK_WIDGET(self->popover));
        self->popover = nullptr;
    }
    self->state.clear();
    G_OBJECT_CLASS(ladybird_location_entry_parent_class)->finalize(object);
}

static void ladybird_location_entry_size_allocate(GtkWidget* widget, int width, int height, int baseline)
{
    GTK_WIDGET_CLASS(ladybird_location_entry_parent_class)->size_allocate(widget, width, height, baseline);
    gtk_popover_present(LADYBIRD_LOCATION_ENTRY(widget)->popover);
}

static void ladybird_location_entry_class_init(LadybirdLocationEntryClass* klass)
{
    G_OBJECT_CLASS(klass)->finalize = ladybird_location_entry_finalize;
    GTK_WIDGET_CLASS(klass)->size_allocate = ladybird_location_entry_size_allocate;
}

static void ladybird_location_entry_init(LadybirdLocationEntry* self)
{
    self->state = adopt_own(*new LocationEntryState {
        .autocomplete = make<WebView::Autocomplete>(),
        .suggestions = {},
        .favicon = {},
        .selected_index = -1,
        .user_text = {},
        .is_focused = false,
        .is_loading = false,
        .updating_text = false,
        .loading_pulse_source_id = 0,
        .on_navigate = {},
    });

    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);

    if (auto const& search_engine = WebView::Application::settings().search_engine(); search_engine.has_value()) {
        auto placeholder = ByteString::formatted("Search with {} or enter URL", search_engine->name);
        gtk_entry_set_placeholder_text(GTK_ENTRY(self), placeholder.characters());
    } else {
        gtk_entry_set_placeholder_text(GTK_ENTRY(self), "Enter URL or search...");
    }

    // Load completion popover from resource
    Ladybird::GObjectPtr builder { gtk_builder_new_from_resource("/org/ladybird/Ladybird/gtk/location-entry.ui") };
    self->popover = LadybirdWidgets::get_builder_object<GtkPopover>(builder, "completion_popover");
    self->list_box = LadybirdWidgets::get_builder_object<GtkListBox>(builder, "completion_list_box");
    gtk_widget_set_parent(GTK_WIDGET(self->popover), GTK_WIDGET(self));

    // Add headers to suggestions sections
    gtk_list_box_set_header_func(self->list_box, ladybird_location_entry_completion_popover_header_func, nullptr, nullptr);

    // Clicking a suggestion navigates to it
    g_signal_connect_swapped(self->list_box, "row-activated", G_CALLBACK(+[](LadybirdLocationEntry* self, GtkListBoxRow* row) {
        auto index = gtk_list_box_row_get_index(row);
        if (index >= 0 && static_cast<size_t>(index) < self->state->suggestions.size()) {
            set_entry_text_suppressed(self, self->state->suggestions[index].text.to_byte_string().characters());
            ladybird_location_entry_hide_completions(self);
            ladybird_location_entry_navigate(self);
        }
    }),
        self);

    // Autocomplete results callback
    self->state->autocomplete->on_autocomplete_query_complete = [self](auto suggestions, WebView::AutocompleteResultKind kind) {
        if (suggestions.is_empty() || !self->state->is_focused) {
            ladybird_location_entry_hide_completions(self);
            return;
        }

        if (kind != WebView::AutocompleteResultKind::Final)
            return;

        self->state->suggestions = move(suggestions);
        self->state->selected_index = -1;
        ladybird_location_entry_show_completions(self);
    };

    // Text changed -> query autocomplete
    g_signal_connect_swapped(self, "changed", G_CALLBACK(+[](LadybirdLocationEntry* self, GtkEditable*) {
        if (!self->state->is_focused || self->state->updating_text)
            return;
        auto* text = gtk_editable_get_text(GTK_EDITABLE(self));
        if (!text || text[0] == '\0') {
            ladybird_location_entry_hide_completions(self);
            return;
        }
        self->state->user_text = MUST(String::from_utf8(StringView { text, strlen(text) }));
        self->state->autocomplete->query_autocomplete_engine(self->state->user_text);
    }),
        self);

    // Enter navigates
    g_signal_connect_swapped(self, "activate", G_CALLBACK(+[](LadybirdLocationEntry* self, GtkEntry*) {
        ladybird_location_entry_hide_completions(self);
        ladybird_location_entry_navigate(self);
    }),
        self);

    // Key controller for Up/Down/Escape
    auto* key_controller = gtk_event_controller_key_new();
    g_signal_connect_swapped(key_controller, "key-pressed", G_CALLBACK(+[](LadybirdLocationEntry* self, guint keyval, guint, GdkModifierType) -> gboolean {
        if (!gtk_widget_get_visible(GTK_WIDGET(self->popover)))
            return GDK_EVENT_PROPAGATE;

        switch (keyval) {
        case GDK_KEY_Down:
            ladybird_location_entry_move_selection(self, 1);
            return GDK_EVENT_STOP;
        case GDK_KEY_Up:
            ladybird_location_entry_move_selection(self, -1);
            return GDK_EVENT_STOP;
        case GDK_KEY_Escape:
            ladybird_location_entry_hide_completions(self);
            if (!self->state->user_text.is_empty())
                set_entry_text_suppressed(self, self->state->user_text.to_byte_string().characters());
            return GDK_EVENT_STOP;
        default:
            return GDK_EVENT_PROPAGATE;
        }
    }),
        self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(key_controller));

    // Focus tracking
    auto* focus_controller = gtk_event_controller_focus_new();
    g_signal_connect_swapped(focus_controller, "enter", G_CALLBACK(+[](LadybirdLocationEntry* self, GtkEventControllerFocus*) {
        self->state->is_focused = true;
        gtk_entry_set_attributes(GTK_ENTRY(self), nullptr);
    }),
        self);
    g_signal_connect_swapped(focus_controller, "leave", G_CALLBACK(+[](LadybirdLocationEntry* self, GtkEventControllerFocus*) {
        self->state->is_focused = false;
        ladybird_location_entry_hide_completions(self);
        ladybird_location_entry_update_display_attributes(self);
        gtk_editable_select_region(GTK_EDITABLE(self), 0, 0);
    }),
        self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(focus_controller));
}

// Public API

LadybirdLocationEntry* ladybird_location_entry_new(void)
{
    return LADYBIRD_LOCATION_ENTRY(g_object_new(LADYBIRD_TYPE_LOCATION_ENTRY, nullptr));
}

void ladybird_location_entry_set_url(LadybirdLocationEntry* self, char const* url)
{
    set_entry_text_suppressed(self, url ? url : "");

    if (!self->state->is_focused)
        ladybird_location_entry_update_display_attributes(self);
}

void ladybird_location_entry_set_text(LadybirdLocationEntry* self, char const* text)
{
    set_entry_text_suppressed(self, text ? text : "");
    gtk_entry_set_attributes(GTK_ENTRY(self), nullptr);
    ladybird_location_entry_set_favicon(self, nullptr);
}

void ladybird_location_entry_set_favicon(LadybirdLocationEntry* self, GdkPaintable* favicon)
{
    self->state->favicon = Ladybird::GObjectPtr<GdkPaintable>(favicon ? GDK_PAINTABLE(g_object_ref(favicon)) : nullptr);
    ladybird_location_entry_update_leading_icon(self);
}

void ladybird_location_entry_set_loading(LadybirdLocationEntry* self, bool is_loading)
{
    if (self->state->is_loading == is_loading)
        return;

    self->state->is_loading = is_loading;
    if (self->state->is_loading) {
        gtk_widget_add_css_class(GTK_WIDGET(self), "loading");
        gtk_entry_set_progress_pulse_step(GTK_ENTRY(self), 0.15);
        self->state->loading_pulse_source_id = g_timeout_add_full(
            G_PRIORITY_DEFAULT,
            100,
            +[](gpointer user_data) -> gboolean {
                auto* self = LADYBIRD_LOCATION_ENTRY(user_data);
                gtk_entry_progress_pulse(GTK_ENTRY(self));
                return G_SOURCE_CONTINUE;
            },
            self,
            nullptr);
    } else {
        if (self->state->loading_pulse_source_id != 0) {
            g_source_remove(self->state->loading_pulse_source_id);
            self->state->loading_pulse_source_id = 0;
        }
        gtk_widget_remove_css_class(GTK_WIDGET(self), "loading");
    }

    ladybird_location_entry_update_leading_icon(self);
}

static void ladybird_location_entry_update_leading_icon(LadybirdLocationEntry* self)
{
    if (self->state->is_loading) {
        gtk_entry_set_icon_from_icon_name(GTK_ENTRY(self), GTK_ENTRY_ICON_PRIMARY, "content-loading-symbolic");
        gtk_entry_set_icon_tooltip_text(GTK_ENTRY(self), GTK_ENTRY_ICON_PRIMARY, "Loading");
        return;
    }

    if (self->state->favicon.ptr()) {
        gtk_entry_set_icon_from_paintable(GTK_ENTRY(self), GTK_ENTRY_ICON_PRIMARY, self->state->favicon.ptr());
        gtk_entry_set_icon_tooltip_text(GTK_ENTRY(self), GTK_ENTRY_ICON_PRIMARY, "Page icon");
        return;
    }

    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(self), GTK_ENTRY_ICON_PRIMARY, "globe-alt2-symbolic");
    gtk_entry_set_icon_tooltip_text(GTK_ENTRY(self), GTK_ENTRY_ICON_PRIMARY, nullptr);
}

void ladybird_location_entry_focus_and_select_all(LadybirdLocationEntry* self)
{
    gtk_widget_grab_focus(GTK_WIDGET(self));
    gtk_editable_select_region(GTK_EDITABLE(self), 0, -1);
}

void ladybird_location_entry_set_on_navigate(LadybirdLocationEntry* self, Function<void(String)> callback)
{
    self->state->on_navigate = move(callback);
}

// Internal helpers

static void ladybird_location_entry_update_display_attributes(LadybirdLocationEntry* self)
{
    auto* text = gtk_editable_get_text(GTK_EDITABLE(self));
    if (!text || text[0] == '\0') {
        gtk_entry_set_attributes(GTK_ENTRY(self), nullptr);
        return;
    }

    auto url_str = StringView(text, strlen(text));
    auto url_parts = WebView::break_url_into_parts(url_str);

    auto* attrs = pango_attr_list_new();

    if (url_parts.has_value()) {
        auto* dim = pango_attr_foreground_alpha_new(40000);
        pango_attr_list_insert(attrs, dim);

        auto highlight_start = url_parts->scheme_and_subdomain.length();
        auto highlight_end = highlight_start + url_parts->effective_tld_plus_one.length();

        if (highlight_start < highlight_end) {
            auto* domain_alpha = pango_attr_foreground_alpha_new(65535);
            domain_alpha->start_index = highlight_start;
            domain_alpha->end_index = highlight_end;
            pango_attr_list_insert(attrs, domain_alpha);

            auto* semi_bold = pango_attr_weight_new(PANGO_WEIGHT_MEDIUM);
            semi_bold->start_index = highlight_start;
            semi_bold->end_index = highlight_end;
            pango_attr_list_insert(attrs, semi_bold);
        }
    }

    gtk_entry_set_attributes(GTK_ENTRY(self), attrs);
    pango_attr_list_unref(attrs);
}

static void ladybird_location_entry_navigate(LadybirdLocationEntry* self)
{
    auto* text = gtk_editable_get_text(GTK_EDITABLE(self));
    if (!text || text[0] == '\0')
        return;
    auto query = MUST(String::from_utf8(StringView { text, strlen(text) }));
    if (auto url = WebView::sanitize_url(query, WebView::Application::settings().search_engine()); url.has_value()) {
        if (self->state->on_navigate)
            self->state->on_navigate(url->serialize());
    }
}

static char const* completion_section_to_string(WebView::AutocompleteSuggestionSection section)
{
    switch (section) {
    case WebView::AutocompleteSuggestionSection::History:
        return "History";

    case WebView::AutocompleteSuggestionSection::SearchSuggestions:
        return "Search suggestions";

    default:
        return nullptr;
    }
}

static char const* icon_name_from_completion_source(WebView::AutocompleteSuggestionSource source)
{
    switch (source) {
    case WebView::AutocompleteSuggestionSource::Search:
        return "loupe-large-symbolic";

    case WebView::AutocompleteSuggestionSource::History:
        return "history-undo-symbolic";

    default:
        return "globe-alt2-symbolic";
    }
}

static GtkWidget* completion_item_new(WebView::AutocompleteSuggestion const& suggestion)
{
    auto* main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    // Setup title box
    auto* title_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* icon = gtk_image_new();
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(title_box), icon);

    auto* title = gtk_label_new(nullptr);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_box_append(GTK_BOX(title_box), title);

    gtk_widget_set_halign(title_box, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(title_box, true);
    gtk_box_append(GTK_BOX(main_box), title_box);

    // Setup subtitle
    auto* subtitle = gtk_label_new(nullptr);
    gtk_label_set_ellipsize(GTK_LABEL(subtitle), PANGO_ELLIPSIZE_END);
    gtk_widget_set_halign(subtitle, GTK_ALIGN_START);
    gtk_widget_add_css_class(subtitle, "dimmed");
    gtk_box_append(GTK_BOX(main_box), subtitle);

    bool is_icon_set = false;
    if (suggestion.favicon_base64_png.has_value()) {
        gsize size;
        g_autofree auto* icon_buffer = g_base64_decode(suggestion.favicon_base64_png.value().to_byte_string().characters(), &size);

        if (icon_buffer) {
            GdkTexture* texture = gdk_texture_new_from_bytes(g_bytes_new(icon_buffer, size), nullptr);

            if (texture) {
                is_icon_set = true;
                gtk_image_set_from_paintable(GTK_IMAGE(icon), GDK_PAINTABLE(texture));
            }
        }
    }
    if (!is_icon_set) {
        gtk_image_set_from_icon_name(GTK_IMAGE(icon), icon_name_from_completion_source(suggestion.source));
    }

    char const* title_text = suggestion.title
                                 .value_or(suggestion.text)
                                 .to_byte_string()
                                 .characters();
    gtk_label_set_label(GTK_LABEL(title), title_text);

    char const* subtitle_text = nullptr;
    if (suggestion.subtitle.has_value())
        subtitle_text = suggestion.subtitle.value().to_byte_string().characters();
    else if (suggestion.title.has_value())
        subtitle_text = suggestion.text.to_byte_string().characters();

    if (subtitle_text) {
        gtk_label_set_label(GTK_LABEL(subtitle), subtitle_text);
        gtk_widget_set_visible(GTK_WIDGET(subtitle), true);
    } else {
        gtk_widget_set_visible(GTK_WIDGET(subtitle), false);
    }

    GtkWidget* row = gtk_list_box_row_new();
    g_object_set_data(G_OBJECT(row), "section-name", const_cast<char*>(completion_section_to_string(suggestion.section)));
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), main_box);

    return row;
}

static void ladybird_location_entry_show_completions(LadybirdLocationEntry* self)
{
    GtkWidget* child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->list_box))) != nullptr)
        gtk_list_box_remove(self->list_box, child);

    for (auto const& suggestion : self->state->suggestions) {
        gtk_list_box_append(self->list_box, completion_item_new(suggestion));
    }

    gtk_list_box_unselect_all(self->list_box);

    graphene_rect_t bounds;
    bool success = gtk_widget_compute_bounds(GTK_WIDGET(self), GTK_WIDGET(self), &bounds);
    if (bounds.size.width > 0 && success)
        gtk_widget_set_size_request(GTK_WIDGET(self->popover), static_cast<int>(bounds.size.width), -1);

    gtk_popover_popup(self->popover);
}

static void ladybird_location_entry_hide_completions(LadybirdLocationEntry* self)
{
    self->state->suggestions.clear();
    self->state->selected_index = -1;
    gtk_popover_popdown(self->popover);
}

static void ladybird_location_entry_move_selection(LadybirdLocationEntry* self, int delta)
{
    auto& state = *self->state;
    if (state.suggestions.is_empty())
        return;

    auto new_index = state.selected_index + delta;
    if (new_index < 0)
        new_index = static_cast<int>(state.suggestions.size()) - 1;
    if (new_index >= static_cast<int>(state.suggestions.size()))
        new_index = 0;

    state.selected_index = new_index;

    auto* row = gtk_list_box_get_row_at_index(self->list_box, state.selected_index);
    gtk_list_box_select_row(self->list_box, row);
    ladybird_location_entry_apply_selected_suggestion(self);

    // Scroll to current row if outside of view
    auto* scrolled_window = gtk_widget_get_ancestor(GTK_WIDGET(self->list_box), GTK_TYPE_SCROLLED_WINDOW);
    if (scrolled_window) {
        graphene_rect_t row_bounds;
        bool success = gtk_widget_compute_bounds(GTK_WIDGET(row), GTK_WIDGET(self->list_box), &row_bounds);

        if (success) {
            auto* v_adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled_window));
            double current_y = gtk_adjustment_get_value(v_adjustment);
            double height = gtk_widget_get_height(GTK_WIDGET(scrolled_window));

            // If the current row is not fully visible
            if (row_bounds.origin.y < current_y || row_bounds.origin.y + row_bounds.size.height > current_y + height) {
                gtk_adjustment_set_value(v_adjustment, new_index == 0 ? 0 : row_bounds.origin.y);
                gtk_scrolled_window_set_vadjustment(GTK_SCROLLED_WINDOW(scrolled_window), v_adjustment);
            }
        }
    }
}

static void ladybird_location_entry_apply_selected_suggestion(LadybirdLocationEntry* self)
{
    auto& state = *self->state;
    if (state.selected_index < 0 || static_cast<size_t>(state.selected_index) >= state.suggestions.size())
        return;

    set_entry_text_suppressed(self, state.suggestions[state.selected_index].text.to_byte_string().characters(), true);
}
