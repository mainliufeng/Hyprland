#include <gtk/gtk.h>
#include <gdk/gdkwayland.h>
#include <stdio.h>

static void changed(GtkEditable* editable, gpointer data) {
    FILE* output = fopen(data, "w");
    if (output) { fputs(gtk_entry_get_text(GTK_ENTRY(editable)), output); fclose(output); }
}
int main(int argc, char** argv) {
    if (argc != 4) return 2;
    gtk_init(&argc, &argv);
    GtkWidget* dialog = gtk_file_chooser_dialog_new("route-gtk-chooser", NULL, GTK_FILE_CHOOSER_ACTION_OPEN,
                                                   "Cancel", GTK_RESPONSE_CANCEL, "Open", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), argv[2]);
    GtkWidget* entry = gtk_entry_new();
    gtk_file_chooser_set_extra_widget(GTK_FILE_CHOOSER(dialog), entry);
    g_signal_connect(entry, "changed", G_CALLBACK(changed), argv[3]);
    gtk_widget_realize(dialog);
    if (!gdk_wayland_window_set_transient_for_exported(gtk_widget_get_window(dialog), argv[1])) return 3;
    gtk_widget_show_all(dialog);
    gtk_widget_grab_focus(entry);
    g_signal_connect(dialog, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    gtk_main();
    return 0;
}
