/*
 * Copyright (C) 2009,2010  Red Hat, Inc.
 * Copyright (C) 2010  Google, Inc.
 *
 *  This is part of HarfBuzz, a text shaping library.
 *
 * Permission is hereby granted, without written agreement and without
 * license or royalty fees, to use, copy, modify, and distribute this
 * software and its documentation for any purpose, provided that the
 * above copyright notice and the following two paragraphs appear in
 * all copies of this software.
 *
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES
 * ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN
 * IF THE COPYRIGHT HOLDER HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * THE COPYRIGHT HOLDER SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE COPYRIGHT HOLDER HAS NO OBLIGATION TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 * Red Hat Author(s): Behdad Esfahbod
 * Google Author(s): Behdad Esfahbod
 */

#include "hb-ot-shape-private.hh"

#include "hb-ot-map-private.hh"

HB_BEGIN_DECLS


/* XXX vertical */
hb_tag_t default_features[] = {
  HB_TAG('c','a','l','t'),
  HB_TAG('c','c','m','p'),
  HB_TAG('c','l','i','g'),
  HB_TAG('c','s','w','h'),
  HB_TAG('c','u','r','s'),
  HB_TAG('k','e','r','n'),
  HB_TAG('l','i','g','a'),
  HB_TAG('l','o','c','l'),
  HB_TAG('m','a','r','k'),
  HB_TAG('m','k','m','k'),
  HB_TAG('r','l','i','g')
};

static void
hb_ot_shape_collect_features (hb_ot_shape_context_t *c)
{
  switch (c->original_direction) {
    case HB_DIRECTION_LTR:
      c->map->add_bool_feature (HB_TAG ('l','t','r','a'));
      c->map->add_bool_feature (HB_TAG ('l','t','r','m'));
      break;
    case HB_DIRECTION_RTL:
      c->map->add_bool_feature (HB_TAG ('r','t','l','a'));
      c->map->add_bool_feature (HB_TAG ('r','t','l','m'), false);
      break;
    case HB_DIRECTION_TTB:
    case HB_DIRECTION_BTT:
    default:
      break;
  }

  for (unsigned int i = 0; i < ARRAY_LENGTH (default_features); i++)
    c->map->add_bool_feature (default_features[i]);

  /* complex */

  for (unsigned int i = 0; i < c->num_features; i++) {
    const hb_feature_t *feature = &c->features[i];
    c->map->add_feature (feature->tag, feature->value, (feature->start == 0 && feature->end == (unsigned int) -1));
  }
}


static void
hb_ot_shape_setup_masks (hb_ot_shape_context_t *c)
{
  hb_mask_t global_mask = c->map->get_global_mask ();
  if (global_mask)
    c->buffer->set_masks (global_mask, global_mask, 0, (unsigned int) -1);

  for (unsigned int i = 0; i < c->num_features; i++)
  {
    hb_feature_t *feature = &c->features[i];
    if (!(feature->start == 0 && feature->end == (unsigned int)-1)) {
      unsigned int shift;
      hb_mask_t mask = c->map->get_mask (feature->tag, &shift);
      c->buffer->set_masks (feature->value << shift, mask, feature->start, feature->end);
    }
  }

  /* complex */
}


static void
hb_ot_substitute_complex (hb_ot_shape_context_t *c)
{
  if (!hb_ot_layout_has_substitution (c->face))
    return;

  c->map->substitute (c);

  c->applied_substitute_complex = TRUE;
  return;
}

static void
hb_ot_position_complex (hb_ot_shape_context_t *c)
{

  if (!hb_ot_layout_has_positioning (c->face))
    return;

  c->map->position (c);

  hb_ot_layout_position_finish (c->font, c->face, c->buffer);

  c->applied_position_complex = TRUE;
  return;
}


/* Main shaper */

/* Prepare */

static inline hb_bool_t
is_variation_selector (hb_codepoint_t unicode)
{
  return unlikely ((unicode >=  0x180B && unicode <=  0x180D) || /* MONGOLIAN FREE VARIATION SELECTOR ONE..THREE */
		   (unicode >=  0xFE00 && unicode <=  0xFE0F) || /* VARIATION SELECTOR-1..16 */
		   (unicode >= 0xE0100 && unicode <= 0xE01EF));  /* VARIATION SELECTOR-17..256 */
}

static void
hb_form_clusters (hb_ot_shape_context_t *c)
{
  unsigned int count = c->buffer->len;
  for (unsigned int i = 1; i < count; i++)
    if (c->buffer->unicode->v.get_general_category (c->buffer->info[i].codepoint) == HB_CATEGORY_NON_SPACING_MARK)
      c->buffer->info[i].cluster = c->buffer->info[i - 1].cluster;
}

static void
hb_ensure_native_direction (hb_ot_shape_context_t *c)
{
  hb_direction_t direction = c->buffer->props.direction;

  /* TODO vertical */
  if (HB_DIRECTION_IS_HORIZONTAL (direction) &&
      direction != _hb_script_get_horizontal_direction (c->buffer->props.script))
  {
    hb_buffer_reverse_clusters (c->buffer);
    c->buffer->props.direction = HB_DIRECTION_REVERSE (c->buffer->props.direction);
  }
}


