// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////
//~ rjf: Helpers

internal FP_FT_Font
fp_ft_font_from_handle(FP_Handle handle)
{
  FP_FT_Font result = {(FT_Face)handle.u64[0]};
  return result;
}

internal FP_Handle
fp_ft_handle_from_font(FP_FT_Font font)
{
  FP_Handle result = {(U64)font.face};
  return result;
}

internal FT_Face
fp_ft_face_from_codepoint(FT_Face primary_face, U32 codepoint, FT_UInt *glyph_index_out)
{
  FT_Face result = primary_face;
  FT_UInt glyph_index = FT_Get_Char_Index(primary_face, codepoint);
  U64 missing_slot_idx = codepoint%ArrayCount(fp_ft_state->missing_codepoint_cache);
  if(glyph_index == 0 && codepoint >= 0x80 && fp_ft_state->missing_codepoint_cache[missing_slot_idx] != codepoint+1)
  {
    //- rjf: try already-opened fallback faces
    B32 found = 0;
    for(FP_FT_FallbackFace *n = fp_ft_state->first_fallback_face; n != 0; n = n->next)
    {
      FT_UInt fallback_glyph_index = FT_Get_Char_Index(n->face, codepoint);
      if(fallback_glyph_index != 0)
      {
        result = n->face;
        glyph_index = fallback_glyph_index;
        found = 1;
        break;
      }
    }
    
    //- rjf: ask fontconfig for a system font covering this codepoint
    if(!found && fp_ft_state->fc_config != 0)
    {
      FcPattern *pattern = FcPatternCreate();
      FcCharSet *charset = FcCharSetCreate();
      FcCharSetAddChar(charset, codepoint);
      FcPatternAddCharSet(pattern, FC_CHARSET, charset);
      FcPatternAddBool(pattern, FC_SCALABLE, FcTrue);
      FcConfigSubstitute(fp_ft_state->fc_config, pattern, FcMatchPattern);
      FcDefaultSubstitute(pattern);
      FcResult fc_result = FcResultNoMatch;
      FcPattern *match = FcFontMatch(fp_ft_state->fc_config, pattern, &fc_result);
      if(match != 0)
      {
        FcChar8 *file = 0;
        int face_idx = 0;
        FcCharSet *match_charset = 0;
        if(FcPatternGetCharSet(match, FC_CHARSET, 0, &match_charset) == FcResultMatch &&
           FcCharSetHasChar(match_charset, codepoint) &&
           FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch)
        {
          FcPatternGetInteger(match, FC_INDEX, 0, &face_idx);
          FT_Face face = 0;
          if(FT_New_Face(fp_ft_state->library, (char *)file, face_idx, &face) == 0)
          {
            FT_UInt fallback_glyph_index = FT_Get_Char_Index(face, codepoint);
            if(FT_IS_SCALABLE(face) && fallback_glyph_index != 0)
            {
              FP_FT_FallbackFace *n = push_array(fp_ft_state->arena, FP_FT_FallbackFace, 1);
              n->face = face;
              SLLQueuePush(fp_ft_state->first_fallback_face, fp_ft_state->last_fallback_face, n);
              result = face;
              glyph_index = fallback_glyph_index;
              found = 1;
            }
            else
            {
              FT_Done_Face(face);
            }
          }
        }
        FcPatternDestroy(match);
      }
      FcCharSetDestroy(charset);
      FcPatternDestroy(pattern);
    }
    
    //- rjf: no font has it -> remember, to avoid repeated system queries
    if(!found)
    {
      fp_ft_state->missing_codepoint_cache[missing_slot_idx] = codepoint+1;
    }
  }
  if(glyph_index_out != 0)
  {
    *glyph_index_out = glyph_index;
  }
  return result;
}

////////////////////////////////
//~ rjf: Backend Implementations

fp_hook void
fp_init(void)
{
  Arena *arena = arena_alloc();
  fp_ft_state = push_array(arena, FP_FT_State, 1);
  fp_ft_state->arena = arena;
  FT_Init_FreeType(&fp_ft_state->library);
  fp_ft_state->fc_config = FcInitLoadConfigAndFonts();
}

fp_hook FP_Handle
fp_font_open(String8 path)
{
  Temp scratch = scratch_begin(0, 0);
  String8 path_copy = push_str8_copy(scratch.arena, path);
  FP_FT_Font font = {0};
  FT_New_Face(fp_ft_state->library, (char *)path_copy.str, 0, &font.face);
  FP_Handle handle = fp_ft_handle_from_font(font);
  scratch_end(scratch);
  return handle;
}

