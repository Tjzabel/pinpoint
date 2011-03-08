/*
 * Pinpoint: A small-ish presentation tool
 *
 * Copyright (C) 2010 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of the
 * License, or (at your option0 any later version.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Written by: Øyvind Kolås <pippin@linux.intel.com>
 *             Damien Lespiau <damien.lespiau@intel.com>
 *             Emmanuele Bassi <ebassi@linux.intel.com>
 */

#include "pinpoint-main.h"
#include <gio/gio.h>
#ifdef USE_CLUTTER_GST
#include <clutter-gst/clutter-gst.h>
#endif
#ifdef USE_DAX
#include <dax/dax.h>
#include "pp-super-aa.h"
#endif
#include <stdlib.h>

/* #define QUICK_ACCESS_LEFT - uncomment to move speed access from top to left,
 *                             useful on meego netbook
 */

#define RESTDEPTH   -9000.0
#define RESTX        4600.0
#define STARTPOS    -3000.0

static ClutterColor black = {0x00,0x00,0x00,0xff};

typedef struct _ClutterRenderer
{
  PinPointRenderer renderer;
  PinPointData    *data;
  GHashTable      *bg_cache;    /* only load the same backgrounds once */
  ClutterActor    *bg_rect;
  ClutterActor    *background;
  ClutterActor    *midground;
  ClutterActor    *shading;
  ClutterActor    *foreground;

  ClutterActor    *json_layer;

  ClutterActor    *commandline; 
  ClutterActor    *commandline_shading;
  char *path;               /* path of the file of the GFileMonitor callback */
  float rest_y;             /* where the text can rest */

  guint            reload_tag;
} ClutterRenderer;

typedef struct
{
  PinPointRenderer *renderer;
  ClutterActor     *background;
  ClutterActor     *text;
  float rest_y;     /* y coordinate when text is stationary unused */

  ClutterState     *state;
  ClutterActor     *json_slide;
  ClutterActor     *background2;
  ClutterScript    *script;
  ClutterActor     *midground;
  ClutterActor     *foreground;
  ClutterActor     *shading;
} ClutterPointData;

#define CLUTTER_RENDERER(renderer)  ((ClutterRenderer *) renderer)


static void     leave_slide   (ClutterRenderer  *renderer,
                               gboolean          backwards);
static void     show_slide    (ClutterRenderer  *renderer,
                               gboolean          backwards);
static void     action_slide  (ClutterRenderer  *renderer);
static void     activate_commandline   (ClutterRenderer  *renderer);
static void     file_changed  (GFileMonitor     *monitor,
                               GFile            *file,
                               GFile            *other_file,
                               GFileMonitorEvent event_type,
                               ClutterRenderer  *renderer);
static void     box_resized (ClutterActor     *actor,
                             GParamSpec       *pspec,
                             ClutterRenderer  *renderer);
static gboolean key_pressed   (ClutterActor     *actor,
                               ClutterEvent     *event,
                               ClutterRenderer  *renderer);


static void
_destroy_surface (gpointer data)
{
  /* not destroying background, since it would be destoryed with
   * the stage itself.
   */
}

#if 0
static uint hide_cursor = 0;
static gboolean hide_cursor_cb (gpointer stage)
{
  hide_cursor = 0;
  clutter_stage_hide_cursor (stage);
  return FALSE;
}
#endif

static void update_commandline_shading (ClutterRenderer *renderer);

static void
activate_commandline (ClutterRenderer *renderer)
{
  PinPointPoint *point;
  ClutterPointData *data;

  if (!renderer->data->pp_slidep)
    return;
 
  point = renderer->data->pp_slidep->data;
  data = point->data;

  clutter_actor_animate (renderer->commandline,
                         CLUTTER_LINEAR, 500,
                         "opacity", 0xff, NULL);
  clutter_actor_animate (renderer->commandline_shading,
                         CLUTTER_LINEAR, 100,
                         "opacity", (int)(point->shading_opacity*0xff*0.33), NULL);

  g_object_set (renderer->commandline, "editable", TRUE,
                "single-line-mode", TRUE, "activatable", TRUE, NULL);
  clutter_actor_grab_key_focus (renderer->commandline);
}


