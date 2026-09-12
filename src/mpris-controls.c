#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>

typedef enum
{
    POPUP_POSITION_AUTOMATIC,
    POPUP_POSITION_ABOVE,
    POPUP_POSITION_BELOW
} PopupPosition;

typedef enum
{
    PANEL_TRACK_POSITION_BEFORE,
    PANEL_TRACK_POSITION_AFTER
} PanelTrackPosition;

typedef enum
{
    PANEL_TRACK_WIDTH_DYNAMIC,
    PANEL_TRACK_WIDTH_FIXED
} PanelTrackWidth;

typedef struct
{
    XfcePanelPlugin *plugin;
    GtkWidget *box;
    GtkWidget *previous_button;
    GtkWidget *play_pause_button;
    GtkWidget *play_pause_image;
    GtkWidget *next_button;
    GtkWidget *panel_track_container;
    GtkWidget *panel_track_label;
    GtkWidget *metadata_window;
    GtkWidget *artwork_image;
    GtkWidget *title_label;
    GtkWidget *artist_label;
    GtkWidget *album_label;
    GDBusConnection *dbus_connection;
    gchar *active_player;
    gchar *panel_track_text;
    gchar *artwork_url;
    GCancellable *artwork_cancellable;
    guint status_timeout_id;
    guint hover_show_timeout_id;
    guint hover_hide_timeout_id;
    guint panel_scroll_timeout_id;
    guint panel_scroll_position;
    guint hover_delay_ms;
    guint panel_fixed_width;
    PopupPosition popup_position;
    PanelTrackPosition panel_track_position;
    PanelTrackWidth panel_track_width;
    GdkRGBA panel_text_color;
    gboolean hover_enabled;
    gboolean show_panel_track;
    gboolean panel_text_color_custom;
    gboolean show_artwork;
    gboolean show_artist;
    gboolean show_album;
    gboolean show_previous;
    gboolean show_next;
    gboolean is_playing;
} MprisControlsPlugin;

typedef struct
{
    GWeakRef plugin_ref;
    gchar *artwork_url;
    GCancellable *cancellable;
} ArtworkRequest;

static void set_play_pause_state(MprisControlsPlugin *controls, gboolean is_playing);
static gboolean refresh_play_pause_state(gpointer user_data);
static void save_settings(XfcePanelPlugin *plugin, MprisControlsPlugin *controls);

static gchar **
get_mpris_players(MprisControlsPlugin *controls)
{
    GVariant *reply;
    GVariantIter *iter;
    const gchar *name = NULL;
    GPtrArray *players;
    GError *error = NULL;

    if (controls->dbus_connection == NULL)
        return NULL;

    reply = g_dbus_connection_call_sync(controls->dbus_connection,
                                        "org.freedesktop.DBus",
                                        "/org/freedesktop/DBus",
                                        "org.freedesktop.DBus",
                                        "ListNames",
                                        NULL,
                                        G_VARIANT_TYPE("(as)"),
                                        G_DBUS_CALL_FLAGS_NONE,
                                        1000,
                                        NULL,
                                        &error);
    if (reply == NULL)
    {
        g_clear_error(&error);
        return NULL;
    }

    players = g_ptr_array_new_with_free_func(g_free);
    g_variant_get(reply, "(as)", &iter);
    while (g_variant_iter_loop(iter, "&s", &name))
    {
        if (g_str_has_prefix(name, "org.mpris.MediaPlayer2."))
            g_ptr_array_add(players, g_strdup(name));
    }

    g_variant_iter_free(iter);
    g_variant_unref(reply);
    g_ptr_array_add(players, NULL);

    return (gchar **) g_ptr_array_free(players, FALSE);
}

static gboolean
get_player_is_playing(MprisControlsPlugin *controls, const gchar *player)
{
    GVariant *reply;
    GVariant *value;
    const gchar *status;
    GError *error = NULL;
    gboolean is_playing = FALSE;

    if (controls->dbus_connection == NULL || player == NULL)
        return FALSE;

    reply = g_dbus_connection_call_sync(controls->dbus_connection,
                                        player,
                                        "/org/mpris/MediaPlayer2",
                                        "org.freedesktop.DBus.Properties",
                                        "Get",
                                        g_variant_new("(ss)", "org.mpris.MediaPlayer2.Player", "PlaybackStatus"),
                                        G_VARIANT_TYPE("(v)"),
                                        G_DBUS_CALL_FLAGS_NONE,
                                        500,
                                        NULL,
                                        &error);
    if (reply == NULL)
    {
        g_clear_error(&error);
        return FALSE;
    }

    g_variant_get(reply, "(v)", &value);
    status = g_variant_get_string(value, NULL);
    is_playing = g_strcmp0(status, "Playing") == 0;

    g_variant_unref(value);
    g_variant_unref(reply);

    return is_playing;
}

static gboolean
player_name_exists(gchar **players, const gchar *player)
{
    guint i;

    if (players == NULL || player == NULL)
        return FALSE;

    for (i = 0; players[i] != NULL; i++)
    {
        if (g_strcmp0(players[i], player) == 0)
            return TRUE;
    }

    return FALSE;
}

static gchar *
choose_active_player(MprisControlsPlugin *controls, gchar **players, gboolean *is_playing)
{
    guint i;

    if (is_playing != NULL)
        *is_playing = FALSE;

    if (players == NULL || players[0] == NULL)
        return NULL;

    if (player_name_exists(players, controls->active_player)
        && get_player_is_playing(controls, controls->active_player))
    {
        if (is_playing != NULL)
            *is_playing = TRUE;
        return g_strdup(controls->active_player);
    }

    for (i = 0; players[i] != NULL; i++)
    {
        if (g_strcmp0(players[i], controls->active_player) == 0)
            continue;

        if (get_player_is_playing(controls, players[i]))
        {
            if (is_playing != NULL)
                *is_playing = TRUE;
            return g_strdup(players[i]);
        }
    }

    if (player_name_exists(players, controls->active_player))
        return g_strdup(controls->active_player);

    return g_strdup(players[0]);
}

static void
store_active_player(MprisControlsPlugin *controls, const gchar *player)
{
    if (g_strcmp0(controls->active_player, player) == 0)
        return;

    g_free(controls->active_player);
    controls->active_player = g_strdup(player);
}

