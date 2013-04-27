/* biji-note-obj.c
 * Copyright (C) Pierre-Yves LUYTEN 2012 <py@luyten.fr>
 * 
 * bijiben is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * bijiben is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "biji-date-time.h"
#include "biji-note-id.h"
#include "biji-note-book.h"
#include "biji-note-obj.h"
#include "biji-timeout.h"
#include "biji-tracker.h"
#include "biji-zeitgeist.h"
#include "editor/biji-webkit-editor.h"
#include "serializer/biji-lazy-serializer.h"

#include <libgd/gd.h>

/* Icon */
#define ICON_WIDTH 200
#define ICON_HEIGHT 200
#define ICON_FONT "Purusa 10"

/* a cute baby icon without txt. squared. */
#define EMBLEM_WIDTH ICON_WIDTH / 6
#define EMBLEM_HEIGHT EMBLEM_WIDTH

struct _BijiNoteObjPrivate
{
  /* Notebook might be null. */
  BijiNoteBook          *book;

  /* Metadata */
  BijiNoteID            *id;
  GdkRGBA               *color;

  /* Data */
  gchar                 *html;
  gchar                 *raw_text;
  BijiWebkitEditor      *editor;

  /* Save */
  BijiTimeout           *timeout;
  gboolean              needs_save;

  /* Icon might be null untill usefull
   * Emblem is smaller & just shows the color */
  GdkPixbuf             *icon;
  GdkPixbuf             *emblem;
  gboolean              icon_needs_update;
  gboolean              emblem_needs_update;

  /* Tags 
   * In Tomboy, templates are 'system:notebook:%s' tags.*/
  GHashTable            *labels;
  gboolean              is_template ;
  gboolean              does_title_survive;

  /* Signals */
  gulong note_renamed;
  gulong save;
};

/* Properties
 * Actually path is just sent to note id */
enum {
  PROP_0,
  PROP_PATH,
  BIJI_OBJ_PROPERTIES
};

static GParamSpec *properties[BIJI_OBJ_PROPERTIES] = { NULL, };

#define BIJI_NOTE_OBJ_PRIVATE(o)  (G_TYPE_INSTANCE_GET_PRIVATE ((o), BIJI_TYPE_NOTE_OBJ, BijiNoteObjPrivate))

G_DEFINE_TYPE (BijiNoteObj, biji_note_obj, BIJI_TYPE_ITEM);

/* BijiItem iface */
static gchar     * biji_note_obj_get_title                (BijiItem *note);
static gchar     * biji_note_obj_get_path                 (BijiItem *note);
static GdkPixbuf * biji_note_obj_get_icon                 (BijiItem *note);
static GdkPixbuf * biji_note_obj_get_emblem               (BijiItem *note);
static glong       biji_note_obj_get_last_change_date_sec (BijiItem *note);

static void
on_save_timeout (BijiNoteObj *self)
{
  BijiNoteObjPrivate *priv = self->priv;

  if (!priv->needs_save)
    return;

  g_object_ref (self);

  biji_lazy_serialize (self);
  bijiben_push_note_to_tracker(self);

  priv->needs_save = FALSE;
  g_object_unref (self);
}

static void
biji_note_obj_init (BijiNoteObj *self)
{
  BijiNoteObjPrivate *priv ;
    
  priv = G_TYPE_INSTANCE_GET_PRIVATE (self, BIJI_TYPE_NOTE_OBJ, BijiNoteObjPrivate);

  self->priv = priv ;

  priv->id = g_object_new (BIJI_TYPE_NOTE_ID, NULL);

  priv->needs_save = FALSE;
  priv->timeout = biji_timeout_new ();
  priv->save = g_signal_connect_swapped (priv->timeout, "timeout",
                            G_CALLBACK (on_save_timeout), self);

  priv->book = NULL ;
  priv->is_template = FALSE ;

  /* Existing note keep their title.
   * only brand new notes might see title changed */
  priv->does_title_survive = TRUE;

  /* The editor is NULL so we know it's not opened
   * neither fully deserialized */
  priv->html = NULL;
  priv->editor = NULL;

  /* Icon is only computed when necessary */
  priv->icon = NULL;
  priv->emblem = NULL;

  /* Keep value unitialied, so bijiben knows to assign default color */
  priv->color = NULL;

  priv->labels = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
}