static gboolean commandline_cancel_cb (ClutterActor *actor,
                                       ClutterEvent *event,
                                       gpointer      data)
{
  ClutterRenderer *renderer = CLUTTER_RENDERER (data);
  PinPointPoint *point = renderer->data->pp_slidep->data;

  if (clutter_event_type (event) == CLUTTER_KEY_PRESS &&
      (clutter_event_get_key_symbol (event) == CLUTTER_Escape ||
       clutter_event_get_key_symbol (event) == CLUTTER_Tab))
    {
      clutter_actor_grab_key_focus (renderer->data->pp_container);
      clutter_actor_animate (renderer->commandline,
                             CLUTTER_LINEAR, 500,
                             "opacity", (int)(0xff*0.33), NULL);
      g_object_set (renderer->commandline, "editable", FALSE, NULL);
      clutter_actor_animate (renderer->commandline_shading,
                             CLUTTER_LINEAR, 500,
                             "opacity", (int)(point->shading_opacity*0xff*0.33), NULL);
      return TRUE;
    }
  update_commandline_shading (renderer);
  return FALSE;
}

static gboolean commandline_action_cb (ClutterActor *actor,
                                       gpointer      data)
{
  ClutterRenderer *renderer = CLUTTER_RENDERER (data);
  PinPointPoint *point = renderer->data->pp_slidep->data;
  clutter_actor_grab_key_focus (renderer->data->pp_container);
  clutter_actor_animate (renderer->commandline,
                         CLUTTER_LINEAR, 500,
                         "opacity", (int)(0xff*0.33), NULL);
  g_object_set (renderer->commandline, "editable", FALSE, NULL);
  clutter_actor_animate (renderer->commandline_shading,
                         CLUTTER_LINEAR, 500,
                         "opacity", (int)(point->shading_opacity*0xff*0.33), NULL);

  action_slide (renderer);
  return FALSE;
}

static void commandline_notify_cb (ClutterActor *actor,
                                   GParamSpec   *pspec,
                                   gpointer      data)
{
  ClutterRenderer *renderer = CLUTTER_RENDERER (data);
  gfloat scale;
  scale = clutter_actor_get_width (renderer->data->pp_container) /
          (clutter_actor_get_width (actor) / 0.9);
  if (scale > 1.0)
    scale = 1.0;
  clutter_actor_set_scale (actor, scale, scale);
}