static gboolean
get_player_metadata(MprisControlsPlugin *controls,
                    const gchar *player,
                    gchar **title,
                    gchar **artist,
                    gchar **album,
                    gchar **artwork_url)
{
    GVariant *reply;
    GVariant *metadata;
    GVariantIter iter;
    GVariant *value;
    const gchar *key;
    GError *error = NULL;
    gboolean found = FALSE;

    if (title != NULL)
        *title = NULL;
    if (artist != NULL)
        *artist = NULL;
    if (album != NULL)
        *album = NULL;
    if (artwork_url != NULL)
        *artwork_url = NULL;

    if (controls->dbus_connection == NULL || player == NULL)
        return FALSE;

    reply = g_dbus_connection_call_sync(controls->dbus_connection,
                                        player,
                                        "/org/mpris/MediaPlayer2",
                                        "org.freedesktop.DBus.Properties",
                                        "Get",
                                        g_variant_new("(ss)", "org.mpris.MediaPlayer2.Player", "Metadata"),
                                        G_VARIANT_TYPE("(v)"),
                                        G_DBUS_CALL_FLAGS_NONE,
                                        500,
                                        NULL,
                                        &error);
    if (reply == NULL)
    {
        g_clear_error(&error);
        return FALSE;
    }

    g_variant_get(reply, "(v)", &metadata);
    g_variant_iter_init(&iter, metadata);
    while (g_variant_iter_loop(&iter, "{&sv}", &key, &value))
    {
        if (title != NULL && *title == NULL && g_strcmp0(key, "xesam:title") == 0
            && g_variant_is_of_type(value, G_VARIANT_TYPE_STRING))
        {
            *title = g_variant_dup_string(value, NULL);
            found = TRUE;
        }
        else if (artist != NULL && *artist == NULL && g_strcmp0(key, "xesam:artist") == 0)
        {
            if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING))
            {
                *artist = g_variant_dup_string(value, NULL);
                found = TRUE;
            }
            else if (g_variant_is_of_type(value, G_VARIANT_TYPE("as")))
            {
                gchar **artists = g_variant_dup_strv(value, NULL);

                *artist = g_strjoinv(", ", artists);
                g_strfreev(artists);
                found = *artist != NULL;
            }
        }
        else if (album != NULL && *album == NULL && g_strcmp0(key, "xesam:album") == 0
                 && g_variant_is_of_type(value, G_VARIANT_TYPE_STRING))
        {
            *album = g_variant_dup_string(value, NULL);
            found = TRUE;
        }
        else if (artwork_url != NULL && *artwork_url == NULL && g_strcmp0(key, "mpris:artUrl") == 0
                 && g_variant_is_of_type(value, G_VARIANT_TYPE_STRING))
        {
            *artwork_url = g_variant_dup_string(value, NULL);
            found = TRUE;
        }
    }

    g_variant_unref(metadata);
    g_variant_unref(reply);

    return found;
}

static void
stop_panel_track_scroll(MprisControlsPlugin *controls)
{
    if (controls->panel_scroll_timeout_id != 0)
    {
        g_source_remove(controls->panel_scroll_timeout_id);
        controls->panel_scroll_timeout_id = 0;
    }
}

static gboolean
panel_track_needs_scroll(MprisControlsPlugin *controls)
{
    PangoLayout *layout;
    gint width;

    if (!controls->show_panel_track
        || controls->panel_track_width != PANEL_TRACK_WIDTH_FIXED
        || controls->panel_track_text == NULL
        || *controls->panel_track_text == '\0')
    {
        return FALSE;
    }

    layout = gtk_widget_create_pango_layout(controls->panel_track_label,
                                            controls->panel_track_text);
    pango_layout_get_pixel_size(layout, &width, NULL);
    g_object_unref(layout);

    return width > MAX(1, (gint) controls->panel_fixed_width - 8);
}

static gboolean
panel_track_scroll(gpointer user_data)
{
    MprisControlsPlugin *controls = user_data;
    gsize cycle_length;

    if (!panel_track_needs_scroll(controls))
    {
        controls->panel_scroll_timeout_id = 0;
        gtk_widget_queue_draw(controls->panel_track_label);
        return G_SOURCE_REMOVE;
    }

    cycle_length = (gsize) g_utf8_strlen(controls->panel_track_text, -1) + 4;
    controls->panel_scroll_position++;
    if (controls->panel_scroll_position >= cycle_length)
        controls->panel_scroll_position = 0;
    gtk_widget_queue_draw(controls->panel_track_label);

    return G_SOURCE_CONTINUE;
}

static gint
panel_track_text_width(MprisControlsPlugin *controls)
{
    PangoLayout *layout;
    gint width;

    if (controls->panel_track_text == NULL || *controls->panel_track_text == '\0')
        return 8;

    layout = gtk_widget_create_pango_layout(controls->panel_track_label,
                                            controls->panel_track_text);
    pango_layout_get_pixel_size(layout, &width, NULL);
    g_object_unref(layout);

    return width + 8;
}

static gboolean
panel_track_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
    MprisControlsPlugin *controls = user_data;
    GtkStyleContext *style_context;
    PangoLayout *layout;
    PangoRectangle position;
    GdkRGBA color;
    GtkAllocation allocation;
    gchar *cycle_text = NULL;
    const gchar *layout_text;
    gint text_height;
    gint x = 4;
    gint y;

    if (controls->panel_track_text == NULL || *controls->panel_track_text == '\0')
        return FALSE;

    gtk_widget_get_allocation(widget, &allocation);
    layout_text = controls->panel_track_text;
    if (panel_track_needs_scroll(controls))
    {
        cycle_text = g_strconcat(controls->panel_track_text,
                                 "    ",
                                 controls->panel_track_text,
                                 NULL);
        layout_text = cycle_text;
    }

    layout = gtk_widget_create_pango_layout(widget, layout_text);
    pango_layout_set_single_paragraph_mode(layout, TRUE);
    pango_layout_get_pixel_size(layout, NULL, &text_height);
    y = (allocation.height - text_height) / 2;

    if (cycle_text != NULL)
    {
        const gchar *offset = g_utf8_offset_to_pointer(cycle_text,
                                                        (glong) controls->panel_scroll_position);
        gint byte_offset = (gint) (offset - cycle_text);

        pango_layout_index_to_pos(layout, byte_offset, &position);
        x -= PANGO_PIXELS(position.x);
    }

    cairo_save(cr);
    cairo_rectangle(cr, 4, 0, MAX(1, allocation.width - 8), allocation.height);
    cairo_clip(cr);
    cairo_move_to(cr, x, y);
    style_context = gtk_widget_get_style_context(widget);
    if (controls->panel_text_color_custom)
        color = controls->panel_text_color;
    else
        gtk_style_context_get_color(style_context,
                                    gtk_widget_get_state_flags(widget),
                                    &color);
    gdk_cairo_set_source_rgba(cr, &color);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);

    g_object_unref(layout);
    g_free(cycle_text);

    return FALSE;
}

static void
update_panel_track_display(MprisControlsPlugin *controls)
{
    stop_panel_track_scroll(controls);
    controls->panel_scroll_position = 0;

    gtk_widget_set_size_request(controls->panel_track_container,
                                controls->panel_track_width == PANEL_TRACK_WIDTH_FIXED
                                    ? (gint) controls->panel_fixed_width
                                    : panel_track_text_width(controls),
                                -1);
    gtk_widget_queue_resize(controls->panel_track_container);
    gtk_widget_queue_draw(controls->panel_track_label);

    if (panel_track_needs_scroll(controls))
    {
        controls->panel_scroll_timeout_id = g_timeout_add(180,
                                                           panel_track_scroll,
                                                           controls);
    }
}

static void
set_panel_track_text(MprisControlsPlugin *controls, const gchar *text)
{
    if (g_strcmp0(controls->panel_track_text, text) == 0)
        return;

    g_free(controls->panel_track_text);
    controls->panel_track_text = g_strdup(text);
    update_panel_track_display(controls);
}