static void
biji_note_obj_finalize (GObject *object)
{    
  BijiNoteObj        *self = BIJI_NOTE_OBJ(object);
  BijiNoteObjPrivate *priv = self->priv;

  if (priv->timeout)
    g_object_unref (priv->timeout);

  if (priv->needs_save)
    on_save_timeout (self);

  g_clear_object (&priv->id);

  if (priv->html)
    g_free (priv->html);

  if (priv->raw_text);
    g_free (priv->raw_text);

  g_hash_table_destroy (priv->labels);

  g_clear_object (&priv->icon);
  g_clear_object (&priv->emblem);
  gdk_rgba_free (priv->color);

  G_OBJECT_CLASS (biji_note_obj_parent_class)->finalize (object);
}

// Signals to be used by biji note obj
enum {
  NOTE_RENAMED,
  NOTE_DELETED,
  NOTE_CHANGED,
  NOTE_COLOR_CHANGED,
  BIJI_OBJ_SIGNALS
};

static guint biji_obj_signals [BIJI_OBJ_SIGNALS] = { 0 };

/* we do NOT deserialize here. it might be a brand new note
 * it's up the book to ask .note to be read*/
static void
biji_note_obj_constructed (GObject *obj)
{
}

static void
biji_note_obj_set_property (GObject      *object,
                            guint         property_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  BijiNoteObj *self = BIJI_NOTE_OBJ (object);


  switch (property_id)
    {
    case PROP_PATH:
      g_object_set_property (G_OBJECT (self->priv->id), "path", value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
biji_note_obj_get_property (GObject    *object,
                            guint       property_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  BijiNoteObj *self = BIJI_NOTE_OBJ (object);

  switch (property_id)
    {
    case PROP_PATH:
      g_value_set_object (value, biji_note_id_get_path (self->priv->id));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
biji_note_obj_class_init (BijiNoteObjClass *klass)
{
  BijiItemClass*  item_class = BIJI_ITEM_CLASS (klass);
  GObjectClass* object_class = G_OBJECT_CLASS  (klass);

  object_class->constructed = biji_note_obj_constructed;
  object_class->finalize = biji_note_obj_finalize;
  object_class->get_property = biji_note_obj_get_property;
  object_class->set_property = biji_note_obj_set_property;

  properties[PROP_PATH] =
    g_param_spec_string("path",
                        "The note file",
                        "The location where the note is stored and saved",
                        NULL,
                        G_PARAM_READWRITE);

  g_object_class_install_properties (object_class, BIJI_OBJ_PROPERTIES, properties);

  biji_obj_signals[NOTE_RENAMED] = g_signal_new ( "renamed" ,
                                                  G_OBJECT_CLASS_TYPE (klass),
                                                  G_SIGNAL_RUN_LAST,
                                                  0, 
                                                  NULL, 
                                                  NULL,
                                                  g_cclosure_marshal_VOID__VOID,
                                                  G_TYPE_NONE,
                                                  0);

  biji_obj_signals[NOTE_CHANGED] = g_signal_new ( "changed" ,
                                                  G_OBJECT_CLASS_TYPE (klass),
                                                  G_SIGNAL_RUN_LAST,
                                                  0, 
                                                  NULL, 
                                                  NULL,
                                                  g_cclosure_marshal_VOID__VOID,
                                                  G_TYPE_NONE,
                                                  0);

  biji_obj_signals[NOTE_COLOR_CHANGED] = g_signal_new ("color-changed" ,
                                                  G_OBJECT_CLASS_TYPE (klass),
                                                  G_SIGNAL_RUN_LAST,
                                                  0, 
                                                  NULL, 
                                                  NULL,
                                                  g_cclosure_marshal_VOID__VOID,
                                                  G_TYPE_NONE,
                                                  0);

  biji_obj_signals[NOTE_DELETED] = g_signal_new ( "deleted" ,
                                                  G_OBJECT_CLASS_TYPE (klass),
                                                  G_SIGNAL_RUN_LAST,
                                                  0, 
                                                  NULL, 
                                                  NULL,
                                                  g_cclosure_marshal_VOID__VOID,
                                                  G_TYPE_NONE,
                                                  0);

  g_type_class_add_private (klass, sizeof (BijiNoteObjPrivate));

  /* Interface */
  item_class->get_title = biji_note_obj_get_title;
  item_class->get_uuid = biji_note_obj_get_path;
  item_class->get_icon = biji_note_obj_get_icon;
  item_class->get_emblem = biji_note_obj_get_emblem;
  item_class->get_change_sec = biji_note_obj_get_last_change_date_sec;
}

BijiNoteObj *
biji_note_obj_new_from_path (const gchar *path)
{
  return g_object_new (BIJI_TYPE_NOTE_OBJ,
                       "path", path,
                       NULL); 
}

void 
_biji_note_obj_set_book(BijiNoteObj *note, gpointer biji_note_book)
{
  if ( BIJI_IS_NOTE_BOOK(biji_note_book) )
  {   
    note->priv->book = biji_note_book ;
  }
  else
  {
    g_message("criticical : BIJI NOTE OBJ SET BOOK received wrong parameter");
  }
}

gpointer
biji_note_obj_get_note_book(BijiNoteObj *note)
{
  if ( note->priv->book != NULL )
  {
    return note->priv->book ;
  }
  g_message("biji note obj _ biji note obj get note book returns NULL");
  return NULL ;
}

gboolean 
biji_note_obj_are_same (BijiNoteObj *a, BijiNoteObj* b)
{
  if (biji_note_id_equal (a->priv->id,b->priv->id)
      && g_strcmp0 (a->priv->raw_text ,b->priv->raw_text) == 0)
    return TRUE;

  return FALSE;
}

/* First cancel timeout
 * this func is most probably stupid it might exists (move file) */
gboolean
biji_note_obj_trash (BijiNoteObj *note_to_kill)
{
  BijiNoteObjPrivate *priv = note_to_kill->priv;
  GFile *to_trash, *parent, *trash, *backup_file, *icon;
  gchar *note_name, *parent_path, *trash_path, *backup_path, *icon_path;
  GError *error = NULL;
  gboolean result = FALSE;

  priv->needs_save = FALSE;
  biji_timeout_cancel (priv->timeout);

  to_trash = biji_note_id_get_file (note_to_kill->priv->id);

  /* Don't try to backup a file which does not exist */
  if (to_trash)
  {
    note_name = g_file_get_basename (to_trash);
    parent = g_file_get_parent (to_trash);

    /* Create the trash directory
     * No matter if already exists */
    parent_path = g_file_get_path (parent);
    trash_path = g_build_filename (parent_path, ".Trash", NULL);
    g_free (parent_path);
    g_object_unref (parent);
    trash = g_file_new_for_path (trash_path);
    g_file_make_directory (trash, NULL, NULL);

    /* Move the note to trash */
    backup_path = g_build_filename (trash_path, note_name, NULL);
    g_free (trash_path);
    backup_file = g_file_new_for_path (backup_path);
    g_free (note_name);
    g_free (backup_path);
    result = g_file_move (to_trash,
                          backup_file,
                          G_FILE_COPY_OVERWRITE,
                          NULL, // cancellable
                          NULL, // progress callback
                          NULL, // progress_callback_data,
                          &error);

    if (error)
    {
      g_message ("%s", error->message);
      g_error_free (error);
      error = NULL;
    }

    /* Say goodbye however */
    g_object_unref (trash);
    g_object_unref (backup_file);
  }

  /* Delete icon file */
  icon_path = biji_note_obj_get_icon_file (note_to_kill);
  icon = g_file_new_for_path (icon_path);
  g_free (icon_path);
  g_file_delete (icon, NULL, NULL);
  g_object_unref (icon);

  /* Tracker, NoteBook, Memory. TODO : zeitgeist */
  biji_note_delete_from_tracker (note_to_kill);
  g_signal_emit (G_OBJECT (note_to_kill), biji_obj_signals[NOTE_DELETED], 0);
  g_clear_object (&note_to_kill);

  return result;
}

static gchar *
biji_note_obj_get_path (BijiItem *item)
{
  g_return_val_if_fail (BIJI_IS_NOTE_OBJ (item), NULL);

  BijiNoteObj *note = BIJI_NOTE_OBJ (item);

  return biji_note_id_get_path (note->priv->id);
}

BijiNoteID* note_get_id(BijiNoteObj* n)
{
  return n->priv->id;
}

static gchar *
biji_note_obj_get_title (BijiItem *note)
{
  g_return_val_if_fail (BIJI_IS_NOTE_OBJ (note), NULL);

  return biji_note_id_get_title (BIJI_NOTE_OBJ (note)->priv->id);
}

/* If already a title, then note is renamed */
gboolean
biji_note_obj_set_title (BijiNoteObj *note, gchar *proposed_title)
{
  gboolean initial = FALSE;
  note->priv->does_title_survive = TRUE;
  gchar *title;

  if (!biji_note_id_get_title (note->priv->id))
    initial = TRUE;

  if (g_strcmp0 (proposed_title, biji_note_id_get_title (note->priv->id))==0)
    return FALSE;

  /* If the note is really renamed, check the new title */
  if (!initial)
  {
    title = biji_note_book_get_unique_title (note->priv->book, proposed_title);
    biji_note_id_set_last_metadata_change_date_now (note->priv->id);
  }

  /* Otherwise it's up to the caller to sanitize its title */
  else
  {
    title = g_strdup (proposed_title);
  }

  /* Emit signal even if initial title, just to let know */
  biji_note_id_set_title (note->priv->id, title);
  g_free (title);
  g_signal_emit (G_OBJECT (note), biji_obj_signals[NOTE_RENAMED],0);
  return TRUE;
}

gboolean
biji_note_obj_title_survives (BijiNoteObj *note)
{
  return note->priv->does_title_survive;
}

void
biji_note_obj_set_title_survives (BijiNoteObj *obj, gboolean value)
{
  g_return_if_fail (BIJI_IS_NOTE_OBJ(obj));

  obj->priv->does_title_survive = value;
}

gboolean
biji_note_obj_set_last_change_date (BijiNoteObj* n,gchar* date)
{
  g_return_val_if_fail (BIJI_IS_NOTE_OBJ(n), FALSE);
  return biji_note_id_set_last_change_date (n->priv->id,date);
}

static glong
biji_note_obj_get_last_change_date_sec (BijiItem *item)
{
  BijiNoteObj *n = BIJI_NOTE_OBJ (item);

  return biji_note_id_get_last_change_date_sec(note_get_id(n));
}

gchar *
biji_note_obj_get_last_change_date_string (BijiNoteObj *self)
{
  return biji_get_time_diff_with_time (
             biji_note_id_get_last_change_date_sec(note_get_id(self)));
}

gchar *
biji_note_obj_get_last_metadata_change_date (BijiNoteObj *note)
{
  g_return_val_if_fail (BIJI_IS_NOTE_OBJ (note), NULL);

  return biji_note_id_get_last_metadata_change_date (note->priv->id);
}

gboolean
biji_note_obj_set_last_metadata_change_date (BijiNoteObj* n,gchar* date)
{
  g_return_val_if_fail (BIJI_IS_NOTE_OBJ(n), FALSE);

  return biji_note_id_set_last_metadata_change_date (n->priv->id,date);
}

gboolean
biji_note_obj_set_note_create_date (BijiNoteObj* n,gchar *date)
{
  g_return_val_if_fail (BIJI_IS_NOTE_OBJ(n), FALSE);

  return biji_note_id_set_create_date (n->priv->id, date);
}

static void
biji_note_obj_set_rgba_internal (BijiNoteObj *n, GdkRGBA *rgba)
{
  n->priv->color = gdk_rgba_copy(rgba);
  n->priv->icon_needs_update = TRUE;
  n->priv->emblem_needs_update = TRUE;

  g_signal_emit (G_OBJECT (n), biji_obj_signals[NOTE_COLOR_CHANGED],0);
}


void
biji_note_obj_set_rgba (BijiNoteObj *n, GdkRGBA *rgba)
{
  if (!n->priv->color)
    biji_note_obj_set_rgba_internal (n, rgba);

  else if (!gdk_rgba_equal (n->priv->color,rgba))
  {
    gdk_rgba_free (n->priv->color);
    biji_note_obj_set_rgba_internal (n, rgba);

    biji_note_id_set_last_metadata_change_date_now (n->priv->id);
    biji_note_obj_save_note (n);
  }
}

gboolean
biji_note_obj_get_rgba(BijiNoteObj *n,
                       GdkRGBA *rgba)
{
  g_return_val_if_fail (BIJI_IS_NOTE_OBJ (n), FALSE);

  if (n->priv->color && rgba)
    {
      *rgba = *(n->priv->color);
      return TRUE;
    }

  return FALSE;
}

static void
biji_note_obj_clear_icons (BijiNoteObj *note)
{
  if (note->priv->icon)
  {
    g_clear_object (&note->priv->icon);
    note->priv->icon = NULL ;
  }

  if (note->priv->emblem)
  {
    g_clear_object (&note->priv->emblem);
    note->priv->emblem = NULL ;
  }

  g_signal_emit (G_OBJECT (note), biji_obj_signals[NOTE_CHANGED],0);
}

void biji_note_obj_set_raw_text (BijiNoteObj *note, gchar *plain_text)
{
  if (note->priv->raw_text)
    g_free (note->priv->raw_text);

  note->priv->raw_text = g_strdup (plain_text);
  biji_note_obj_clear_icons (note);
}

GList *
biji_note_obj_get_collections (BijiNoteObj *n)
{
  g_return_val_if_fail (BIJI_IS_NOTE_OBJ (n), NULL);

  return g_hash_table_get_values (n->priv->labels);
}

gboolean
biji_note_obj_has_collection (BijiNoteObj *note, gchar *label)
{
  if (g_hash_table_lookup (note->priv->labels, label))
    return TRUE;

  return FALSE;
}

gboolean
biji_note_obj_add_collection (BijiNoteObj *note, gchar *label, gboolean on_user_action_cb)
{
  g_return_val_if_fail (BIJI_IS_NOTE_OBJ (note), FALSE);
  g_return_val_if_fail (label != NULL, FALSE);
  g_return_val_if_fail (!biji_note_obj_has_collection (note, label), FALSE);

  gchar *tag = g_strdup (label);

  g_hash_table_add (note->priv->labels, tag);

  if (on_user_action_cb)
  {
    biji_push_existing_collection_to_note (note, tag); // Tracker
    biji_note_id_set_last_metadata_change_date_now (note->priv->id);
    biji_note_obj_save_note (note);
  }

  return TRUE;
}

gboolean
biji_note_obj_remove_collection (BijiNoteObj *note, gchar *label, gchar *urn)
{
  g_return_val_if_fail (BIJI_IS_NOTE_OBJ (note), FALSE);

  if (g_hash_table_remove (note->priv->labels, label))
  {
    biji_remove_collection_from_note (note, urn); // tracker.
    biji_note_id_set_last_metadata_change_date_now (note->priv->id);
    biji_note_obj_save_note (note);
    return TRUE;
  }

  return FALSE;
}

gboolean
biji_note_obj_has_tag_prefix (BijiNoteObj *note, gchar *label)
{
  gboolean retval = FALSE;
  GList *tags, *l;

  tags = g_hash_table_get_keys (note->priv->labels);

  for (l = tags; l != NULL; l=l->next)
  {
    if (g_str_has_prefix (l->data, label))
    {
      retval = TRUE;
      break;
    }
  }

  g_list_free (tags);
  return retval;
}

gboolean
note_obj_is_template (BijiNoteObj *n)
{
  g_return_val_if_fail(BIJI_IS_NOTE_OBJ(n),FALSE);
  return n->priv->is_template;
}

void
note_obj_set_is_template (BijiNoteObj *n,gboolean is_template)
{
  n->priv->is_template = is_template ;
}

void
biji_note_obj_save_note (BijiNoteObj *self)
{
  self->priv->needs_save = TRUE;
  biji_timeout_reset (self->priv->timeout, 3000);
}

gchar *
biji_note_obj_get_icon_file (BijiNoteObj *note)
{
  g_return_val_if_fail (BIJI_IS_NOTE_OBJ (note), NULL);

  gchar *uuid, *basename, *filename;

  uuid = biji_note_id_get_uuid (note->priv->id);
  basename = biji_str_mass_replace (uuid, ".note", ".png", NULL);

  filename = g_build_filename (g_get_user_cache_dir (),
                               g_get_application_name (),
                               basename,
                               NULL);

  g_free (uuid);
  g_free (basename);

  return filename;
}

void
biji_note_obj_set_icon (BijiNoteObj *note, GdkPixbuf *pix)
{
  g_return_if_fail (BIJI_IS_NOTE_OBJ (note));

  if (!note->priv->icon)
    note->priv->icon = pix;

  else
    g_warning ("Cannot use _set_icon_ with iconified note. This has no sense.");
}

static GdkPixbuf *
biji_note_obj_get_icon (BijiItem *item)
{
  GdkRGBA               note_color;
  gchar                 *text;
  cairo_t               *cr;
  PangoLayout           *layout;
  PangoFontDescription  *desc;
  GdkPixbuf             *ret = NULL;
  cairo_surface_t       *surface = NULL;
  GtkBorder              frame_slice = { 4, 3, 3, 6 };

  BijiNoteObj *note = BIJI_NOTE_OBJ (item);

  if (note->priv->icon && !note->priv->icon_needs_update)
    return note->priv->icon;

  /* Create & Draw surface */
  surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                        ICON_WIDTH,
                                        ICON_HEIGHT) ;
  cr = cairo_create (surface);

  /* Background */
  cairo_rectangle (cr, 0, 0, ICON_WIDTH, ICON_HEIGHT);
  if (biji_note_obj_get_rgba (note, &note_color))
    gdk_cairo_set_source_rgba (cr, &note_color);

  cairo_fill (cr);

  /* Text */
  text = biji_note_get_raw_text (note);
  if (text != NULL)
  {
    cairo_translate (cr, 10, 10);
    layout = pango_cairo_create_layout (cr);

    pango_layout_set_width (layout, 180000 );
    pango_layout_set_wrap (layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_height (layout, 180000 ) ;

    pango_layout_set_text (layout, text, -1);
    desc = pango_font_description_from_string (ICON_FONT);
    pango_layout_set_font_description (layout, desc);
    pango_font_description_free (desc);

    cairo_set_source_rgb (cr, 0.0, 0.0, 0.0);
    pango_cairo_update_layout (cr, layout);
    pango_cairo_show_layout (cr, layout);

    g_object_unref (layout);
  }

  cairo_destroy (cr);

  ret = gdk_pixbuf_get_from_surface (surface,
                                     0, 0,
                                     ICON_WIDTH,
                                     ICON_HEIGHT);
  cairo_surface_destroy (surface);

  note->priv->icon = gd_embed_image_in_frame (ret, "resource:///org/gnome/bijiben/thumbnail-frame.png",
                                              &frame_slice, &frame_slice);
  g_clear_object (&ret);
  note->priv->icon_needs_update = FALSE;

  return note->priv->icon;
}

static GdkPixbuf *
biji_note_obj_get_emblem (BijiItem *item)
{
  GdkRGBA                note_color;
  cairo_t               *cr;
  cairo_surface_t       *surface = NULL;
  BijiNoteObj           *note = BIJI_NOTE_OBJ (item);

  if (note->priv->emblem && !note->priv->emblem_needs_update)
    return note->priv->emblem;

  /* Create & Draw surface */
  surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                        EMBLEM_WIDTH,
                                        EMBLEM_HEIGHT) ;
  cr = cairo_create (surface);

  /* Background */
  cairo_rectangle (cr, 0, 0, EMBLEM_WIDTH, EMBLEM_HEIGHT);
  if (biji_note_obj_get_rgba (note, &note_color))
    gdk_cairo_set_source_rgba (cr, &note_color);

  cairo_fill (cr);

  /* Border */
  cairo_set_source_rgba (cr, 0.3, 0.3, 0.3, 1);
  cairo_set_line_width (cr, 1);
  cairo_rectangle (cr, 0, 0, EMBLEM_WIDTH, EMBLEM_HEIGHT);
  cairo_stroke (cr);

  cairo_destroy (cr);

  note->priv->emblem = gdk_pixbuf_get_from_surface (surface,
                                                    0, 0,
                                                    EMBLEM_WIDTH,
                                                    EMBLEM_HEIGHT);

  cairo_surface_destroy (surface);
  note->priv->emblem_needs_update = FALSE;

  return note->priv->emblem;
}

/* Single Note */

gchar *
biji_note_get_raw_text(BijiNoteObj *note)
{
  if (note->priv->raw_text)
    return note->priv->raw_text;

  return "";
}

gboolean
biji_note_obj_is_template(BijiNoteObj *note)
{
  return note_obj_is_template(note);
}

gchar *
biji_note_obj_get_last_change_date(BijiNoteObj *note)
{
  g_return_val_if_fail (BIJI_IS_NOTE_OBJ (note), NULL);

  return biji_note_id_get_last_change_date (note->priv->id);
}

gchar *
biji_note_obj_get_create_date(BijiNoteObj *note)
{
  g_return_val_if_fail (BIJI_IS_NOTE_OBJ (note), NULL);

  return biji_note_id_get_create_date (note->priv->id);
}

gboolean
biji_note_obj_set_create_date (BijiNoteObj *note, gchar *date)
{
  g_return_val_if_fail (BIJI_IS_NOTE_OBJ (note), FALSE);

  return biji_note_id_set_create_date (note->priv->id, date);
}

void
biji_note_obj_set_create_date_now (BijiNoteObj *note)
{
  g_return_if_fail (BIJI_IS_NOTE_OBJ (note));

  biji_note_id_set_create_date_now (note->priv->id);
}

void
biji_note_obj_set_last_change_date_now (BijiNoteObj *note)
{
  g_return_if_fail (BIJI_IS_NOTE_OBJ (note));

  biji_note_id_set_last_change_date_now (note->priv->id);
}

void
biji_note_obj_set_all_dates_now (BijiNoteObj *note)
{
  g_return_if_fail (BIJI_IS_NOTE_OBJ (note));
  BijiNoteID *id = note->priv->id;

  biji_note_id_set_create_date_now (id);
  biji_note_id_set_last_change_date_now (id);
  biji_note_id_set_last_metadata_change_date_now (id);
}

/* Webkit */

gchar *
biji_note_obj_get_html (BijiNoteObj *note)
{
  if (BIJI_IS_NOTE_OBJ (note))
    return note->priv->html;

  else
    return NULL;
}

void
biji_note_obj_set_html_content (BijiNoteObj *note,
                                gchar *html)
{
  if (html)
  {
    g_free (note->priv->html);
    note->priv->html = g_strdup (html);
  }
}

gboolean
biji_note_obj_is_opened (BijiNoteObj *note)
{
  return BIJI_IS_WEBKIT_EDITOR (note->priv->editor);
}

static void
_biji_note_obj_close (BijiNoteObj *note)
{
  BijiNoteObjPrivate *priv = note->priv;
  priv->editor = NULL;

  /* Delete if note is totaly blank
   * Actually we just need to remove it from book
   * since no change could trigger save */
  if (!priv->raw_text)
  {
    biji_note_book_remove_note (priv->book, note);
  }

  /* If the note only has one row. put some title */
  else if (!biji_note_obj_title_survives (note))
  {
    gchar *title = biji_note_book_get_unique_title (biji_note_obj_get_note_book (note),
                                                    priv->raw_text);
    biji_note_obj_set_title (note, title);
    g_free (title);
  }

  /* Else the note is not empty & has more than a row.
   * But the first row might still be empty.*/
  else if (!biji_note_id_get_title (priv->id) ||
           g_strcmp0 (biji_note_id_get_title (priv->id),"")==0)
  {
    biji_note_obj_set_title (note, priv->raw_text);
  }
}

GtkWidget *
biji_note_obj_open (BijiNoteObj *note)
{
  note->priv->editor = biji_webkit_editor_new (note);
  note->priv->editor = biji_webkit_editor_new (note);

  g_signal_connect_swapped (note->priv->editor, "destroy",
                            G_CALLBACK (_biji_note_obj_close), note);

  insert_zeitgeist (note, ZEITGEIST_ZG_ACCESS_EVENT) ;

  return GTK_WIDGET (note->priv->editor);
}

GtkWidget *
biji_note_obj_get_editor (BijiNoteObj *note)
{
  if (!biji_note_obj_is_opened (note))
    return NULL;

  return GTK_WIDGET (note->priv->editor);
}

void
biji_note_obj_editor_apply_format (BijiNoteObj *note, gint format)
{
  if (biji_note_obj_is_opened (note))
    biji_webkit_editor_apply_format ( note->priv->editor , format);
}

gboolean
biji_note_obj_editor_has_selection (BijiNoteObj *note)
{
  if (biji_note_obj_is_opened (note))
    return biji_webkit_editor_has_selection (note->priv->editor);

  return FALSE;
}

gchar *
biji_note_obj_editor_get_selection (BijiNoteObj *note)
{
  if (biji_note_obj_is_opened (note))
    return biji_webkit_editor_get_selection (note->priv->editor);

  return NULL;
}

void biji_note_obj_editor_cut (BijiNoteObj *note)
{
  if (biji_note_obj_is_opened (note))
    biji_webkit_editor_cut (note->priv->editor);
}

void biji_note_obj_editor_copy (BijiNoteObj *note)
{
  if (biji_note_obj_is_opened (note))
    biji_webkit_editor_copy (note->priv->editor);
}

void biji_note_obj_editor_paste (BijiNoteObj *note)
{
  if (biji_note_obj_is_opened (note))
    biji_webkit_editor_paste (note->priv->editor);
}

