#include <gio/gio.h>
#include <string.h>
static int song;
static gboolean playing=TRUE;
static guint64 position=42000000;
static const char xml[]=
 "<node><interface name='org.mpris.MediaPlayer2.Player'>"
 "<method name='Next'/><method name='Previous'/><method name='PlayPause'/><method name='Seek'><arg name='Offset' type='x' direction='in'/></method><method name='SetPosition'><arg name='TrackId' type='o' direction='in'/><arg name='Position' type='x' direction='in'/></method>"
 "<property name='Metadata' type='a{sv}' access='read'/>"
 "<property name='PlaybackStatus' type='s' access='read'/>"
 "<property name='Position' type='x' access='read'/>"
 "<property name='CanControl' type='b' access='read'/>"
 "<property name='CanGoNext' type='b' access='read'/>"
 "<property name='CanGoPrevious' type='b' access='read'/>"
 "<property name='CanPlay' type='b' access='read'/>"
 "<property name='CanPause' type='b' access='read'/>"
 "<property name='CanSeek' type='b' access='read'/>"
 "</interface></node>";
static GVariant *property(GDBusConnection *c,const char *sender,const char *path,const char *iface,const char *name,GError **e,void *data) {
    (void)c;(void)sender;(void)path;(void)iface;(void)e;(void)data;
    if (!strcmp(name,"PlaybackStatus")) return g_variant_new_string(playing?"Playing":"Paused");
    if (!strcmp(name,"Position")) return g_variant_new_int64(position);
    if (!strcmp(name,"Metadata")) {
        GVariantBuilder b; g_variant_builder_init(&b,G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&b,"{sv}","xesam:title",g_variant_new_string(song?"Song B":"Song A"));
        const char *artists[]={"Test Artist",NULL};
        g_variant_builder_add(&b,"{sv}","xesam:artist",g_variant_new_strv(artists,-1));
        g_variant_builder_add(&b,"{sv}","xesam:album",g_variant_new_string("Test Album"));
        g_variant_builder_add(&b,"{sv}","mpris:length",g_variant_new_int64(180000000));
        g_variant_builder_add(&b,"{sv}","mpris:trackid",g_variant_new("o",song?"/org/mpris/MediaPlayer2/track/1":"/org/mpris/MediaPlayer2/track/0"));
        return g_variant_builder_end(&b);
    }
    return g_variant_new_boolean(TRUE);
}
static void method(GDBusConnection *c,const char *sender,const char *path,const char *iface,const char *name,GVariant *args,GDBusMethodInvocation *inv,void *data) {
    (void)c;(void)sender;(void)path;(void)iface;(void)data;
    if (!strcmp(name,"Next")) song=1;
    else if (!strcmp(name,"Previous")) song=0;
    else if (!strcmp(name,"PlayPause")) playing=!playing;
    else if (!strcmp(name,"Seek")) {
        gint64 offset=0; g_variant_get_child(args,0,"x",&offset);
        gint64 next=(gint64)position+offset;
        if (next<0) next=0;
        if (next>180000000) next=180000000;
        position=(guint64)next;
    } else if (!strcmp(name,"SetPosition")) {
        const char *track=NULL; guint64 us=0;
        g_variant_get_child(args,0,"o",&track); g_variant_get_child(args,1,"x",&us);
        if (strcmp(track,song?"/org/mpris/MediaPlayer2/track/1":"/org/mpris/MediaPlayer2/track/0")) {
            g_dbus_method_invocation_return_error(inv,g_dbus_error_quark(),G_DBUS_ERROR_INVALID_ARGS,"Track id does not match the current track"); return;
        }
        position=us;
    }
    GVariantBuilder changed, invalidated;
    g_variant_builder_init(&changed,G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_init(&invalidated,G_VARIANT_TYPE("as"));
    g_variant_builder_add(&changed,"{sv}","PlaybackStatus",g_variant_new_string(playing?"Playing":"Paused"));
    g_variant_builder_add(&changed,"{sv}","Metadata",property(c,sender,path,iface,"Metadata",NULL,NULL));
    g_dbus_connection_emit_signal(c,NULL,path,"org.freedesktop.DBus.Properties","PropertiesChanged",
        g_variant_new("(sa{sv}as)",iface,&changed,&invalidated),NULL);
    if (!strcmp(name,"Seek") || !strcmp(name,"SetPosition"))
        g_dbus_connection_emit_signal(c,NULL,path,iface,"Seeked",g_variant_new("(x)",(gint64)position),NULL);
    g_dbus_method_invocation_return_value(inv,NULL);
}
static const GDBusInterfaceVTable vtable={.method_call=method,.get_property=property};
int main(void) {
    GDBusConnection *bus=g_bus_get_sync(G_BUS_TYPE_SESSION,NULL,NULL); if (!bus) return 1;
    GDBusNodeInfo *info=g_dbus_node_info_new_for_xml(xml,NULL);
    if (!info) return 1;
    if (!g_dbus_connection_register_object(bus,"/org/mpris/MediaPlayer2",info->interfaces[0],&vtable,NULL,NULL,NULL)) return 1;
    g_bus_own_name_on_connection(bus,"org.mpris.MediaPlayer2.eqweb_test",G_BUS_NAME_OWNER_FLAGS_NONE,NULL,NULL,NULL,NULL);
    GMainLoop *loop=g_main_loop_new(NULL,FALSE); g_main_loop_run(loop); return 0;
}