static void
refresh_panel_track_label(MprisControlsPlugin *controls)
{
    gchar *title = NULL;
    gchar *artist = NULL;
    gchar *panel_text;

    if (controls->active_player != NULL)
        get_player_metadata(controls, controls->active_player, &title, &artist, NULL, NULL);

    if (title != NULL && *title != '\0' && artist != NULL && *artist != '\0')
        panel_text = g_strdup_printf("%s - %s", title, artist);
    else if (title != NULL && *title != '\0')
        panel_text = g_strdup(title);
    else if (artist != NULL && *artist != '\0')
        panel_text = g_strdup(artist);
    else
        panel_text = g_strdup("No track playing");

    set_panel_track_text(controls, panel_text);

    g_free(panel_text);
    g_free(title);
    g_free(artist);
}

static gboolean
call_mpris_player_method(MprisControlsPlugin *controls, const gchar *method)
{
    if (controls->dbus_connection == NULL || controls->active_player == NULL)
        return FALSE;

    g_dbus_connection_call(controls->dbus_connection,
                           controls->active_player,
                           "/org/mpris/MediaPlayer2",
                           "org.mpris.MediaPlayer2.Player",
                           method,
                           NULL,
                           NULL,
                           G_DBUS_CALL_FLAGS_NONE,
                           1000,
                           NULL,
                           NULL,
                           NULL);

    return TRUE;
}

static GtkWidget *
create_icon_button(const gchar *icon_name, const gchar *tooltip, GCallback callback, gpointer user_data, GtkWidget **image_out)
{
    GtkWidget *button = gtk_button_new();
    GtkWidget *image = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_BUTTON);

    gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(button, FALSE);
    gtk_widget_set_tooltip_text(button, tooltip);
    gtk_container_add(GTK_CONTAINER(button), image);
    gtk_widget_show(image);

    g_signal_connect(button, "clicked", callback, user_data);

    if (image_out != NULL)
        *image_out = image;

    return button;
}

static void
previous_clicked(GtkButton *button, gpointer user_data)
{
    (void) button;

    call_mpris_player_method(user_data, "Previous");
}

static void
play_pause_clicked(GtkButton *button, gpointer user_data)
{
    (void) button;

    if (call_mpris_player_method(user_data, "PlayPause"))
        set_play_pause_state(user_data, !((MprisControlsPlugin *) user_data)->is_playing);
}

static void
next_clicked(GtkButton *button, gpointer user_data)
{
    (void) button;

    call_mpris_player_method(user_data, "Next");
}

static void
artwork_request_free(ArtworkRequest *request)
{
    g_weak_ref_clear(&request->plugin_ref);
    g_clear_object(&request->cancellable);
    g_free(request->artwork_url);
    g_free(request);
}

static void
set_artwork_fallback(MprisControlsPlugin *controls)
{
    gtk_image_set_from_icon_name(GTK_IMAGE(controls->artwork_image),
                                 "audio-x-generic-symbolic",
                                 GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(controls->artwork_image), 48);
}

static void
artwork_pixbuf_ready(GObject *source_object, GAsyncResult *result, gpointer user_data)
{
    ArtworkRequest *request = user_data;
    XfcePanelPlugin *plugin;
    MprisControlsPlugin *controls = NULL;
    GdkPixbuf *pixbuf;
    GError *error = NULL;

    (void) source_object;

    pixbuf = gdk_pixbuf_new_from_stream_finish(result, &error);
    plugin = g_weak_ref_get(&request->plugin_ref);
    if (plugin != NULL)
        controls = g_object_get_data(G_OBJECT(plugin), "mpris-controls");

    if (pixbuf != NULL && controls != NULL
        && g_strcmp0(controls->artwork_url, request->artwork_url) == 0
        && !g_cancellable_is_cancelled(request->cancellable))
    {
        gtk_image_set_from_pixbuf(GTK_IMAGE(controls->artwork_image), pixbuf);
    }

    g_clear_error(&error);
    g_clear_object(&pixbuf);
    g_clear_object(&plugin);
    artwork_request_free(request);
}

static void
artwork_stream_ready(GObject *source_object, GAsyncResult *result, gpointer user_data)
{
    ArtworkRequest *request = user_data;
    GFileInputStream *stream;
    GError *error = NULL;

    stream = g_file_read_finish(G_FILE(source_object), result, &error);
    if (stream == NULL)
    {
        g_clear_error(&error);
        artwork_request_free(request);
        return;
    }

    gdk_pixbuf_new_from_stream_at_scale_async(G_INPUT_STREAM(stream),
                                               80,
                                               80,
                                               TRUE,
                                               request->cancellable,
                                               artwork_pixbuf_ready,
                                               request);
    g_object_unref(stream);
}

static void
load_artwork(MprisControlsPlugin *controls, const gchar *artwork_url)
{
    ArtworkRequest *request;
    GFile *file;

    if (g_strcmp0(controls->artwork_url, artwork_url) == 0)
        return;

    if (controls->artwork_cancellable != NULL)
        g_cancellable_cancel(controls->artwork_cancellable);
    g_clear_object(&controls->artwork_cancellable);

    g_free(controls->artwork_url);
    controls->artwork_url = g_strdup(artwork_url);
    set_artwork_fallback(controls);

    if (artwork_url == NULL || *artwork_url == '\0')
        return;

    controls->artwork_cancellable = g_cancellable_new();
    request = g_new0(ArtworkRequest, 1);
    g_weak_ref_init(&request->plugin_ref, G_OBJECT(controls->plugin));
    request->artwork_url = g_strdup(artwork_url);
    request->cancellable = g_object_ref(controls->artwork_cancellable);

    if (g_uri_peek_scheme(artwork_url) != NULL)
        file = g_file_new_for_uri(artwork_url);
    else
        file = g_file_new_for_path(artwork_url);

    g_file_read_async(file,
                      G_PRIORITY_DEFAULT,
                      request->cancellable,
                      artwork_stream_ready,
                      request);
    g_object_unref(file);
}

static void
refresh_metadata_window(MprisControlsPlugin *controls)
{
    gchar *title = NULL;
    gchar *artist = NULL;
    gchar *album = NULL;
    gchar *artwork_url = NULL;

    if (controls->active_player == NULL
        || !get_player_metadata(controls, controls->active_player, &title, &artist, &album, &artwork_url))
    {
        gtk_label_set_text(GTK_LABEL(controls->title_label), "No active MPRIS player");
        if (controls->show_artist)
        {
            gtk_label_set_text(GTK_LABEL(controls->artist_label), "Start playback to show track details");
            gtk_widget_show(controls->artist_label);
        }
        else
            gtk_widget_hide(controls->artist_label);
        gtk_widget_hide(controls->album_label);
    }
    else
    {
        gtk_label_set_text(GTK_LABEL(controls->title_label),
                           title != NULL && *title != '\0' ? title : "Unknown title");
        if (controls->show_artist)
        {
            gtk_label_set_text(GTK_LABEL(controls->artist_label),
                               artist != NULL && *artist != '\0' ? artist : "Unknown artist");
            gtk_widget_show(controls->artist_label);
        }
        else
            gtk_widget_hide(controls->artist_label);

        if (controls->show_album && album != NULL && *album != '\0')
        {
            gtk_label_set_text(GTK_LABEL(controls->album_label), album);
            gtk_widget_show(controls->album_label);
        }
        else
            gtk_widget_hide(controls->album_label);

    }

    if (controls->show_artwork)
    {
        gtk_widget_show(controls->artwork_image);
        load_artwork(controls, artwork_url);
    }
    else
    {
        gtk_widget_hide(controls->artwork_image);
        load_artwork(controls, NULL);
    }

    g_free(title);
    g_free(artist);
    g_free(album);
    g_free(artwork_url);
}