fp_hook FP_Handle
fp_font_open_from_static_data_string(String8 *data_ptr)
{
  FP_FT_Font font = {0};
  FT_New_Memory_Face(fp_ft_state->library, data_ptr->str, (FT_Long)data_ptr->size, 0, &font.face);
  FP_Handle handle = fp_ft_handle_from_font(font);
  return handle;
}

fp_hook void
fp_font_close(FP_Handle handle)
{
  FP_FT_Font font = fp_ft_font_from_handle(handle);
  if(font.face != 0)
  {
    FT_Done_Face(font.face);
  }
}

fp_hook FP_Metrics
fp_metrics_from_font(FP_Handle handle)
{
  FP_FT_Font font = fp_ft_font_from_handle(handle);
  FP_Metrics result = {0};
  if(font.face != 0)
  {
    result.design_units_per_em = (F32)(font.face->units_per_EM);
    result.ascent              = (F32)font.face->ascender;
    result.descent             = -(F32)font.face->descender;
    result.line_gap            = (F32)(font.face->height - font.face->ascender + font.face->descender);
    result.capital_height      = (F32)(font.face->ascender);
  }
  return result;
}

fp_hook FP_RasterResult
fp_raster(Arena *arena, FP_Handle handle, F32 size, FP_RasterFlags flags, String8 string)
{
  ProfBeginFunction();
  FP_FT_Font font = fp_ft_font_from_handle(handle);
  FP_RasterResult result = {0};
  if(font.face != 0)
  {
    Temp scratch = scratch_begin(&arena, 1);
    
    //- rjf: unpack font
    FT_Face face = font.face;
    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)((96.f/72.f) * size));
    S64 ascent  = face->size->metrics.ascender >> 6;
    S64 descent = abs_s64(face->size->metrics.descender >> 6);
    S64 height  = face->size->metrics.height >> 6;
    
    //- rjf: unpack string, pick face (primary or fallback) & glyph per codepoint
    String32 string32 = str32_from_8(scratch.arena, string);
    FT_Face *glyph_faces = push_array(scratch.arena, FT_Face, string32.size);
    FT_UInt *glyph_indices = push_array(scratch.arena, FT_UInt, string32.size);
    FT_UInt pixel_size = (FT_UInt)((96.f/72.f) * size);
    for EachIndex(idx, string32.size)
    {
      glyph_faces[idx] = fp_ft_face_from_codepoint(face, string32.str[idx], &glyph_indices[idx]);
    }
    
    //- rjf: measure
    S32 total_width = 0;
    for EachIndex(idx, string32.size)
    {
      FT_Face glyph_face = glyph_faces[idx];
      if(glyph_face != face)
      {
        FT_Set_Pixel_Sizes(glyph_face, 0, pixel_size);
      }
      FT_Load_Glyph(glyph_face, glyph_indices[idx], FT_LOAD_DEFAULT);
      total_width += (glyph_face->glyph->advance.x >> 6);
    }
    
    //- rjf: allocate & fill atlas w/ rasterization
    Vec2S16 dim = {(S16)total_width+1, height+1};
    U64 atlas_size = dim.x * dim.y * 4;
    U8 *atlas = push_array(arena, U8, atlas_size);
    S32 baseline = ascent;
    S32 atlas_write_x = 0;
    for EachIndex(idx, string32.size)
    {
      FT_Face glyph_face = glyph_faces[idx];
      if(glyph_face != face)
      {
        FT_Set_Pixel_Sizes(glyph_face, 0, pixel_size);
      }
      FT_Load_Glyph(glyph_face, glyph_indices[idx], FT_LOAD_RENDER);
      FT_Bitmap *bmp = &glyph_face->glyph->bitmap;
      S32 top = glyph_face->glyph->bitmap_top;
      S32 left = glyph_face->glyph->bitmap_left;
      if(bmp->pixel_mode == FT_PIXEL_MODE_GRAY)
      {
        for(S32 row = 0; row < (S32)bmp->rows; row += 1)
        {
          S32 y = baseline - top + row;
          if(y < 0 || dim.y <= y) { continue; }
          for(S32 col = 0; col < (S32)bmp->width; col += 1)
          {
            S32 x = atlas_write_x + left + col;
            if(x < 0 || dim.x <= x) { continue; }
            U64 off = (y*dim.x + x)*4;
            atlas[off+0] = 255;
            atlas[off+1] = 255;
            atlas[off+2] = 255;
            atlas[off+3] = bmp->buffer[row*bmp->pitch + col];
          }
        }
      }
      atlas_write_x += (glyph_face->glyph->advance.x >> 6);
    }
    
    //- rjf: fill result
    result.atlas_dim = dim;
    result.advance   = (F32)total_width;
    result.atlas     = atlas;
    scratch_end(scratch);
  }
  ProfEnd();
  return result;
}
