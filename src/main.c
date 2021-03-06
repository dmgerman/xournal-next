/*
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This software is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <sys/stat.h>
#include <string.h>
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <libgnomecanvas/libgnomecanvas.h>
#include <assert.h>

#include "xournal.h"
#include "xo-mru.h"
#include "xo-intl.h"
#include "xo-callbacks.h"
#include "xo-misc.h"
#include "xo-file.h"
#include "xo-paint.h"
#include "xo-shapes.h"

GtkWidget *winMain;
GnomeCanvas *canvas;
GtkBuilder *builder;

//--------------------
// command line options

typedef struct command_line_options {
    gint openAtPageNumber;
    gboolean screenshot;
    gboolean noNextSplash;
    int fileCount;
    char **fileArguments;
} command_line_options;



//--------------------



struct Journal journal; // the journal
struct BgPdf bgpdf;  // the PDF loader stuff
struct UIData ui;   // the user interface data
struct UndoItem *undo, *redo; // the undo and redo stacks

double DEFAULT_ZOOM;

void init_stuff (command_line_options *clOptions)
{
  GtkWidget *w;
  GList *dev_list;
  GdkDevice *device;
  GdkScreen *screen;
  int i, j;
  struct Brush *b;
  gboolean can_xinput, success;
  gchar *tmppath, *tmpfn;
  char *filename;

  // use only first filename, ignore the rest
  filename = (clOptions->fileCount > 0)? clOptions->fileArguments[0] : NULL;


#ifndef GDK_WINDOWING_X11
  if (clOptions->screenshot) {
      w = gtk_message_dialog_new(GTK_WINDOW (winMain), GTK_DIALOG_DESTROY_WITH_PARENT,
                                 GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, _("--screenshot option is only supported in X11. Ignoring option"));
      wrapper_gtk_dialog_run(GTK_DIALOG(w));
      gtk_widget_destroy(w);
      clOptions->screenshot = FALSE;
  }
#endif


  // check that screenshot and filename options  are not both given
  if (clOptions->screenshot && filename) {
      w = gtk_message_dialog_new(GTK_WINDOW (winMain), GTK_DIALOG_DESTROY_WITH_PARENT,
                                 GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, _("--screenshot option can not be used at the same time as filename. Ignoring option"));
      wrapper_gtk_dialog_run(GTK_DIALOG(w));
      gtk_widget_destroy(w);
      clOptions->screenshot = FALSE;
  }


  // create some data structures needed to populate the preferences
  ui.default_page.bg = g_new(struct Background, 1);

  // initialize config file names
  tmppath = g_build_filename(g_get_home_dir(), CONFIG_DIR, NULL);
  g_mkdir(tmppath, 0700); // safer (MRU data may be confidential)
  ui.mrufile = g_build_filename(tmppath, MRU_FILE, NULL);
  ui.configfile = g_build_filename(tmppath, CONFIG_FILE, NULL);
  g_free(tmppath);

  // initialize preferences
  init_config_default();
  load_config_from_file();
  ui.font_name = g_strdup(ui.default_font_name);
  ui.font_size = ui.default_font_size;
  ui.hiliter_alpha_mask = 0xffffff00 + (guint)(255*ui.hiliter_opacity);

  // we need an empty canvas prior to creating the journal structures
  canvas = GNOME_CANVAS (gnome_canvas_new_aa ());

  // initialize data
  ui.default_page.bg->canvas_item = NULL;
  ui.layerbox_length = 0;

   
  undo = NULL; redo = NULL;
  journal.pages = NULL;
  bgpdf.status = STATUS_NOT_INIT;

  new_journal();  
  
  ui.cur_item_type = ITEM_NONE;
  ui.cur_item = NULL;
  ui.cur_path.coords = NULL;
  ui.cur_path_storage_alloc = 0;
  ui.cur_path.ref_count = 1;
  ui.cur_widths = NULL;
  ui.cur_widths_storage_alloc = 0;

  ui.selection = NULL;
  ui.cursor = NULL;
  ui.pen_cursor_pix = ui.hiliter_cursor_pix = NULL;

  ui.cur_brush = &(ui.brushes[0][ui.toolno[0]]);
  for (j=0; j<=NUM_BUTTONS; j++)
    for (i=0; i < NUM_STROKE_TOOLS; i++) {
      b = &(ui.brushes[j][i]);
      b->tool_type = i;
      if (b->color_no>=0) {
        b->color_rgba = predef_colors_rgba[b->color_no];
        if (i == TOOL_HIGHLIGHTER) {
          b->color_rgba &= ui.hiliter_alpha_mask;
        }
      }
      b->thickness = predef_thickness[i][b->thickness_no];
    }
  for (i=0; i<NUM_STROKE_TOOLS; i++)
    g_memmove(ui.default_brushes+i, &(ui.brushes[0][i]), sizeof(struct Brush));

  ui.cur_mapping = 0;
  ui.which_unswitch_button = 0;
  ui.in_proximity = FALSE;
  ui.warned_generate_fontconfig = FALSE;
  
  reset_recognizer();

  // initialize various interface elements
  
  gtk_window_set_default_size(GTK_WINDOW (winMain), ui.window_default_width, ui.window_default_height);
  if (ui.maximize_at_start) gtk_window_maximize(GTK_WINDOW (winMain));
  update_toolbar_and_menu();
  update_font_button();

  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("journalApplyAllPages")), ui.bg_apply_all_pages);
  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("journalNewPageKeepsBG")), ui.new_page_bg_from_pdf);
  if (ui.fullscreen) {
    gtk_check_menu_item_set_active(
      GTK_CHECK_MENU_ITEM(GET_COMPONENT("viewFullscreen")), TRUE);
    gtk_toggle_tool_button_set_active(
      GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonFullscreen")), TRUE);
    gtk_window_fullscreen(GTK_WINDOW(winMain));
  }
  gtk_button_set_relief(GTK_BUTTON(GET_COMPONENT("buttonColorChooser")), GTK_RELIEF_NONE);

  allow_all_accels();
  add_scroll_bindings();

  // prevent interface items from stealing focus
  // glade doesn't properly handle can_focus, so manually set it
  gtk_combo_box_set_focus_on_click(GTK_COMBO_BOX(GET_COMPONENT("comboLayer")), FALSE);
  g_signal_connect(GET_COMPONENT("spinPageNo"), "activate",
          G_CALLBACK(handle_activate_signal), NULL);
  gtk_container_forall(GTK_CONTAINER(winMain), unset_flags, (gpointer)GTK_CAN_FOCUS);
  GTK_WIDGET_SET_FLAGS(GTK_WIDGET(canvas), GTK_CAN_FOCUS);
  GTK_WIDGET_SET_FLAGS(GET_COMPONENT("spinPageNo"), GTK_CAN_FOCUS);
  
  // install hooks on button/key/activation events to make the spinPageNo lose focus
  gtk_container_forall(GTK_CONTAINER(winMain), install_focus_hooks, NULL);

  // set up and initialize the canvas

  gtk_widget_show (GTK_WIDGET (canvas));
  w = GET_COMPONENT("scrolledwindowMain");
  gtk_container_add (GTK_CONTAINER (w), GTK_WIDGET (canvas));
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW (w), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_events (GTK_WIDGET (canvas), 
     GDK_EXPOSURE_MASK | GDK_POINTER_MOTION_MASK | GDK_BUTTON_MOTION_MASK | 
     GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_KEY_PRESS_MASK | 
     GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK |
     GDK_PROXIMITY_IN_MASK | GDK_PROXIMITY_OUT_MASK);
  gnome_canvas_set_pixels_per_unit (canvas, ui.zoom);
  gnome_canvas_set_center_scroll_region (canvas, TRUE);
  gtk_layout_get_hadjustment(GTK_LAYOUT (canvas))->step_increment = ui.scrollbar_step_increment;
  gtk_layout_get_vadjustment(GTK_LAYOUT (canvas))->step_increment = ui.scrollbar_step_increment;

  // set up the page size and canvas size
  update_page_stuff();

  g_signal_connect ((gpointer) canvas, "button_press_event",
                    G_CALLBACK (on_canvas_button_press_event),
                    NULL);
  g_signal_connect ((gpointer) canvas, "button_release_event",
                    G_CALLBACK (on_canvas_button_release_event),
                    NULL);
  g_signal_connect ((gpointer) canvas, "enter_notify_event",
                    G_CALLBACK (on_canvas_enter_notify_event),
                    NULL);
  g_signal_connect ((gpointer) canvas, "leave_notify_event",
                    G_CALLBACK (on_canvas_leave_notify_event),
                    NULL);
  g_signal_connect ((gpointer) canvas, "proximity_in_event",
                    G_CALLBACK (on_canvas_proximity_event),
                    NULL);
  g_signal_connect ((gpointer) canvas, "proximity_out_event",
                    G_CALLBACK (on_canvas_proximity_event),
                    NULL);
  g_signal_connect ((gpointer) canvas, "expose_event",
                    G_CALLBACK (on_canvas_expose_event),
                    NULL);
  g_signal_connect ((gpointer) canvas, "key_press_event",
                    G_CALLBACK (on_canvas_key_press_event),
                    NULL);
  g_signal_connect ((gpointer) canvas, "motion_notify_event",
                    G_CALLBACK (on_canvas_motion_notify_event),
                    NULL);
  g_signal_connect ((gpointer) gtk_layout_get_vadjustment(GTK_LAYOUT(canvas)),
                    "value-changed", G_CALLBACK (on_vscroll_changed),
                    NULL);
  g_signal_connect ((gpointer) gtk_layout_get_hadjustment(GTK_LAYOUT(canvas)),
                    "value-changed", G_CALLBACK (on_hscroll_changed),
                    NULL);
  g_object_set_data (G_OBJECT (winMain), "canvas", canvas);

  screen = gtk_widget_get_screen(winMain);
  ui.screen_width = gdk_screen_get_width(screen);
  ui.screen_height = gdk_screen_get_height(screen);
  
  can_xinput = FALSE;
  dev_list = gdk_devices_list();
  while (dev_list != NULL) {
    device = (GdkDevice *)dev_list->data;
    if (device != gdk_device_get_core_pointer() && device->num_axes >= 2) {
      /* get around a GDK bug: map the valuator range CORRECTLY to [0,1] */