static void
position_metadata_window(MprisControlsPlugin *controls)
{
    GtkWidget *toplevel;
    GtkAllocation button_allocation;
    GtkRequisition natural_size;
    GdkWindow *toplevel_window;
    GdkDisplay *display;
    GdkMonitor *monitor;
    GdkRectangle workarea;
    gint root_x;
    gint root_y;
    gint relative_x = 0;
    gint relative_y = 0;
    gint popup_x;
    gint popup_y;

    toplevel = gtk_widget_get_toplevel(controls->play_pause_button);
    toplevel_window = gtk_widget_get_window(toplevel);
    if (toplevel_window == NULL)
        return;

    gdk_window_get_origin(toplevel_window, &root_x, &root_y);
    gtk_widget_translate_coordinates(controls->play_pause_button, toplevel, 0, 0,
                                     &relative_x, &relative_y);
    gtk_widget_get_allocation(controls->play_pause_button, &button_allocation);
    gtk_widget_get_preferred_size(controls->metadata_window, NULL, &natural_size);

    popup_x = root_x + relative_x + (button_allocation.width - natural_size.width) / 2;
    popup_y = root_y + relative_y - natural_size.height - 8;

    display = gtk_widget_get_display(controls->play_pause_button);
    monitor = gdk_display_get_monitor_at_window(display, toplevel_window);
    if (monitor == NULL)
        return;
    gdk_monitor_get_workarea(monitor, &workarea);

    popup_x = CLAMP(popup_x, workarea.x, workarea.x + workarea.width - natural_size.width);
    if (controls->popup_position == POPUP_POSITION_BELOW)
        popup_y = root_y + relative_y + button_allocation.height + 8;
    else if (controls->popup_position == POPUP_POSITION_AUTOMATIC && popup_y < workarea.y)
        popup_y = root_y + relative_y + button_allocation.height + 8;

    popup_y = CLAMP(popup_y, workarea.y, workarea.y + workarea.height - natural_size.height);

    gtk_window_move(GTK_WINDOW(controls->metadata_window), popup_x, popup_y);
}

static gboolean
show_metadata_window(gpointer user_data)
{
    MprisControlsPlugin *controls = user_data;

    controls->hover_show_timeout_id = 0;
    if (!controls->hover_enabled)
        return G_SOURCE_REMOVE;

    refresh_metadata_window(controls);
    position_metadata_window(controls);
    gtk_widget_show(controls->metadata_window);

    return G_SOURCE_REMOVE;
}

static gboolean
hide_metadata_window(gpointer user_data)
{
    MprisControlsPlugin *controls = user_data;

    controls->hover_hide_timeout_id = 0;
    gtk_widget_hide(controls->metadata_window);

    return G_SOURCE_REMOVE;
}

static void
cancel_hover_timeout(guint *timeout_id)
{
    if (*timeout_id != 0)
    {
        g_source_remove(*timeout_id);
        *timeout_id = 0;
    }
}

static gboolean
hover_entered(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
    MprisControlsPlugin *controls = user_data;

    (void) widget;
    if (event->mode != GDK_CROSSING_NORMAL)
        return FALSE;

    cancel_hover_timeout(&controls->hover_hide_timeout_id);
    if (controls->hover_enabled
        && !gtk_widget_get_visible(controls->metadata_window)
        && controls->hover_show_timeout_id == 0)
    {
        if (controls->hover_delay_ms == 0)
            show_metadata_window(controls);
        else
            controls->hover_show_timeout_id = g_timeout_add(controls->hover_delay_ms,
                                                            show_metadata_window,
                                                            controls);
    }

    return FALSE;
}

static gboolean
hover_left(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
    MprisControlsPlugin *controls = user_data;

    (void) widget;
    if (event->mode != GDK_CROSSING_NORMAL)
        return FALSE;

    cancel_hover_timeout(&controls->hover_show_timeout_id);
    cancel_hover_timeout(&controls->hover_hide_timeout_id);
    controls->hover_hide_timeout_id = g_timeout_add(200, hide_metadata_window, controls);

    return FALSE;
}

static void
set_play_pause_state(MprisControlsPlugin *controls, gboolean is_playing)
{
    if (controls == NULL || controls->play_pause_image == NULL)
        return;

    controls->is_playing = is_playing;

    if (is_playing)
    {
        gtk_image_set_from_icon_name(GTK_IMAGE(controls->play_pause_image), "media-playback-pause-symbolic", GTK_ICON_SIZE_BUTTON);
    }
    else
    {
        gtk_image_set_from_icon_name(GTK_IMAGE(controls->play_pause_image), "media-playback-start-symbolic", GTK_ICON_SIZE_BUTTON);
    }

    gtk_widget_queue_draw(controls->play_pause_image);
}

static gboolean
refresh_play_pause_state(gpointer user_data)
{
    MprisControlsPlugin *controls = user_data;
    gchar **players;
    gchar *player;
    gboolean is_playing = FALSE;

    if (controls == NULL)
        return G_SOURCE_REMOVE;

    players = get_mpris_players(controls);
    player = choose_active_player(controls, players, &is_playing);
    if (player != NULL)
    {
        store_active_player(controls, player);
        set_play_pause_state(controls, is_playing);
    }
    else
        set_play_pause_state(controls, FALSE);

    refresh_panel_track_label(controls);

    if (controls->hover_enabled && gtk_widget_get_visible(controls->metadata_window))
        refresh_metadata_window(controls);

    g_free(player);
    g_strfreev(players);

    return G_SOURCE_CONTINUE;
}

static void
update_orientation(MprisControlsPlugin *controls, XfcePanelPlugin *plugin)
{
    GtkOrientation orientation = xfce_panel_plugin_get_orientation(plugin);

    gtk_orientable_set_orientation(GTK_ORIENTABLE(controls->box), orientation);
}

static void
orientation_changed(XfcePanelPlugin *plugin, GtkOrientation orientation, MprisControlsPlugin *controls)
{
    (void) orientation;
    update_orientation(controls, plugin);
}

static gboolean
size_changed(XfcePanelPlugin *plugin, gint size, MprisControlsPlugin *controls)
{
    gint button_size = MAX(16, size - 2);

    (void) plugin;

    gtk_widget_set_size_request(controls->previous_button, button_size, button_size);
    gtk_widget_set_size_request(controls->play_pause_button, button_size, button_size);
    gtk_widget_set_size_request(controls->next_button, button_size, button_size);

    return TRUE;
}

static gboolean
key_file_get_boolean_default(GKeyFile *key_file, const gchar *key, gboolean default_value)
{
    GError *error = NULL;
    gboolean value = g_key_file_get_boolean(key_file, "Settings", key, &error);

    if (error != NULL)
    {
        g_clear_error(&error);
        return default_value;
    }

    return value;
}