#if 0
static gboolean stage_motion (ClutterActor    *actor,
                              ClutterEvent    *event,
                              ClutterRenderer *renderer)
{
  gfloat stage_width, stage_height;

  if (hide_cursor)
    g_source_remove (hide_cursor);
  clutter_stage_show_cursor (CLUTTER_STAGE (actor));
  hide_cursor = g_timeout_add (500, hide_cursor_cb, actor);

  if (!pp_get_fullscreen (renderer))
    return FALSE;

  clutter_actor_get_size (renderer->stage, &stage_width, &stage_height);
#ifdef QUICK_ACCESS_LEFT
  if (event->motion.x < 8)
    {
      float d = event->motion.y / stage_height;
#else
  if (event->motion.y < 8)
    {
      float d = event->motion.x / stage_width;
#endif
      if (renderer->data->pp_slidep)
        {
          leave_slide (renderer, FALSE);
        }
      renderer->data->pp_slidep =
        g_list_nth (renderer->data->pp_slides,
                    g_list_length (renderer->data->pp_slides) * d);
      show_slide (renderer, FALSE);
    }

  return FALSE;
}
#endif

static void
clutter_renderer_init (PinPointRenderer   *pp_renderer,
                       const char         *pinpoint_file,
                       PinPointData       *data)
{
  ClutterRenderer *renderer = CLUTTER_RENDERER (pp_renderer);
  GFileMonitor *monitor;
  ClutterActor *box;

  renderer->data = data;
  renderer->rest_y = STARTPOS;

  box = data->pp_container;

  renderer->bg_rect = clutter_rectangle_new_with_color (&black);
  renderer->background = clutter_group_new ();
  renderer->midground = clutter_group_new ();
  renderer->foreground = clutter_group_new ();
  renderer->json_layer = clutter_group_new ();
  renderer->shading = clutter_rectangle_new_with_color (&black);
  renderer->commandline_shading = clutter_rectangle_new_with_color (&black);
  renderer->commandline = clutter_text_new ();
  clutter_actor_set_opacity (renderer->shading, 0x77);
  clutter_actor_set_opacity (renderer->commandline_shading, 0x77);

  clutter_container_add_actor (CLUTTER_CONTAINER (renderer->midground),
                               renderer->shading);
  clutter_container_add (CLUTTER_CONTAINER (box),
                         renderer->bg_rect,
                         renderer->background,
                         renderer->midground,
                         renderer->foreground,
                         renderer->json_layer,
                         renderer->commandline_shading,
                         renderer->commandline,
                         NULL);

  g_signal_connect (box, "key-press-event",
                    G_CALLBACK (key_pressed), renderer);
  g_signal_connect (box, "notify::width",
                    G_CALLBACK (box_resized), renderer);
  g_signal_connect (box, "notify::height",
                    G_CALLBACK (box_resized), renderer);
#if 0
  g_signal_connect (stage, "motion-event",
                    G_CALLBACK (stage_motion), renderer);
#endif
  g_signal_connect (renderer->commandline, "activate",
                    G_CALLBACK (commandline_action_cb), renderer);
  g_signal_connect (renderer->commandline, "captured-event",
                    G_CALLBACK (commandline_cancel_cb), renderer);
  g_signal_connect (renderer->commandline, "notify::width",
                    G_CALLBACK (commandline_notify_cb), renderer);


  renderer->path = g_strdup (pinpoint_file);
  if (renderer->path)
    {
      monitor = g_file_monitor (g_file_new_for_commandline_arg (pinpoint_file),
                                G_FILE_MONITOR_NONE, NULL, NULL);
      g_signal_connect (monitor, "changed", G_CALLBACK (file_changed),
                                            renderer);
    }

  renderer->bg_cache = g_hash_table_new_full (g_str_hash, g_str_equal,
                                              g_free, _destroy_surface);
}

static void
clutter_renderer_run (PinPointRenderer *renderer)
{
  show_slide (CLUTTER_RENDERER (renderer), FALSE);
}

static void
clutter_renderer_finalize (PinPointRenderer *pp_renderer)
{
  ClutterRenderer *renderer = CLUTTER_RENDERER (pp_renderer);

  if (renderer->reload_tag)
    g_source_remove (renderer->reload_tag);

  g_hash_table_unref (renderer->bg_cache);

  g_free (renderer->path);

  g_free (renderer);
}

static ClutterActor *
_clutter_get_texture (ClutterRenderer *renderer,
                      const char      *file)
{
  ClutterActor *source;

  source = g_hash_table_lookup (renderer->bg_cache, file);
  if (source)
    {
      return clutter_clone_new (source);
    }

  source = g_object_new (CLUTTER_TYPE_TEXTURE,
                         "filename", file,
                         "load-data-async", TRUE,
                         NULL);

  if (!source)
    return NULL;

  clutter_container_add_actor (CLUTTER_CONTAINER (renderer->data->pp_container),
                               source);
  clutter_actor_hide (source);

  g_hash_table_insert (renderer->bg_cache, (char *) g_strdup (file), source);

  return clutter_clone_new (source);
}


static gboolean
clutter_renderer_make_point (PinPointRenderer *pp_renderer,
                             PinPointPoint    *point)
{
  ClutterRenderer *renderer = CLUTTER_RENDERER (pp_renderer);
  ClutterPointData *data = point->data;
  const gchar *file = point->bg;
  gchar *full_path = NULL;
  ClutterColor color;
  gboolean ret;

  if (point->bg_type != PP_BG_COLOR && renderer->path && file)
    {
      gchar *dir = g_path_get_dirname (renderer->path);
      full_path = g_build_filename (dir, file, NULL);
      g_free (dir);

      file = full_path;
    }

  switch (point->bg_type)
    {
    case PP_BG_COLOR:
      {
        ret = clutter_color_from_string (&color, point->bg);
        if (ret)
          data->background = g_object_new (CLUTTER_TYPE_RECTANGLE,
                                           "color", &color,
                                           "width", 100.0,
                                           "height", 100.0,
                                           NULL);
      }
      break;
    case PP_BG_IMAGE:
      data->background = _clutter_get_texture (renderer, file);
      ret = TRUE;
      break;
    case PP_BG_VIDEO:
#ifdef USE_CLUTTER_GST
      data->background = clutter_gst_video_texture_new ();
      clutter_media_set_filename (CLUTTER_MEDIA (data->background), file);
      /* should pre-roll the video and set the size */
      clutter_actor_set_size (data->background, 400, 300);
      ret = TRUE;
#endif
      break;
    case PP_BG_SVG:
#ifdef USE_DAX
      {
        ClutterActor *aa, *svg;
        GError *error = NULL;

        aa = pp_super_aa_new ();
        pp_super_aa_set_resolution (PP_SUPER_AA (aa), 2, 2);
        svg = dax_actor_new_from_file (file, &error);
        mx_offscreen_set_pick_child (MX_OFFSCREEN (aa), TRUE);
        clutter_container_add_actor (CLUTTER_CONTAINER (aa),
                                     svg);

        data->background = aa;

        if (data->background == NULL)
          {
            g_warning ("Could not open SVG file %s: %s",
                       file, error->message);
            g_clear_error (&error);
          }
        ret = data->background != NULL;
      }
#endif
      break;
    default:
      g_assert_not_reached();
    }

  g_free (full_path);

  if (data->background)
    {
      clutter_container_add_actor (CLUTTER_CONTAINER (renderer->background),
                                   data->background);
      clutter_actor_set_opacity (data->background, 0);
    }

  clutter_color_from_string (&color, point->text_color);

  if (point->use_markup)
    {
      data->text = g_object_new (CLUTTER_TYPE_TEXT,
                                 "font-name", point->font,
                                 "text", point->text,
                                 "line-alignment", point->text_align,
                                 "color", &color,
                                 "use-markup", TRUE,
                                 NULL);
    }
  else
    {
      data->text = g_object_new (CLUTTER_TYPE_TEXT,
                                 "font-name", point->font,
                                 "text", point->text,
                                 "line-alignment", point->text_align,
                                 "color", &color,
                                 NULL);
    }

  clutter_container_add_actor (CLUTTER_CONTAINER (renderer->foreground),
                               data->text);

  clutter_actor_set_position (data->text, RESTX, renderer->rest_y);
  data->rest_y = renderer->rest_y;
  renderer->rest_y += clutter_actor_get_height (data->text);
  clutter_actor_set_depth (data->text, RESTDEPTH);

  return ret;
}

static void *
clutter_renderer_allocate_data (PinPointRenderer *renderer)
{
  ClutterPointData *data = g_slice_new0 (ClutterPointData);
  data->renderer = renderer;
  return data;
}

static void
clutter_renderer_free_data (PinPointRenderer *renderer,
                            void             *datap)
{
  ClutterPointData *data = datap;

  if (data->background)
    clutter_actor_destroy (data->background);
  if (data->text)
    clutter_actor_destroy (data->text);
  if (data->json_slide)
    clutter_actor_destroy (data->json_slide);
  if (data->script)
    g_object_unref (data->script);
  g_slice_free (ClutterPointData, data);
}



static gboolean
key_pressed (ClutterActor    *actor,
             ClutterEvent    *event,
             ClutterRenderer *renderer)
{
  if (event) /* There is no event for the first triggering */
  switch (clutter_event_get_key_symbol (event))
    {
      case CLUTTER_Left:
      case CLUTTER_Up:
        if (renderer->data->pp_slidep && renderer->data->pp_slidep->prev)
          {
            leave_slide (renderer, TRUE);
            renderer->data->pp_slidep = renderer->data->pp_slidep->prev;
            show_slide (renderer, TRUE);
          }
        break;
      case CLUTTER_BackSpace:
      case CLUTTER_Prior:
        if (renderer->data->pp_slidep && renderer->data->pp_slidep->prev)
          {
            leave_slide (renderer, TRUE);
            renderer->data->pp_slidep = renderer->data->pp_slidep->prev;
            show_slide (renderer, TRUE);
          }
        break;
      case CLUTTER_Right:
      case CLUTTER_space:
      case CLUTTER_Next:
      case CLUTTER_Down:
        if (renderer->data->pp_slidep && renderer->data->pp_slidep->next)
          {
            leave_slide (renderer, FALSE);
            renderer->data->pp_slidep = renderer->data->pp_slidep->next;
            show_slide (renderer, FALSE);
          }
        break;
      case CLUTTER_Return:
        action_slide (renderer);
        break;
      case CLUTTER_Tab:
        activate_commandline (renderer);
        break;
    }
  return TRUE;
}


static void leave_slide (ClutterRenderer *renderer,
                         gboolean         backwards)
{
  PinPointPoint *point = renderer->data->pp_slidep->data;
  ClutterPointData *data = point->data;

  if (!point->transition)
    {
      clutter_actor_animate (data->text,
                             CLUTTER_LINEAR, 2000,
                             "depth", RESTDEPTH,
                             "scale-x", 1.0,
                             "scale-y", 1.0,
                             "x", RESTX,
                             "y", data->rest_y,
                             NULL);
      if (data->background)
        {
          clutter_actor_animate (data->background,
                                 CLUTTER_LINEAR, 1000,
                                 "opacity", 0x0,
                                 NULL);
#ifdef USE_CLUTTER_GST
          if (CLUTTER_GST_IS_VIDEO_TEXTURE (data->background))
            {
              clutter_media_set_playing (CLUTTER_MEDIA (data->background), FALSE);
            }
#endif
#ifdef USE_DAX
          if (DAX_IS_ACTOR (data->background))
            {
              dax_actor_set_playing (DAX_ACTOR (data->background), FALSE);
            }
          else if (PP_IS_SUPER_AA (data->background))
            {
              ClutterActor *actor;

              actor = mx_offscreen_get_child (MX_OFFSCREEN (data->background));
              dax_actor_set_playing (DAX_ACTOR (actor), FALSE);
            }
#endif
        }
    }
  else
    {
      if (data->script)
        {
          if (backwards)
            clutter_state_set_state (data->state, "pre");
          else 
            clutter_state_set_state (data->state, "post");
        }
    }
}

static void state_completed (ClutterState *state, gpointer user_data)
{
  PinPointPoint    *point = user_data;
  ClutterPointData *data = point->data;
  const gchar *new_state = clutter_state_get_state (state);

  if (new_state == g_intern_static_string ("post") ||
      new_state == g_intern_static_string ("pre"))
    {
      clutter_actor_hide (data->json_slide);
      if (data->background2)
        {
          clutter_actor_reparent (data->text, CLUTTER_RENDERER (data->renderer)->foreground);

          g_object_set (data->text,
                        "depth", RESTDEPTH,
                        "scale-x", 1.0,
                        "scale-y", 1.0,
                        "x", RESTX,
                        "y", data->rest_y,
                        NULL);
          clutter_actor_set_opacity (data->background, 0);
        }
    }
}

static void
action_slide (ClutterRenderer *renderer)
{
  PinPointPoint *point;
  ClutterPointData *data;
  const char *command = NULL;

  if (!renderer->data->pp_slidep)
    return;
 
  point = renderer->data->pp_slidep->data;
  data = point->data;

  if (data->state)
    clutter_state_set_state (data->state, "action");

  command = clutter_text_get_text (CLUTTER_TEXT (renderer->commandline));
  if (command && *command)
    {
      char *tmp = g_strdup_printf ("%s &", command);
      g_print ("running: %s\n", tmp);
      system (tmp);
      g_free (tmp);
    }
}

static gchar *pp_lookup_transition (const gchar *transition)
{
  int i;
  gchar *dirs[] ={ "", "./transitions/", PKGDATADIR, NULL};
  for (i = 0; dirs[i]; i++)
    {
      gchar *path = g_strdup_printf ("%s%s.json", dirs[i], transition);
      if (g_file_test (path, G_FILE_TEST_EXISTS))
        return path;
      g_free (path);
    }
  return NULL;
}


static void update_commandline_shading (ClutterRenderer *renderer)
{
   ClutterColor color;
   float text_x, text_y, text_width, text_height;
   float shading_x, shading_y, shading_width, shading_height;
   const char *command;
   PinPointPoint *point;
   point = renderer->data->pp_slidep->data;
   clutter_actor_get_size (renderer->commandline, &text_width, &text_height);
   clutter_actor_get_position (renderer->commandline, &text_x, &text_y);
   pp_get_shading_position_size (clutter_actor_get_width (renderer->data->pp_container),
                                 clutter_actor_get_height (renderer->data->pp_container),
                                 text_x, text_y,
                                 text_width, text_height,
                                 1.0,
                                 &shading_x, &shading_y,
                                 &shading_width, &shading_height);
   
   clutter_color_from_string (&color, point->shading_color);
   g_object_set (renderer->commandline_shading,
          "x", shading_x,
          "y", shading_y,
          NULL);
   command = clutter_text_get_text (CLUTTER_TEXT (renderer->commandline));
   clutter_actor_animate (renderer->commandline_shading,
          CLUTTER_EASE_OUT_QUINT, 1000,
          "opacity", command && *command?(int)(point->shading_opacity*255*0.33):0,
          "color", &color,
          "width", shading_width,
          "height", shading_height,
          NULL);
  }

static void
show_slide (ClutterRenderer *renderer, gboolean backwards)
{
  PinPointPoint *point;
  ClutterPointData *data;
  ClutterColor color;
  gfloat width, height;
  ClutterActorBox box;

  if (!renderer->data->pp_slidep)
    return;

  clutter_actor_get_allocation_box (renderer->data->pp_container, &box);
  width = box.x2 - box.x1;
  height = box.y2 - box.y1;

  point = renderer->data->pp_slidep->data;
  data = point->data;

  if (point->stage_color)
    {
      clutter_color_from_string (&color, point->stage_color);
      clutter_rectangle_set_color (CLUTTER_RECTANGLE (renderer->bg_rect),
                                   &color);
    }

  if (renderer->bg_rect)
    clutter_actor_set_size (renderer->bg_rect, width, height);

  if (data->background)
    {
      float bg_x, bg_y, bg_width, bg_height, bg_scale_x, bg_scale_y;
      if (CLUTTER_IS_RECTANGLE (data->background))
        {
          bg_width = width;
          bg_height = height;
          clutter_actor_set_size (data->background, bg_width, bg_height);
        }
      else
        {
          clutter_actor_get_size (data->background, &bg_width, &bg_height);
        }

      pp_get_background_position_scale (point,
                                        width, height,
                                        bg_width, bg_height,
                                        &bg_x, &bg_y, &bg_scale_x, &bg_scale_y);

      clutter_actor_set_scale (data->background, bg_scale_x, bg_scale_y);
      clutter_actor_set_position (data->background, bg_x, bg_y);

#ifdef USE_CLUTTER_GST
      if (CLUTTER_GST_IS_VIDEO_TEXTURE (data->background))
        {
          clutter_media_set_progress (CLUTTER_MEDIA (data->background), 0.0);
          clutter_media_set_playing (CLUTTER_MEDIA (data->background), TRUE);
        }
      else
#endif
#ifdef USE_DAX
     if (DAX_IS_ACTOR (data->background))
       {
         dax_actor_set_playing (DAX_ACTOR (data->background), TRUE);
       }
     else if (PP_IS_SUPER_AA (data->background))
       {
         ClutterActor *actor;

         actor = mx_offscreen_get_child (MX_OFFSCREEN (data->background));
         dax_actor_set_playing (DAX_ACTOR (actor), TRUE);
       }
     else
#endif
       {
       }
    }

  if (!point->transition)
    {
      clutter_actor_animate (renderer->foreground,
                             CLUTTER_LINEAR, 500,
                             "opacity", 255,
                             NULL);
      clutter_actor_animate (renderer->midground,
                             CLUTTER_LINEAR, 500,
                             "opacity", 255,
                             NULL);
      clutter_actor_animate (renderer->background,
                             CLUTTER_LINEAR, 500,
                             "opacity", 255,
                             NULL);

      
      if (point->text && *point->text)
        {
         float text_x, text_y, text_width, text_height, text_scale;
         float shading_x, shading_y, shading_width, shading_height;

         clutter_actor_get_size (data->text, &text_width, &text_height);
         pp_get_text_position_scale (point,
                                     width, height,
                                     text_width, text_height,
                                     &text_x, &text_y,
                                     &text_scale);
         pp_get_shading_position_size (width, height,
                                       text_x, text_y,
                                       text_width, text_height,
                                       text_scale,
                                       &shading_x, &shading_y,
                                       &shading_width, &shading_height);
         clutter_color_from_string (&color, point->shading_color);

         clutter_actor_animate (data->text,
                                CLUTTER_EASE_OUT_QUINT, 1000,
                                "depth", 0.0,
                                "scale-x", text_scale,
                                "scale-y", text_scale,
                                "x", text_x,
                                "y", text_y,
                                NULL);

         clutter_actor_animate (renderer->shading,
                CLUTTER_EASE_OUT_QUINT, 1000,
                "x", shading_x,
                "y", shading_y,
                "opacity", (int)(point->shading_opacity*255),
                "color", &color,
                "width", shading_width,
                "height", shading_height,
                NULL);
        }
      else
        {
           clutter_actor_animate (renderer->shading,
                  CLUTTER_LINEAR, 500,
                  "opacity", 0,
                  "width", 0,
                  "height", 0,
                  NULL);
        }


      if (data->background)
         clutter_actor_animate (data->background,
                                CLUTTER_LINEAR, 1000,
                                "opacity", 0xff,
                                NULL);
    }
  else
    {
      GError *error = NULL;
      /* fade out global group of texts when using a custom .json template */
      clutter_actor_animate (renderer->foreground,
                             CLUTTER_LINEAR, 500,
                             "opacity", 0,
                             NULL);
      clutter_actor_animate (renderer->midground,
                             CLUTTER_LINEAR, 500,
                             "opacity", 0,
                             NULL);
      clutter_actor_animate (renderer->background,
                             CLUTTER_LINEAR, 500,
                             "opacity", 0,
                             NULL);
      if (!data->script)
        {
          gchar *path = pp_lookup_transition (point->transition);
          data->script = clutter_script_new ();
          clutter_script_load_from_file (data->script, path, &error);
          g_free (path);
          data->foreground = CLUTTER_ACTOR (clutter_script_get_object (data->script, "foreground"));
          data->midground = CLUTTER_ACTOR (clutter_script_get_object (data->script, "midground"));
          data->background2 = CLUTTER_ACTOR (clutter_script_get_object (data->script, "background"));
          data->state = CLUTTER_STATE (clutter_script_get_object (data->script, "state"));
          data->json_slide = CLUTTER_ACTOR (clutter_script_get_object (data->script, "actor"));
          clutter_container_add_actor (CLUTTER_CONTAINER (renderer->json_layer), data->json_slide);
          g_signal_connect (data->state, "completed", G_CALLBACK (state_completed), point);
          clutter_state_warp_to_state (data->state, "pre");

          if (data->background2) /* parmanently steal background */
            {
              clutter_actor_reparent (data->background, data->background2);
            }
        }

      clutter_actor_set_size (data->json_slide, width, height);
      clutter_actor_set_size (data->foreground, width, height);
      clutter_actor_set_size (data->background2, width, height);

      if (!data->json_slide)
        {
          g_warning ("failed to load transition %s %s\n", point->transition, error?error->message:"");
          return;
        }
      if (data->foreground)
        {
          clutter_actor_reparent (data->text, data->foreground);
        }
              clutter_actor_set_opacity (data->background, 255);

      {
       float text_x, text_y, text_width, text_height, text_scale;

       clutter_actor_get_size (data->text, &text_width, &text_height);
       pp_get_text_position_scale (point,
                                   width, height,
                                   text_width, text_height,
                                   &text_x, &text_y,
                                   &text_scale);
       g_object_set (data->text,
                     "depth", 0.0,
                     "scale-x", text_scale,
                     "scale-y", text_scale,
                     "x", text_x,
                     "y", text_y,
                     NULL);


       if (clutter_actor_get_width (data->text) > 1.0)
         {
           ClutterColor color;
           float shading_x, shading_y, shading_width, shading_height;
           clutter_color_from_string (&color, point->shading_color);


           pp_get_shading_position_size (width, height,
                                         text_x, text_y,
                                         text_width, text_height,
                                         text_scale,
                                         &shading_x, &shading_y,
                                         &shading_width, &shading_height);
           if (!data->shading)
             {
               data->shading = clutter_rectangle_new_with_color (&black);
               clutter_container_add_actor (CLUTTER_CONTAINER (data->midground),
                                            data->shading);
               clutter_actor_set_size (data->midground, width, height);
             }
           g_object_set (data->shading,
                  "depth", -0.01,
                  "x", shading_x,
                  "y", shading_y,
                  "opacity", (int)(point->shading_opacity*255),
                  "color", &color,
                  "width", shading_width,
                  "height", shading_height,
                  NULL);
         }
       else /* no text, fade out shading */
         if (data->shading)
           g_object_set (data->shading, "opacity", 0, NULL);
       if (data->foreground)
         {
           clutter_actor_reparent (data->text, data->foreground);
         }
      }

      if (!backwards)
        clutter_actor_raise_top (data->json_slide);
      clutter_actor_show (data->json_slide);
      clutter_state_set_state (data->state, "show");
    }


  /* render potentially executed commands */
  {
   float text_x, text_y, text_width, text_height;

   clutter_color_from_string (&color, point->text_color);
   g_object_set (renderer->commandline,
                 "font-name", point->font,
                 "text", point->command?point->command:"",
                 "color", &color,
                 NULL);
   color.alpha *= 0.33;
   g_object_set (renderer->commandline,
                 "selection-color", &color,
                 NULL);

   clutter_actor_get_size (renderer->commandline, &text_width, &text_height);
   if (point->position == CLUTTER_GRAVITY_SOUTH ||
       point->position == CLUTTER_GRAVITY_SOUTH_WEST)
     text_y = height * 0.05;
   else
     text_y = height * 0.95 - text_height;
   text_x = width * 0.05;


   g_object_set (renderer->commandline,
          "x", text_x,
          "y", text_y,
          NULL);
   clutter_actor_animate (renderer->commandline,
          CLUTTER_EASE_OUT_QUINT, 1000,
          "opacity", point->command?(gint)(0xff*0.33):0,
          NULL);

   update_commandline_shading (renderer);
  }
}

static gboolean
resize (gpointer data)
{
  ClutterRenderer *renderer = data;
  renderer->reload_tag = 0;
  show_slide (renderer, FALSE);
  return FALSE;
}

static void
box_resized (ClutterActor    *actor,
             GParamSpec      *pspec,
             ClutterRenderer *renderer)
{
  if (!renderer->reload_tag)
    renderer->reload_tag = g_idle_add_full (G_PRIORITY_HIGH_IDLE,
                                            (GSourceFunc)resize,
                                            renderer,
                                            NULL);
}

static gboolean
reload (gpointer data)
{
  ClutterRenderer *renderer = data;
  char   *text = NULL;
  if (!g_file_get_contents (renderer->path, &text, NULL, NULL))
    g_error ("failed to load slides from %s\n", renderer->path);
  renderer->rest_y = STARTPOS;
  pp_parse_slides (PINPOINT_RENDERER (renderer), renderer->data, text);
  g_free (text);
  show_slide(renderer, FALSE);
  renderer->reload_tag = 0;
  return FALSE;
}

static void
file_changed (GFileMonitor      *monitor,
              GFile             *file,
              GFile             *other_file,
              GFileMonitorEvent  event_type,
              ClutterRenderer   *renderer)
{
  if (renderer->reload_tag)
    g_source_remove (renderer->reload_tag);
  renderer->reload_tag = g_timeout_add (200, reload, renderer);
}

static ClutterRenderer clutter_renderer_vtable =
{
  .renderer =
    {
      .init = clutter_renderer_init,
      .run = clutter_renderer_run,
      .finalize = clutter_renderer_finalize,
      .make_point = clutter_renderer_make_point,
      .allocate_data = clutter_renderer_allocate_data,
      .free_data = clutter_renderer_free_data
    }
};

PinPointRenderer *pp_clutter_renderer (void)
{
  return g_memdup (&clutter_renderer_vtable, sizeof (ClutterRenderer));
}