#ifdef ENABLE_XINPUT_BUGFIX
      gdk_device_set_axis_use(device, 0, GDK_AXIS_IGNORE);
      gdk_device_set_axis_use(device, 1, GDK_AXIS_IGNORE);
#endif
      gdk_device_set_mode(device, GDK_MODE_SCREEN);
      if (g_strrstr(device->name, "raser"))
        gdk_device_set_source(device, GDK_SOURCE_ERASER);
      can_xinput = TRUE;
    }
    dev_list = dev_list->next;
  }
  if (!can_xinput)
    gtk_widget_set_sensitive(GET_COMPONENT("optionsUseXInput"), FALSE);

  ui.use_xinput = ui.allow_xinput && can_xinput;

  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("optionsProgressiveBG")), ui.progressive_bg);
  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("optionsPrintRuling")), ui.print_ruling);
  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("optionsLegacyPDFExport")), ui.exportpdf_prefer_legacy);
  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("optionsLayersPDFExport")), ui.exportpdf_layers);
  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("optionsAutoloadPdfXoj")), ui.autoload_pdf_xoj);
  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("optionsAutosaveXoj")), ui.autosave_enabled);
  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("optionsLeftHanded")), ui.left_handed);
  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("optionsShortenMenus")), ui.shorten_menus);
  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("optionsAutoSavePrefs")), ui.auto_save_prefs);
  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("optionsButtonSwitchMapping")), ui.button_switch_mapping);
  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("optionsPenCursor")), ui.pen_cursor);
  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("viewDisplayLayersAbovePrefs")), ui.display_layers_above);

  hide_unimplemented();

  update_undo_redo_enabled();
  update_copy_paste_enabled();
  update_vbox_order(ui.vertical_order[ui.fullscreen?1:0]);
  gtk_widget_grab_focus(GTK_WIDGET(canvas));

  // show everything...
  
  gtk_widget_show (winMain);
  update_cursor();

  /* this will cause extension events to get enabled/disabled, but
     we need the windows to be mapped first */
  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("optionsUseXInput")), ui.use_xinput);

  /* fix a bug in GTK+ 2.16 and 2.17: scrollbars shouldn't get extended
     input events from pointer motion when cursor moves into main window */

  if (!gtk_check_version(2, 16, 0)) {
    g_signal_connect (
      GET_COMPONENT("menubar"),
      "event", G_CALLBACK (filter_extended_events),
      NULL);
    g_signal_connect (
      GET_COMPONENT("toolbarMain"),
      "event", G_CALLBACK (filter_extended_events),
      NULL);
    g_signal_connect (
      GET_COMPONENT("toolbarPen"),
      "event", G_CALLBACK (filter_extended_events),
      NULL);
    g_signal_connect (
      GET_COMPONENT("statusbar"),
      "event", G_CALLBACK (filter_extended_events),
      NULL);
    g_signal_connect (
      (gpointer)(gtk_scrolled_window_get_vscrollbar(GTK_SCROLLED_WINDOW(w))),
      "event", G_CALLBACK (filter_extended_events),
      NULL);
    g_signal_connect (
      (gpointer)(gtk_scrolled_window_get_hscrollbar(GTK_SCROLLED_WINDOW(w))),
      "event", G_CALLBACK (filter_extended_events),
      NULL);
  }

  // load the MRU
  
  mru_init();

  // and finally, open a file specified on the command line
  // (moved here because display parameters weren't initialized yet...)
  
  if (filename == NULL) {
      if (clOptions->screenshot) {
          printf("Click on screen to make screenshot...\n");
          on_journalScreenshot_activate(NULL, NULL);
      }
      return;
  }

  set_cursor_busy(TRUE);
  if (g_path_is_absolute(filename))
    tmpfn = g_strdup(filename);
  else {
    tmppath = g_get_current_dir();
    tmpfn = g_build_filename(tmppath, filename, NULL);
    g_free(tmppath);
  }
  success = open_journal(tmpfn);

  if (success) {
    // make sure the page to jump to is valid
    if (clOptions->openAtPageNumber > journal.npages) {
      clOptions->openAtPageNumber = journal.npages;
    } else if (clOptions->openAtPageNumber < 1) {
      clOptions->openAtPageNumber = 1;
    }
    // only jump in needed
    if (clOptions->openAtPageNumber != 1) 
      do_switch_page(clOptions->openAtPageNumber - 1, TRUE, TRUE);
  }
  
  g_free(tmpfn);
  set_cursor_busy(FALSE);
  if (!success) {
    w = gtk_message_dialog_new(GTK_WINDOW (winMain), GTK_DIALOG_DESTROY_WITH_PARENT,
       GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, _("Error opening file '%s'"), filename);
    wrapper_gtk_dialog_run(GTK_DIALOG(w));
    gtk_widget_destroy(w);
  }
}