static gint
key_file_get_integer_default(GKeyFile *key_file, const gchar *key, gint default_value)
{
    GError *error = NULL;
    gint value = g_key_file_get_integer(key_file, "Settings", key, &error);

    if (error != NULL)
    {
        g_clear_error(&error);
        return default_value;
    }

    return value;
}

static void
load_settings(XfcePanelPlugin *plugin, MprisControlsPlugin *controls)
{
    GKeyFile *key_file;
    gchar *filename;
    gchar *color_string;
    gint popup_position;

    controls->hover_enabled = TRUE;
    controls->show_panel_track = FALSE;
    controls->show_artwork = TRUE;
    controls->show_artist = TRUE;
    controls->show_album = TRUE;
    controls->show_previous = TRUE;
    controls->show_next = TRUE;
    controls->hover_delay_ms = 300;
    controls->panel_fixed_width = 220;
    controls->popup_position = POPUP_POSITION_AUTOMATIC;
    controls->panel_track_position = PANEL_TRACK_POSITION_AFTER;
    controls->panel_track_width = PANEL_TRACK_WIDTH_DYNAMIC;
    gdk_rgba_parse(&controls->panel_text_color, "#ffffff");
    controls->panel_text_color_custom = FALSE;
    filename = xfce_panel_plugin_lookup_rc_file(plugin);
    if (filename == NULL)
        return;

    key_file = g_key_file_new();
    if (g_key_file_load_from_file(key_file, filename, G_KEY_FILE_NONE, NULL))
    {
        controls->hover_enabled = key_file_get_boolean_default(key_file, "hover-enabled", TRUE);
        controls->show_panel_track = key_file_get_boolean_default(key_file, "show-panel-track", FALSE);
        controls->show_artwork = key_file_get_boolean_default(key_file, "show-artwork", TRUE);
        controls->show_artist = key_file_get_boolean_default(key_file, "show-artist", TRUE);
        controls->show_album = key_file_get_boolean_default(key_file, "show-album", TRUE);
        controls->show_previous = key_file_get_boolean_default(key_file, "show-previous", TRUE);
        controls->show_next = key_file_get_boolean_default(key_file, "show-next", TRUE);
        controls->hover_delay_ms = CLAMP(key_file_get_integer_default(key_file, "hover-delay-ms", 300), 0, 2000);
        if (controls->hover_delay_ms != 0)
            controls->hover_delay_ms = controls->hover_delay_ms >= 600 ? 700 : 300;
        popup_position = key_file_get_integer_default(key_file, "popup-position", POPUP_POSITION_AUTOMATIC);
        if (popup_position >= POPUP_POSITION_AUTOMATIC && popup_position <= POPUP_POSITION_BELOW)
            controls->popup_position = popup_position;

        popup_position = key_file_get_integer_default(key_file, "panel-track-position", PANEL_TRACK_POSITION_AFTER);
        if (popup_position >= PANEL_TRACK_POSITION_BEFORE && popup_position <= PANEL_TRACK_POSITION_AFTER)
            controls->panel_track_position = popup_position;

        popup_position = key_file_get_integer_default(key_file, "panel-track-width", PANEL_TRACK_WIDTH_DYNAMIC);
        if (popup_position >= PANEL_TRACK_WIDTH_DYNAMIC && popup_position <= PANEL_TRACK_WIDTH_FIXED)
            controls->panel_track_width = popup_position;

        controls->panel_fixed_width = CLAMP(key_file_get_integer_default(key_file, "panel-fixed-width", 220), 100, 600);
        controls->panel_text_color_custom = key_file_get_boolean_default(key_file,
                                                                          "panel-text-color-custom",
                                                                          FALSE);
        color_string = g_key_file_get_string(key_file, "Settings", "panel-text-color", NULL);
        if (color_string != NULL)
        {
            gdk_rgba_parse(&controls->panel_text_color, color_string);
            g_free(color_string);
        }
    }

    g_key_file_unref(key_file);
    g_free(filename);
}

static void
save_settings(XfcePanelPlugin *plugin, MprisControlsPlugin *controls)
{
    GKeyFile *key_file;
    gchar *filename;
    gchar *color_string;
    gchar *data;
    gsize length;
    GError *error = NULL;

    filename = xfce_panel_plugin_save_location(plugin, TRUE);
    if (filename == NULL)
        return;

    key_file = g_key_file_new();
    g_key_file_set_boolean(key_file, "Settings", "hover-enabled", controls->hover_enabled);
    g_key_file_set_boolean(key_file, "Settings", "show-panel-track", controls->show_panel_track);
    g_key_file_set_boolean(key_file, "Settings", "show-artwork", controls->show_artwork);
    g_key_file_set_boolean(key_file, "Settings", "show-artist", controls->show_artist);
    g_key_file_set_boolean(key_file, "Settings", "show-album", controls->show_album);
    g_key_file_set_boolean(key_file, "Settings", "show-previous", controls->show_previous);
    g_key_file_set_boolean(key_file, "Settings", "show-next", controls->show_next);
    g_key_file_set_integer(key_file, "Settings", "hover-delay-ms", controls->hover_delay_ms);
    g_key_file_set_integer(key_file, "Settings", "popup-position", controls->popup_position);
    g_key_file_set_integer(key_file, "Settings", "panel-track-position", controls->panel_track_position);
    g_key_file_set_integer(key_file, "Settings", "panel-track-width", controls->panel_track_width);
    g_key_file_set_integer(key_file, "Settings", "panel-fixed-width", controls->panel_fixed_width);
    g_key_file_set_boolean(key_file,
                           "Settings",
                           "panel-text-color-custom",
                           controls->panel_text_color_custom);
    color_string = gdk_rgba_to_string(&controls->panel_text_color);
    g_key_file_set_string(key_file, "Settings", "panel-text-color", color_string);
    g_free(color_string);
    data = g_key_file_to_data(key_file, &length, NULL);

    if (!g_file_set_contents(filename, data, length, &error))
    {
        g_warning("Unable to save MPRIS Controls settings: %s", error->message);
        g_clear_error(&error);
    }

    g_free(data);
    g_key_file_unref(key_file);
    g_free(filename);
}

static void
apply_settings(MprisControlsPlugin *controls)
{
    gtk_widget_set_visible(controls->panel_track_container, controls->show_panel_track);
    gtk_widget_set_size_request(controls->panel_track_container,
                                controls->panel_track_width == PANEL_TRACK_WIDTH_FIXED
                                    ? (gint) controls->panel_fixed_width
                                    : -1,
                                -1);
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(controls->panel_track_container),
                                              controls->panel_track_width == PANEL_TRACK_WIDTH_FIXED
                                                  ? (gint) controls->panel_fixed_width
                                                  : -1);
    gtk_scrolled_window_set_max_content_width(GTK_SCROLLED_WINDOW(controls->panel_track_container),
                                              controls->panel_track_width == PANEL_TRACK_WIDTH_FIXED
                                                  ? (gint) controls->panel_fixed_width
                                                  : -1);
    gtk_box_reorder_child(GTK_BOX(controls->box),
                          controls->panel_track_container,
                          controls->panel_track_position == PANEL_TRACK_POSITION_BEFORE ? 0 : -1);
    update_panel_track_display(controls);

    gtk_widget_set_visible(controls->previous_button, controls->show_previous);
    gtk_widget_set_visible(controls->next_button, controls->show_next);

    if (!controls->hover_enabled)
    {
        cancel_hover_timeout(&controls->hover_show_timeout_id);
        cancel_hover_timeout(&controls->hover_hide_timeout_id);
        gtk_widget_hide(controls->metadata_window);
    }

    if (gtk_widget_get_visible(controls->metadata_window))
    {
        refresh_metadata_window(controls);
        position_metadata_window(controls);
    }
}