/* Substitute */

static void
hb_mirror_chars (hb_ot_shape_context_t *c)
{
  hb_unicode_get_mirroring_func_t get_mirroring = c->buffer->unicode->v.get_mirroring;

  if (HB_DIRECTION_IS_FORWARD (c->buffer->props.direction))
    return;

  hb_mask_t rtlm_mask = c->map->get_mask (HB_TAG ('r','t','l','m'));

  unsigned int count = c->buffer->len;
  for (unsigned int i = 0; i < count; i++) {
    hb_codepoint_t codepoint = get_mirroring (c->buffer->info[i].codepoint);
    if (likely (codepoint == c->buffer->info[i].codepoint))
      c->buffer->info[i].mask |= rtlm_mask;
    else
      c->buffer->info[i].codepoint = codepoint;
  }
}

static void
hb_map_glyphs (hb_font_t    *font,
	       hb_face_t    *face,
	       hb_buffer_t  *buffer)
{
  if (unlikely (!buffer->len))
    return;

  buffer->clear_output ();
  unsigned int count = buffer->len - 1;
  for (buffer->i = 0; buffer->i < count;) {
    if (unlikely (is_variation_selector (buffer->info[buffer->i + 1].codepoint))) {
      buffer->add_output_glyph (hb_font_get_glyph (font, face, buffer->info[buffer->i].codepoint, buffer->info[buffer->i + 1].codepoint));
      buffer->i++;
    } else {
      buffer->add_output_glyph (hb_font_get_glyph (font, face, buffer->info[buffer->i].codepoint, 0));
    }
  }
  if (likely (buffer->i < buffer->len))
    buffer->add_output_glyph (hb_font_get_glyph (font, face, buffer->info[buffer->i].codepoint, 0));
  buffer->swap ();
}

static void
hb_substitute_default (hb_ot_shape_context_t *c)
{
  hb_map_glyphs (c->font, c->face, c->buffer);
}

static void
hb_substitute_complex_fallback (hb_ot_shape_context_t *c HB_UNUSED)
{
  /* TODO Arabic */
}


/* Position */

static void
hb_position_default (hb_ot_shape_context_t *c)
{
  hb_buffer_clear_positions (c->buffer);

  unsigned int count = c->buffer->len;
  for (unsigned int i = 0; i < count; i++) {
    hb_glyph_metrics_t metrics;
    hb_font_get_glyph_metrics (c->font, c->face, c->buffer->info[i].codepoint, &metrics);
    c->buffer->pos[i].x_advance = metrics.x_advance;
    c->buffer->pos[i].y_advance = metrics.y_advance;
  }
}

static void
hb_position_complex_fallback (hb_ot_shape_context_t *c HB_UNUSED)
{
  /* TODO Mark pos */
}

static void
hb_truetype_kern (hb_ot_shape_context_t *c)
{
  /* TODO Check for kern=0 */
  unsigned int count = c->buffer->len;
  for (unsigned int i = 1; i < count; i++) {
    hb_position_t kern, kern1, kern2;
    kern = hb_font_get_kerning (c->font, c->face, c->buffer->info[i - 1].codepoint, c->buffer->info[i].codepoint);
    kern1 = kern >> 1;
    kern2 = kern - kern1;
    c->buffer->pos[i - 1].x_advance += kern1;
    c->buffer->pos[i].x_advance += kern2;
    c->buffer->pos[i].x_offset += kern2;
  }
}

static void
hb_position_complex_fallback_visual (hb_ot_shape_context_t *c)
{
  hb_truetype_kern (c);
}


/* Do it! */

static void
hb_ot_shape_internal (hb_ot_shape_context_t *c)
{
  hb_ot_shape_setup_masks (c);

  hb_form_clusters (c);

  /* SUBSTITUTE */
  {
    c->buffer->clear_masks ();

    /* Mirroring needs to see the original direction */
    hb_mirror_chars (c);

    hb_ensure_native_direction (c);

    hb_substitute_default (c);

    hb_ot_substitute_complex (c);

    if (!c->applied_substitute_complex)
      hb_substitute_complex_fallback (c);
  }

  /* POSITION */
  {
    c->buffer->clear_masks ();

    hb_position_default (c);

    hb_ot_position_complex (c);

    hb_bool_t position_fallback = !c->applied_position_complex;
    if (position_fallback)
      hb_position_complex_fallback (c);

    if (HB_DIRECTION_IS_BACKWARD (c->buffer->props.direction))
      hb_buffer_reverse (c->buffer);

    if (position_fallback)
      hb_position_complex_fallback_visual (c);
  }

  c->buffer->props.direction = c->original_direction;
}

void
hb_ot_shape (hb_font_t    *font,
	     hb_face_t    *face,
	     hb_buffer_t  *buffer,
	     hb_feature_t *features,
	     unsigned int  num_features)
{
  hb_ot_shape_context_t c = {font, face, buffer, features, num_features};
  hb_ot_map_t map;

  /* Setup transient context members */
  c.original_direction = buffer->props.direction;
  c.map = &map;

  hb_ot_shape_collect_features (&c);
  c.map->compile (&c);

  hb_ot_shape_internal (&c);
}


HB_END_DECLS
