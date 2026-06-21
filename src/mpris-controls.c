#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>

typedef struct
{
    GtkWidget *box;
    GtkWidget *previous_button;
    GtkWidget *play_pause_button;
    GtkWidget *play_pause_image;
    GtkWidget *next_button;
    GDBusConnection *dbus_connection;
    gchar *active_player;
    guint status_timeout_id;
    gboolean is_playing;
} MprisControlsPlugin;

static void set_play_pause_state(MprisControlsPlugin *controls, gboolean is_playing);
static gboolean refresh_play_pause_state(gpointer user_data);

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

    for (i = 0; players[i] != NULL; i++)
    {
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

static void
call_mpris_player_method(MprisControlsPlugin *controls, const gchar *method)
{
    gchar **players;
    gchar *player;
    gboolean is_playing = FALSE;
    GError *error = NULL;

    if (controls->dbus_connection == NULL)
        return;

    players = get_mpris_players(controls);
    player = choose_active_player(controls, players, &is_playing);
    if (player == NULL)
    {
        g_strfreev(players);
        return;
    }

    store_active_player(controls, player);
    g_dbus_connection_call_sync(controls->dbus_connection,
                                player,
                                "/org/mpris/MediaPlayer2",
                                "org.mpris.MediaPlayer2.Player",
                                method,
                                NULL,
                                NULL,
                                G_DBUS_CALL_FLAGS_NONE,
                                1000,
                                NULL,
                                &error);
    if (error != NULL)
        g_clear_error(&error);

    g_free(player);
    g_strfreev(players);
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

    call_mpris_player_method(user_data, "PlayPause");
    set_play_pause_state(user_data, !((MprisControlsPlugin *) user_data)->is_playing);
}

static void
next_clicked(GtkButton *button, gpointer user_data)
{
    (void) button;

    call_mpris_player_method(user_data, "Next");
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
        gtk_widget_set_tooltip_text(controls->play_pause_button, "Pause");
    }
    else
    {
        gtk_image_set_from_icon_name(GTK_IMAGE(controls->play_pause_image), "media-playback-start-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_widget_set_tooltip_text(controls->play_pause_button, "Play");
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

static void
free_controls(gpointer data)
{
    MprisControlsPlugin *controls = data;

    if (controls->status_timeout_id != 0)
        g_source_remove(controls->status_timeout_id);

    if (controls->dbus_connection != NULL)
        g_object_unref(controls->dbus_connection);

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

    gtk_box_pack_start(GTK_BOX(controls->box), controls->previous_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls->box), controls->play_pause_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls->box), controls->next_button, FALSE, FALSE, 0);

    update_orientation(controls, plugin);

    gtk_container_add(GTK_CONTAINER(plugin), controls->box);
    gtk_widget_show_all(GTK_WIDGET(plugin));

    g_object_set_data_full(G_OBJECT(plugin), "mpris-controls", controls, free_controls);

    g_signal_connect(plugin, "orientation-changed", G_CALLBACK(orientation_changed), controls);
    g_signal_connect(plugin, "size-changed", G_CALLBACK(size_changed), controls);

    size_changed(plugin, xfce_panel_plugin_get_size(plugin), controls);
    set_play_pause_state(controls, FALSE);
    refresh_play_pause_state(controls);
    controls->status_timeout_id = g_timeout_add_seconds(2, refresh_play_pause_state, controls);
}

XFCE_PANEL_PLUGIN_REGISTER(mpris_controls_construct)