static void
setting_toggled(GtkToggleButton *button, gpointer user_data)
{
    MprisControlsPlugin *controls = user_data;
    const gchar *setting = g_object_get_data(G_OBJECT(button), "setting");
    gboolean active = gtk_toggle_button_get_active(button);

    if (g_strcmp0(setting, "hover") == 0)
        controls->hover_enabled = active;
    else if (g_strcmp0(setting, "panel-track") == 0)
        controls->show_panel_track = active;
    else if (g_strcmp0(setting, "panel-theme-color") == 0)
    {
        GtkWidget *color_button = g_object_get_data(G_OBJECT(button), "color-button");

        controls->panel_text_color_custom = !active;
        if (color_button != NULL)
        {
            if (!active)
                gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(color_button),
                                           &controls->panel_text_color);
            gtk_widget_set_sensitive(color_button, !active);
        }
    }
    else if (g_strcmp0(setting, "artwork") == 0)
        controls->show_artwork = active;
    else if (g_strcmp0(setting, "artist") == 0)
        controls->show_artist = active;
    else if (g_strcmp0(setting, "album") == 0)
        controls->show_album = active;
    else if (g_strcmp0(setting, "previous") == 0)
        controls->show_previous = active;
    else if (g_strcmp0(setting, "next") == 0)
        controls->show_next = active;

    apply_settings(controls);
    save_settings(controls->plugin, controls);
}

static GtkWidget *
create_setting_check(const gchar *label,
                     const gchar *setting,
                     gboolean active,
                     MprisControlsPlugin *controls)
{
    GtkWidget *button = gtk_check_button_new_with_label(label);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), active);
    g_object_set_data(G_OBJECT(button), "setting", (gpointer) setting);
    g_signal_connect(button, "toggled", G_CALLBACK(setting_toggled), controls);

    return button;
}

static void
hover_delay_changed(GtkComboBox *combo, gpointer user_data)
{
    MprisControlsPlugin *controls = user_data;
    const gchar *active_id = gtk_combo_box_get_active_id(combo);

    if (active_id != NULL)
        controls->hover_delay_ms = (guint) g_ascii_strtoull(active_id, NULL, 10);

    save_settings(controls->plugin, controls);
}

static void
popup_position_changed(GtkComboBox *combo, gpointer user_data)
{
    MprisControlsPlugin *controls = user_data;
    const gchar *active_id = gtk_combo_box_get_active_id(combo);

    if (g_strcmp0(active_id, "above") == 0)
        controls->popup_position = POPUP_POSITION_ABOVE;
    else if (g_strcmp0(active_id, "below") == 0)
        controls->popup_position = POPUP_POSITION_BELOW;
    else
        controls->popup_position = POPUP_POSITION_AUTOMATIC;

    apply_settings(controls);
    save_settings(controls->plugin, controls);
}

static void
panel_track_position_changed(GtkComboBox *combo, gpointer user_data)
{
    MprisControlsPlugin *controls = user_data;
    const gchar *active_id = gtk_combo_box_get_active_id(combo);

    controls->panel_track_position = g_strcmp0(active_id, "before") == 0
                                         ? PANEL_TRACK_POSITION_BEFORE
                                         : PANEL_TRACK_POSITION_AFTER;
    apply_settings(controls);
    save_settings(controls->plugin, controls);
}

static void
panel_track_width_changed(GtkComboBox *combo, gpointer user_data)
{
    MprisControlsPlugin *controls = user_data;
    GtkWidget *fixed_width_spin;
    const gchar *active_id = gtk_combo_box_get_active_id(combo);

    controls->panel_track_width = g_strcmp0(active_id, "fixed") == 0
                                      ? PANEL_TRACK_WIDTH_FIXED
                                      : PANEL_TRACK_WIDTH_DYNAMIC;
    fixed_width_spin = g_object_get_data(G_OBJECT(combo), "fixed-width-spin");
    if (fixed_width_spin != NULL)
    {
        gtk_widget_set_sensitive(fixed_width_spin,
                                 controls->panel_track_width == PANEL_TRACK_WIDTH_FIXED);
    }

    apply_settings(controls);
    save_settings(controls->plugin, controls);
}

static void
panel_fixed_width_changed(GtkSpinButton *spin, gpointer user_data)
{
    MprisControlsPlugin *controls = user_data;

    controls->panel_fixed_width = (guint) gtk_spin_button_get_value_as_int(spin);
    apply_settings(controls);
    save_settings(controls->plugin, controls);
}

static void
panel_text_color_changed(GtkColorButton *color_button, gpointer user_data)
{
    MprisControlsPlugin *controls = user_data;
    GtkWidget *theme_color_check;

    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(color_button), &controls->panel_text_color);
    controls->panel_text_color_custom = TRUE;
    theme_color_check = g_object_get_data(G_OBJECT(color_button), "theme-color-check");
    if (theme_color_check != NULL)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(theme_color_check), FALSE);

    update_panel_track_display(controls);
    save_settings(controls->plugin, controls);
}

static void
attach_section_label(GtkGrid *grid, const gchar *text, gint row)
{
    GtkWidget *label = gtk_label_new(NULL);
    gchar *markup = g_markup_printf_escaped("<b>%s</b>", text);

    gtk_label_set_markup(GTK_LABEL(label), markup);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_margin_top(label, row == 0 ? 0 : 8);
    gtk_grid_attach(grid, label, 0, row, 2, 1);
    g_free(markup);
}

static void
attach_setting_label(GtkGrid *grid, const gchar *text, gint row)
{
    GtkWidget *label = gtk_label_new(text);

    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_grid_attach(grid, label, 0, row, 1, 1);
}

static const gchar *
hover_delay_id(guint hover_delay_ms)
{
    if (hover_delay_ms == 0)
        return "0";
    if (hover_delay_ms >= 600)
        return "700";

    return "300";
}

static const gchar *
popup_position_id(PopupPosition position)
{
    if (position == POPUP_POSITION_ABOVE)
        return "above";
    if (position == POPUP_POSITION_BELOW)
        return "below";

    return "automatic";
}

static const gchar *
panel_track_position_id(PanelTrackPosition position)
{
    return position == PANEL_TRACK_POSITION_BEFORE ? "before" : "after";
}

static const gchar *
panel_track_width_id(PanelTrackWidth width)
{
    return width == PANEL_TRACK_WIDTH_FIXED ? "fixed" : "dynamic";
}

static void
dismiss_metadata_window(MprisControlsPlugin *controls)
{
    cancel_hover_timeout(&controls->hover_show_timeout_id);
    cancel_hover_timeout(&controls->hover_hide_timeout_id);
    stop_panel_track_scroll(controls);
    gtk_widget_hide(controls->metadata_window);
}