void xo_warn_user(gchar *message)
{
    GtkWidget *dialog;

    dialog = gtk_message_dialog_new(GTK_WINDOW(winMain), GTK_DIALOG_MODAL,
                                    GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, 
                                    message, NULL);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

void xo_toolbutton_enable_button_press(char *name, void *callback)
{
    GList *list = gtk_container_get_children(GTK_CONTAINER(GET_COMPONENT(name)));

      
    GtkWidget *w2 = GTK_WIDGET(g_list_first(list)->data);

    // the big assumption is that this list is size 1 and only contains the GtkButton
    assert(strcmp(G_OBJECT_TYPE_NAME(w2), "GtkButton") == 0);

    g_signal_connect (w2,
                      "button-press-event", G_CALLBACK (callback),
                      NULL);
}


GtkWidget *xo_init_gtk_builder(char *executableFileName)
{
  gchar *path = g_path_get_dirname(executableFileName);
  gchar *pathGlade = g_build_filename(path, GLADE_FILE, NULL);

  GError *err = NULL;

  // Search for the glade file in the location of the binary, its parent, then in the installed data directory
  builder = gtk_builder_new();
  if(!gtk_builder_add_from_file(builder, pathGlade, &err)) {
    gchar *parentPathGlade;
    parentPathGlade = g_build_filename(path, "..", GLADE_FILE, NULL);
    err = NULL;
    if(!gtk_builder_add_from_file(builder, parentPathGlade, &err)) {
      gchar *dataPathGlade;
      dataPathGlade = PACKAGE_DATA_DIR "/" PACKAGE "/" GLADE_FILE;
      err = NULL;
      if (!gtk_builder_add_from_file(builder, dataPathGlade, &err)) {
        fprintf(stderr, "Not able to find  Glade file from (searched in location of binary (%s), its parent directory (%s), and (%s)) CWD: %s\n", pathGlade, parentPathGlade, dataPathGlade, err->message);
        exit(1);
      }
    }
    g_free(parentPathGlade);
  }
  g_free(path);
  g_free(pathGlade);

  xo_toolbutton_enable_button_press("buttonNextPage",      on_buttonNextPage_button_press_event);
  xo_toolbutton_enable_button_press("buttonPreviousPage",  on_buttonNextPage_button_press_event);
  xo_toolbutton_enable_button_press("buttonFirstPage",  on_buttonNextPage_button_press_event);
  xo_toolbutton_enable_button_press("buttonLastPage",  on_buttonNextPage_button_press_event);
  

  gtk_builder_connect_signals (builder, NULL);

  winMain = (GtkWidget *)gtk_builder_get_object (builder, "winMain");


}




void parse_command_line(int argc, char* argv[], command_line_options *clo)
{
  GError  *error = NULL;
  GOptionEntry entries[] = {
    { "page", 'p', 0, G_OPTION_ARG_INT,       &(clo->openAtPageNumber), "Jump to Page", "N" },
    { "screenshot", 's', 0, G_OPTION_ARG_NONE, &(clo->screenshot), "Start with screenshot", "S" },
    { "no-next-splash-message", 0, 0, G_OPTION_ARG_NONE, &(clo->noNextSplash), "Do not show the Next splash message ", NULL },
    { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &(clo->fileArguments), NULL, N_("[FILE]") },
    { NULL }
  };
  GOptionContext *context;
  
  // parse command line options
  context = g_option_context_new ("");
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  g_option_context_add_group (context, gtk_get_option_group (TRUE));
  if (!g_option_context_parse (context, &argc, &argv, &error)) {
    fprintf(stderr, "%s\n\nUsage %s [options]* [filename]\n\nUse --help to see options\n\n",
            error->message, argv[0]);
    exit (1);
  }

  if (clo->screenshot) {
      // simply disable it it. It gets on the way
      clo->noNextSplash = TRUE;
  }
      

  // The pointer to the rest of the arguments marks its end with NULL, so we need to traverse it to find out how many they were
  
  clo->fileCount = 0;
  if (clo->fileArguments) {
    char **p = clo->fileArguments;
    while (*p != NULL) {
      p++;
      clo->fileCount++;
    }
  }
}

int
main (int argc, char *argv[])
{
  gchar *path, *path1, *path2;

  command_line_options clOptions = {
      1, // openAtPagenumber
      FALSE, // screenshot
      FALSE, // noNextSplash
      0, // fileCount
      NULL, //fileArguments
  };


#ifdef ENABLE_NLS
  bindtextdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);
#endif
  
  gtk_set_locale ();


  gtk_init (&argc, &argv);

  parse_command_line(argc, argv, &clOptions);

  path = g_path_get_dirname(argv[0]);
  path1 = g_build_filename(path, "pixmaps", NULL);
  path2 = g_build_filename(path, "..", "pixmaps", NULL);
  add_pixmap_directory (path);
  add_pixmap_directory (path2);
  add_pixmap_directory (path1);
  g_free(path);
  g_free(path1);
  g_free(path2);
  add_pixmap_directory (PACKAGE_DATA_DIR "/" PACKAGE "/pixmaps");

  xo_init_gtk_builder(argv[0]);

  
  init_stuff (&clOptions);

  gtk_window_set_icon(GTK_WINDOW(winMain), create_pixbuf("xournal.png"));
  
  if (!clOptions.noNextSplash) {
      xo_warn_user(_("This is not an official build of xournal.\n\n You should not use it unless you understand what you are doing. You have been warned.\n\n--dmg"));
  }

  gtk_main ();

  if (bgpdf.status != STATUS_NOT_INIT) shutdown_bgpdf();


  // save page numbers
  if (ui.filename != NULL && strcmp(mru_filename(0), ui.filename) == 0) {
  // make sure we have a name before we update it...
    mru_set_pagenumber(0, ui.pageno+1);
    journal_metadata_page_save(ui.filename, ui.pageno+1);
  }

  mru_save_list();
  autosave_cleanup(&ui.autosave_filename_list);
  if (ui.auto_save_prefs) save_config_to_file();
  
  return 0;
}