static void
configure_plugin(XfcePanelPlugin *plugin, MprisControlsPlugin *controls)
{
    GtkWidget *dialog;
    GtkWidget *content;
    GtkWidget *grid;
    GtkWidget *widget;
    GtkWidget *hover_delay_combo;
    GtkWidget *position_combo;
    GtkWidget *panel_track_position_combo;
    GtkWidget *panel_track_width_combo;
    GtkWidget *panel_fixed_width_spin;
    GtkWidget *panel_theme_color_check;
    GtkWidget *panel_text_color_button;
    GtkWidget *parent;
    GdkRGBA theme_color;
    gint row = 0;

    dismiss_metadata_window(controls);

    parent = gtk_widget_get_toplevel(GTK_WIDGET(plugin));
    dialog = gtk_dialog_new_with_buttons("MPRIS Controls",
                                         GTK_IS_WINDOW(parent) ? GTK_WINDOW(parent) : NULL,
                                         GTK_DIALOG_DESTROY_WITH_PARENT,
                                         "_Close",
                                         GTK_RESPONSE_CLOSE,
                                         NULL);
    gtk_window_set_icon_name(GTK_WINDOW(dialog), "multimedia-player");
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ALWAYS);

    content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_box_pack_start(GTK_BOX(content), grid, TRUE, TRUE, 0);

    attach_section_label(GTK_GRID(grid), "Track information", row++);
    widget = create_setting_check("Show on hover", "hover", controls->hover_enabled, controls);
    gtk_grid_attach(GTK_GRID(grid), widget, 0, row++, 2, 1);
    widget = create_setting_check("Show cover art", "artwork", controls->show_artwork, controls);
    gtk_grid_attach(GTK_GRID(grid), widget, 0, row++, 2, 1);
    widget = create_setting_check("Show artist", "artist", controls->show_artist, controls);
    gtk_grid_attach(GTK_GRID(grid), widget, 0, row++, 2, 1);
    widget = create_setting_check("Show album", "album", controls->show_album, controls);
    gtk_grid_attach(GTK_GRID(grid), widget, 0, row++, 2, 1);

    attach_setting_label(GTK_GRID(grid), "Hover delay", row);
    hover_delay_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(hover_delay_combo), "0", "Instant");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(hover_delay_combo), "300", "Short");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(hover_delay_combo), "700", "Long");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(hover_delay_combo), hover_delay_id(controls->hover_delay_ms));
    gtk_grid_attach(GTK_GRID(grid), hover_delay_combo, 1, row++, 1, 1);

    attach_setting_label(GTK_GRID(grid), "Popup position", row);
    position_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(position_combo), "automatic", "Automatic");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(position_combo), "above", "Above panel");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(position_combo), "below", "Below panel");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(position_combo), popup_position_id(controls->popup_position));
    gtk_grid_attach(GTK_GRID(grid), position_combo, 1, row++, 1, 1);

    attach_section_label(GTK_GRID(grid), "Panel track label", row++);
    widget = create_setting_check("Show track label", "panel-track", controls->show_panel_track, controls);
    gtk_grid_attach(GTK_GRID(grid), widget, 0, row++, 2, 1);

    attach_setting_label(GTK_GRID(grid), "Position", row);
    panel_track_position_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(panel_track_position_combo), "before", "Before controls");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(panel_track_position_combo), "after", "After controls");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(panel_track_position_combo),
                                panel_track_position_id(controls->panel_track_position));
    gtk_grid_attach(GTK_GRID(grid), panel_track_position_combo, 1, row++, 1, 1);

    attach_setting_label(GTK_GRID(grid), "Width", row);
    panel_track_width_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(panel_track_width_combo), "dynamic", "Dynamic");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(panel_track_width_combo), "fixed", "Fixed with scrolling");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(panel_track_width_combo),
                                panel_track_width_id(controls->panel_track_width));
    gtk_grid_attach(GTK_GRID(grid), panel_track_width_combo, 1, row++, 1, 1);

    attach_setting_label(GTK_GRID(grid), "Fixed width (px)", row);
    panel_fixed_width_spin = gtk_spin_button_new_with_range(100, 600, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(panel_fixed_width_spin), controls->panel_fixed_width);
    gtk_widget_set_sensitive(panel_fixed_width_spin,
                             controls->panel_track_width == PANEL_TRACK_WIDTH_FIXED);
    gtk_grid_attach(GTK_GRID(grid), panel_fixed_width_spin, 1, row++, 1, 1);

    panel_theme_color_check = create_setting_check("Use theme text color",
                                                   "panel-theme-color",
                                                   !controls->panel_text_color_custom,
                                                   controls);
    gtk_grid_attach(GTK_GRID(grid), panel_theme_color_check, 0, row++, 2, 1);

    attach_setting_label(GTK_GRID(grid), "Custom text color", row);
    panel_text_color_button = gtk_color_button_new();
    gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(panel_text_color_button), FALSE);
    gtk_style_context_get_color(gtk_widget_get_style_context(controls->panel_track_label),
                                GTK_STATE_FLAG_NORMAL,
                                &theme_color);
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(panel_text_color_button),
                               controls->panel_text_color_custom
                                   ? &controls->panel_text_color
                                   : &theme_color);
    gtk_widget_set_sensitive(panel_text_color_button, controls->panel_text_color_custom);
    gtk_grid_attach(GTK_GRID(grid), panel_text_color_button, 1, row++, 1, 1);
    g_object_set_data(G_OBJECT(panel_theme_color_check), "color-button", panel_text_color_button);
    g_object_set_data(G_OBJECT(panel_text_color_button), "theme-color-check", panel_theme_color_check);

    attach_section_label(GTK_GRID(grid), "Panel controls", row++);
    widget = create_setting_check("Show previous button", "previous", controls->show_previous, controls);
    gtk_grid_attach(GTK_GRID(grid), widget, 0, row++, 2, 1);
    widget = create_setting_check("Show next button", "next", controls->show_next, controls);
    gtk_grid_attach(GTK_GRID(grid), widget, 0, row++, 2, 1);

    g_signal_connect(hover_delay_combo, "changed", G_CALLBACK(hover_delay_changed), controls);
    g_signal_connect(position_combo, "changed", G_CALLBACK(popup_position_changed), controls);
    g_signal_connect(panel_track_position_combo,
                     "changed",
                     G_CALLBACK(panel_track_position_changed),
                     controls);
    g_object_set_data(G_OBJECT(panel_track_width_combo), "fixed-width-spin", panel_fixed_width_spin);
    g_signal_connect(panel_track_width_combo,
                     "changed",
                     G_CALLBACK(panel_track_width_changed),
                     controls);
    g_signal_connect(panel_fixed_width_spin,
                     "value-changed",
                     G_CALLBACK(panel_fixed_width_changed),
                     controls);
    g_signal_connect(panel_text_color_button,
                     "color-set",
                     G_CALLBACK(panel_text_color_changed),
                     controls);
    g_signal_connect_swapped(dialog, "response", G_CALLBACK(gtk_widget_destroy), dialog);

    gtk_widget_show_all(dialog);
}

static void
free_controls(gpointer data)
{
    MprisControlsPlugin *controls = data;

    if (controls->status_timeout_id != 0)
        g_source_remove(controls->status_timeout_id);

    cancel_hover_timeout(&controls->hover_show_timeout_id);
    cancel_hover_timeout(&controls->hover_hide_timeout_id);
    stop_panel_track_scroll(controls);

    if (controls->metadata_window != NULL)
    {
        gtk_widget_destroy(controls->metadata_window);
        g_object_unref(controls->metadata_window);
    }

    if (controls->artwork_cancellable != NULL)
        g_cancellable_cancel(controls->artwork_cancellable);
    g_clear_object(&controls->artwork_cancellable);

    if (controls->dbus_connection != NULL)
        g_object_unref(controls->dbus_connection);

    g_free(controls->artwork_url);
    g_free(controls->panel_track_text);
    g_free(controls->active_player);
    g_free(controls);
}

static void
mpris_controls_construct(XfcePanelPlugin *plugin)
{
    MprisControlsPlugin *controls = g_new0(MprisControlsPlugin, 1);
    GError *error = NULL;

    xfce_panel_plugin_set_small(plugin, TRUE);
    gtk_widget_set_name(GTK_WIDGET(plugin), "mpris-controls-plugin");
    controls->plugin = plugin;
    load_settings(plugin, controls);

    controls->dbus_connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (controls->dbus_connection == NULL)
    {
        g_warning("Unable to connect to the session D-Bus: %s", error->message);
        g_clear_error(&error);
    }

    controls->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    controls->previous_button = create_icon_button("media-skip-backward-symbolic", "Previous track", G_CALLBACK(previous_clicked), controls, NULL);
    controls->play_pause_button = create_icon_button("media-playback-start-symbolic", "Play", G_CALLBACK(play_pause_clicked), controls, &controls->play_pause_image);
    controls->next_button = create_icon_button("media-skip-forward-symbolic", "Next track", G_CALLBACK(next_clicked), controls, NULL);
    controls->panel_track_label = gtk_drawing_area_new();
    controls->panel_track_container = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_name(controls->panel_track_label, "mpris-controls-panel-track");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(controls->panel_track_container),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_NEVER);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(controls->panel_track_container),
                                        GTK_SHADOW_NONE);
    gtk_container_add(GTK_CONTAINER(controls->panel_track_container), controls->panel_track_label);
    g_signal_connect(controls->panel_track_label, "draw", G_CALLBACK(panel_track_draw), controls);
    gtk_widget_set_tooltip_text(controls->play_pause_button, NULL);

    controls->metadata_window = g_object_ref_sink(gtk_window_new(GTK_WINDOW_POPUP));
    controls->artwork_image = gtk_image_new();
    controls->title_label = gtk_label_new(NULL);
    controls->artist_label = gtk_label_new(NULL);
    controls->album_label = gtk_label_new(NULL);

    {
        GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
        GtkStyleContext *style_context;

        gtk_window_set_decorated(GTK_WINDOW(controls->metadata_window), FALSE);
        gtk_window_set_resizable(GTK_WINDOW(controls->metadata_window), FALSE);
        gtk_window_set_accept_focus(GTK_WINDOW(controls->metadata_window), FALSE);
        gtk_window_set_skip_taskbar_hint(GTK_WINDOW(controls->metadata_window), TRUE);
        gtk_window_set_skip_pager_hint(GTK_WINDOW(controls->metadata_window), TRUE);
        gtk_window_set_type_hint(GTK_WINDOW(controls->metadata_window), GDK_WINDOW_TYPE_HINT_TOOLTIP);
        gtk_widget_set_name(controls->metadata_window, "mpris-controls-track-info");
        style_context = gtk_widget_get_style_context(controls->metadata_window);
        gtk_style_context_add_class(style_context, GTK_STYLE_CLASS_TOOLTIP);

        gtk_container_set_border_width(GTK_CONTAINER(content), 8);
        gtk_widget_set_size_request(controls->artwork_image, 80, 80);
        gtk_widget_set_size_request(text, 220, -1);
        set_artwork_fallback(controls);
        gtk_label_set_xalign(GTK_LABEL(controls->title_label), 0.0f);
        gtk_label_set_xalign(GTK_LABEL(controls->artist_label), 0.0f);
        gtk_label_set_xalign(GTK_LABEL(controls->album_label), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(controls->title_label), PANGO_ELLIPSIZE_END);
        gtk_label_set_ellipsize(GTK_LABEL(controls->artist_label), PANGO_ELLIPSIZE_END);
        gtk_label_set_ellipsize(GTK_LABEL(controls->album_label), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(controls->title_label), 32);
        gtk_label_set_max_width_chars(GTK_LABEL(controls->artist_label), 32);
        gtk_label_set_max_width_chars(GTK_LABEL(controls->album_label), 32);

        style_context = gtk_widget_get_style_context(controls->title_label);
        gtk_style_context_add_class(style_context, "title");
        style_context = gtk_widget_get_style_context(controls->artist_label);
        gtk_style_context_add_class(style_context, "dim-label");
        style_context = gtk_widget_get_style_context(controls->album_label);
        gtk_style_context_add_class(style_context, "dim-label");

        gtk_box_pack_start(GTK_BOX(text), controls->title_label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(text), controls->artist_label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(text), controls->album_label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(content), controls->artwork_image, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(content), text, TRUE, TRUE, 0);
        gtk_container_add(GTK_CONTAINER(controls->metadata_window), content);
        gtk_widget_show_all(content);
    }

    gtk_box_pack_start(GTK_BOX(controls->box), controls->previous_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls->box), controls->play_pause_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls->box), controls->next_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls->box), controls->panel_track_container, FALSE, FALSE, 0);

    update_orientation(controls, plugin);

    gtk_container_add(GTK_CONTAINER(plugin), controls->box);
    gtk_widget_show_all(GTK_WIDGET(plugin));

    g_object_set_data_full(G_OBJECT(plugin), "mpris-controls", controls, free_controls);

    g_signal_connect(plugin, "orientation-changed", G_CALLBACK(orientation_changed), controls);
    g_signal_connect(plugin, "size-changed", G_CALLBACK(size_changed), controls);
    g_signal_connect(plugin, "save", G_CALLBACK(save_settings), controls);
    g_signal_connect(plugin, "configure-plugin", G_CALLBACK(configure_plugin), controls);
    g_signal_connect(controls->play_pause_button, "enter-notify-event", G_CALLBACK(hover_entered), controls);
    g_signal_connect(controls->play_pause_button, "leave-notify-event", G_CALLBACK(hover_left), controls);
    gtk_widget_add_events(controls->metadata_window, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(controls->metadata_window, "enter-notify-event", G_CALLBACK(hover_entered), controls);
    g_signal_connect(controls->metadata_window, "leave-notify-event", G_CALLBACK(hover_left), controls);
    xfce_panel_plugin_menu_show_configure(plugin);

    apply_settings(controls);
    size_changed(plugin, xfce_panel_plugin_get_size(plugin), controls);
    set_play_pause_state(controls, FALSE);
    refresh_play_pause_state(controls);
    controls->status_timeout_id = g_timeout_add_seconds(2, refresh_play_pause_state, controls);
}

XFCE_PANEL_PLUGIN_REGISTER(mpris_controls_construct)
