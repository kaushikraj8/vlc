/*****************************************************************************
 * crop.c : Crop video plugin for vlc
 *****************************************************************************
 * Copyright (C) 2002, 2003 the VideoLAN team
 * $Id$
 *
 * Authors: Samuel Hocevar <sam@zoy.org>
 *          mod by Cedric Cocquebert <Cedric.Cocquebert@supelec.fr>
 *          based of DScaler idea (M. Samblanet)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <stdlib.h>                                      /* malloc(), free() */
#include <string.h>

#include <vlc/vlc.h>
#include <vlc_vout.h>
#include <vlc_interface.h>

#include "filter_common.h"

#define BEST_AUTOCROP 1
#ifdef BEST_AUTOCROP
    #define RATIO_MAX 15000  // 10*4/3 for a 360
#endif

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int  Create    ( vlc_object_t * );
static void Destroy   ( vlc_object_t * );

static int  Init      ( vout_thread_t * );
static void End       ( vout_thread_t * );
static int  Manage    ( vout_thread_t * );
static void Render    ( vout_thread_t *, picture_t * );

static void UpdateStats    ( vout_thread_t *, picture_t * );

static int  SendEvents( vlc_object_t *, char const *,
                        vlc_value_t, vlc_value_t, void * );

#ifdef BEST_AUTOCROP
/*****************************************************************************
 * Callback prototypes
 *****************************************************************************/
static int FilterCallback ( vlc_object_t *, char const *,
                            vlc_value_t, vlc_value_t, void * );
#endif

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
#define GEOMETRY_TEXT N_("Crop geometry (pixels)")
#define GEOMETRY_LONGTEXT N_("Set the geometry of the zone to crop. This is set as <width> x <height> + <left offset> + <top offset>.")

#define AUTOCROP_TEXT N_("Automatic cropping")
#define AUTOCROP_LONGTEXT N_("Automatically detect black borders and crop them.")

#ifdef BEST_AUTOCROP
#define RATIOMAX_TEXT N_("Ratio max (x 1000)")
#define RATIOMAX_LONGTEXT N_("Maximum image ratio. The crop plugin will never automatically crop to a higher ratio (ie, to a more \"flat\" image). The value is x1000: 1333 means 4/3.")

#define RATIO_TEXT N_("Manual ratio")
#define RATIO_LONGTEXT N_("Force a ratio (0 for automatic). Value is x1000: 1333 means 4/3.")

#define TIME_TEXT N_("Number of images for change")
#define TIME_LONGTEXT N_("The number of consecutive images with the same detected ratio (different from the previously detected ratio) to consider that ratio chnged and trigger recrop.")

#define DIFF_TEXT N_("Number of lines for change")
#define DIFF_LONGTEXT N_("The minimum difference in the number of detected black lines to consider that ratio changed and trigger recrop.")

#define NBP_TEXT N_("Number of non black pixels ")
#define NBP_LONGTEXT N_("The maximum of non-black pixels in a line to consider"\
                        " that the line is black.")

#define SKIP_TEXT N_("Skip percentage (%)")
#define SKIP_LONGTEXT N_("Percentage of the line to consider while checking for black lines. This allows to skip logos in black borders and crop them anyway.")

#define LUM_TEXT N_("Luminance threshold ")
#define LUM_LONGTEXT N_("Maximum luminance to consider a pixel as black (0-255).")
#endif

vlc_module_begin();
    set_description( _("Crop video filter") );
    set_shortname( _("Crop" ));
    set_category( CAT_VIDEO );
    set_subcategory( SUBCAT_VIDEO_VFILTER );
    set_capability( "video filter", 0 );

    add_string( "crop-geometry", NULL, NULL, GEOMETRY_TEXT,
                                             GEOMETRY_LONGTEXT, VLC_FALSE );
    add_bool( "autocrop", 0, NULL, AUTOCROP_TEXT,
                                   AUTOCROP_LONGTEXT, VLC_FALSE );

#ifdef BEST_AUTOCROP
    add_integer_with_range( "autocrop-ratio-max", 2405, 0, RATIO_MAX, NULL,
                            RATIOMAX_TEXT, RATIOMAX_LONGTEXT, VLC_TRUE );

    add_integer_with_range( "crop-ratio", 0, 0, RATIO_MAX, NULL, RATIO_TEXT,
                            RATIO_LONGTEXT, VLC_FALSE );
    add_integer( "autocrop-time", 25, NULL, TIME_TEXT,
                 TIME_LONGTEXT, VLC_TRUE );
    add_integer( "autocrop-diff", 16, NULL, DIFF_TEXT,
                                            DIFF_LONGTEXT, VLC_TRUE );

    add_integer( "autocrop-non-black-pixels", 3, NULL,
                 NBP_TEXT, NBP_LONGTEXT, VLC_TRUE );

    add_integer_with_range( "autocrop-skip-percent", 17, 0, 100, NULL,
                            SKIP_TEXT, SKIP_LONGTEXT, VLC_TRUE );

    add_integer_with_range( "autocrop-luminance-threshold", 40, 0, 128, NULL,
                            LUM_TEXT, LUM_LONGTEXT, VLC_TRUE );
#endif //BEST_AUTOCROP

    add_shortcut( "crop" );
    set_callbacks( Create, Destroy );
vlc_module_end();

/*****************************************************************************
 * vout_sys_t: Crop video output method descriptor
 *****************************************************************************
 * This structure is part of the video output thread descriptor.
 * It describes the Crop specific properties of an output thread.
 *****************************************************************************/
struct vout_sys_t
{
    vout_thread_t *p_vout;

    unsigned int i_x, i_y;
    unsigned int i_width, i_height, i_aspect;

    vlc_bool_t b_autocrop;

    /* Autocrop specific variables */
    unsigned int i_lastchange;
    vlc_bool_t   b_changed;
#ifdef BEST_AUTOCROP
    unsigned int i_ratio_max;
    unsigned int i_threshold, i_skipPercent, i_nonBlackPixel, i_diff, i_time;
    unsigned int i_ratio;
#endif

};

/*****************************************************************************
 * Control: control facility for the vout (forwards to child vout)
 *****************************************************************************/
static int Control( vout_thread_t *p_vout, int i_query, va_list args )
{
    return vout_vaControl( p_vout->p_sys->p_vout, i_query, args );
}

/*****************************************************************************
 * Create: allocates Crop video thread output method
 *****************************************************************************
 * This function allocates and initializes a Crop vout method.
 *****************************************************************************/
static int Create( vlc_object_t *p_this )
{
    vout_thread_t *p_vout = (vout_thread_t *)p_this;

    /* Allocate structure */
    p_vout->p_sys = malloc( sizeof( vout_sys_t ) );
    if( p_vout->p_sys == NULL )
    {
        msg_Err( p_vout, "out of memory" );
        return VLC_ENOMEM;
    }

    p_vout->pf_init = Init;
    p_vout->pf_end = End;
    p_vout->pf_manage = Manage;
    p_vout->pf_render = Render;
    p_vout->pf_display = NULL;
    p_vout->pf_control = Control;

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Init: initialize Crop video thread output method
 *****************************************************************************/
static int Init( vout_thread_t *p_vout )
{
    int   i_index;
    char *psz_var;
    picture_t *p_pic;
    video_format_t fmt;

    I_OUTPUTPICTURES = 0;
    memset( &fmt, 0, sizeof(video_format_t) );

    p_vout->p_sys->i_lastchange = 0;
    p_vout->p_sys->b_changed = VLC_FALSE;

    /* Initialize the output structure */
    p_vout->output.i_chroma = p_vout->render.i_chroma;
    p_vout->output.i_width  = p_vout->render.i_width;
    p_vout->output.i_height = p_vout->render.i_height;
    p_vout->output.i_aspect = p_vout->render.i_aspect;
    p_vout->fmt_out = p_vout->fmt_in;

    /* Shall we use autocrop ? */
    p_vout->p_sys->b_autocrop = config_GetInt( p_vout, "autocrop" );
#ifdef BEST_AUTOCROP
    p_vout->p_sys->i_ratio_max = config_GetInt( p_vout, "autocrop-ratio-max" );
    p_vout->p_sys->i_threshold =
                    config_GetInt( p_vout, "autocrop-luminance-threshold" );
    p_vout->p_sys->i_skipPercent =
                    config_GetInt( p_vout, "autocrop-skip-percent" );
    p_vout->p_sys->i_nonBlackPixel =
                    config_GetInt( p_vout, "autocrop-non-black-pixels" );
    p_vout->p_sys->i_diff = config_GetInt( p_vout, "autocrop-diff" );
    p_vout->p_sys->i_time = config_GetInt( p_vout, "autocrop-time" );
    vlc_value_t val={0};
    var_Get( p_vout, "ratio-crop", &val );
    val.psz_string = "0";
    var_SetString( p_vout, "ratio-crop", val.psz_string);

    if (p_vout->p_sys->b_autocrop)
        p_vout->p_sys->i_ratio = 0;
    else
    {
        p_vout->p_sys->i_ratio = config_GetInt( p_vout, "crop-ratio" );
        // ratio < width / height => ratio = 0 (unchange ratio)
        if (p_vout->p_sys->i_ratio < (p_vout->output.i_width * 1000) / p_vout->output.i_height)
            p_vout->p_sys->i_ratio = 0;
    }
#endif


    /* Get geometry value from the user */
    psz_var = config_GetPsz( p_vout, "crop-geometry" );
    if( psz_var )
    {
        char *psz_parser, *psz_tmp;

        psz_parser = psz_tmp = psz_var;
        while( *psz_tmp && *psz_tmp != 'x' ) psz_tmp++;

        if( *psz_tmp )
        {
            psz_tmp[0] = '\0';
            p_vout->p_sys->i_width = atoi( psz_parser );

            psz_parser = ++psz_tmp;
            while( *psz_tmp && *psz_tmp != '+' ) psz_tmp++;

            if( *psz_tmp )
            {
                psz_tmp[0] = '\0';
                p_vout->p_sys->i_height = atoi( psz_parser );

                psz_parser = ++psz_tmp;
                while( *psz_tmp && *psz_tmp != '+' ) psz_tmp++;

                if( *psz_tmp )
                {
                    psz_tmp[0] = '\0';
                    p_vout->p_sys->i_x = atoi( psz_parser );
                    p_vout->p_sys->i_y = atoi( ++psz_tmp );
                }
                else
                {
                    p_vout->p_sys->i_x = atoi( psz_parser );
                    p_vout->p_sys->i_y =
                     ( p_vout->output.i_height - p_vout->p_sys->i_height ) / 2;
                }
            }
            else
            {
                p_vout->p_sys->i_height = atoi( psz_parser );
                p_vout->p_sys->i_x =
                     ( p_vout->output.i_width - p_vout->p_sys->i_width ) / 2;
                p_vout->p_sys->i_y =
                     ( p_vout->output.i_height - p_vout->p_sys->i_height ) / 2;
            }
        }
        else
        {
            p_vout->p_sys->i_width = atoi( psz_parser );
            p_vout->p_sys->i_height = p_vout->output.i_height;
            p_vout->p_sys->i_x =
                     ( p_vout->output.i_width - p_vout->p_sys->i_width ) / 2;
            p_vout->p_sys->i_y =
                     ( p_vout->output.i_height - p_vout->p_sys->i_height ) / 2;
        }

        /* Check for validity */
        if( p_vout->p_sys->i_x + p_vout->p_sys->i_width
                                                   > p_vout->output.i_width )
        {
            p_vout->p_sys->i_x = 0;
            if( p_vout->p_sys->i_width > p_vout->output.i_width )
            {
                p_vout->p_sys->i_width = p_vout->output.i_width;
            }
        }

        if( p_vout->p_sys->i_y + p_vout->p_sys->i_height
                                                   > p_vout->output.i_height )
        {
            p_vout->p_sys->i_y = 0;
            if( p_vout->p_sys->i_height > p_vout->output.i_height )
            {
                p_vout->p_sys->i_height = p_vout->output.i_height;
            }
        }

        free( psz_var );
    }
    else
#ifdef BEST_AUTOCROP
    if (p_vout->p_sys->i_ratio)
    {
        p_vout->p_sys->i_aspect    =  p_vout->p_sys->i_ratio * 432;
        p_vout->p_sys->i_width  = p_vout->fmt_out.i_visible_width;
        p_vout->p_sys->i_height = p_vout->output.i_aspect
                                * p_vout->output.i_height / p_vout->p_sys->i_aspect
                                * p_vout->p_sys->i_width / p_vout->output.i_width;
        p_vout->p_sys->i_height += p_vout->p_sys->i_height % 2;
        p_vout->p_sys->i_x = p_vout->fmt_out.i_x_offset;
        p_vout->p_sys->i_y = (p_vout->output.i_height - p_vout->p_sys->i_height) / 2;
    }
    else
#endif
    {
        p_vout->p_sys->i_width  = p_vout->fmt_out.i_visible_width;
        p_vout->p_sys->i_height = p_vout->fmt_out.i_visible_height;
        p_vout->p_sys->i_x = p_vout->fmt_out.i_x_offset;
        p_vout->p_sys->i_y = p_vout->fmt_out.i_y_offset;
    }

    /* Pheeew. Parsing done. */
    msg_Dbg( p_vout, "cropping at %ix%i+%i+%i, %sautocropping",
                     p_vout->p_sys->i_width, p_vout->p_sys->i_height,
                     p_vout->p_sys->i_x, p_vout->p_sys->i_y,
                     p_vout->p_sys->b_autocrop ? "" : "not " );
    /* Set current output image properties */
    p_vout->p_sys->i_aspect = p_vout->fmt_out.i_aspect
           * p_vout->fmt_out.i_visible_height / p_vout->p_sys->i_height
           * p_vout->p_sys->i_width / p_vout->fmt_out.i_visible_width;

#ifdef BEST_AUTOCROP
    msg_Info( p_vout, "ratio %d",  p_vout->p_sys->i_aspect / 432);
#endif
    fmt.i_width = fmt.i_visible_width = p_vout->p_sys->i_width;
    fmt.i_height = fmt.i_visible_height = p_vout->p_sys->i_height;
    fmt.i_x_offset = fmt.i_y_offset = 0;
    fmt.i_chroma = p_vout->render.i_chroma;
    fmt.i_aspect = p_vout->p_sys->i_aspect;
    fmt.i_sar_num = p_vout->p_sys->i_aspect * fmt.i_height / fmt.i_width;
    fmt.i_sar_den = VOUT_ASPECT_FACTOR;

    /* Try to open the real video output */
    p_vout->p_sys->p_vout = vout_Create( p_vout, &fmt );
    if( p_vout->p_sys->p_vout == NULL )
    {
        msg_Err( p_vout, "failed to create vout" );
        intf_UserFatal( p_vout, VLC_FALSE, _("Cropping failed"),
                        _("VLC could not open the video output module.") );
        return VLC_EGENERIC;
    }

    ALLOCATE_DIRECTBUFFERS( VOUT_MAX_PICTURES );

    ADD_CALLBACKS( p_vout->p_sys->p_vout, SendEvents );

#ifdef BEST_AUTOCROP
    var_AddCallback( p_vout, "ratio-crop", FilterCallback, NULL );
#endif

    ADD_PARENT_CALLBACKS( SendEventsToChild );

    return VLC_SUCCESS;
}

/*****************************************************************************
 * End: terminate Crop video thread output method
 *****************************************************************************/
static void End( vout_thread_t *p_vout )
{
    int i_index;

    /* Free the fake output buffers we allocated */
    for( i_index = I_OUTPUTPICTURES ; i_index ; )
    {
        i_index--;
        free( PP_OUTPUTPICTURE[ i_index ]->p_data_orig );
    }
}

/*****************************************************************************
 * Destroy: destroy Crop video thread output method
 *****************************************************************************
 * Terminate an output method created by CropCreateOutputMethod
 *****************************************************************************/
static void Destroy( vlc_object_t *p_this )
{
    vout_thread_t *p_vout = (vout_thread_t *)p_this;

    if( p_vout->p_sys->p_vout )
    {
        DEL_CALLBACKS( p_vout->p_sys->p_vout, SendEvents );
        vlc_object_detach( p_vout->p_sys->p_vout );
        vout_Destroy( p_vout->p_sys->p_vout );
    }

    DEL_PARENT_CALLBACKS( SendEventsToChild );

    free( p_vout->p_sys );
}

/*****************************************************************************
 * Manage: handle Crop events
 *****************************************************************************
 * This function should be called regularly by video output thread. It manages
 * console events. It returns a non null value on error.
 *****************************************************************************/
static int Manage( vout_thread_t *p_vout )
{
    video_format_t fmt;

    if( !p_vout->p_sys->b_changed )
    {
        return VLC_SUCCESS;
    }

    memset( &fmt, 0, sizeof(video_format_t) );

#ifdef BEST_AUTOCROP
    msg_Dbg( p_vout, "cropping at %ix%i+%i+%i, %sautocropping",
                     p_vout->p_sys->i_width, p_vout->p_sys->i_height,
                     p_vout->p_sys->i_x, p_vout->p_sys->i_y,
                     p_vout->p_sys->b_autocrop ? "" : "not " );

    msg_Info( p_vout, "ratio %d",  p_vout->p_sys->i_aspect / 432);
#endif

    vout_Destroy( p_vout->p_sys->p_vout );

    fmt.i_width = fmt.i_visible_width = p_vout->p_sys->i_width;
    fmt.i_height = fmt.i_visible_height = p_vout->p_sys->i_height;
    fmt.i_x_offset = fmt.i_y_offset = 0;
    fmt.i_chroma = p_vout->render.i_chroma;
    fmt.i_aspect = p_vout->p_sys->i_aspect;
    fmt.i_sar_num = p_vout->p_sys->i_aspect * fmt.i_height / fmt.i_width;
    fmt.i_sar_den = VOUT_ASPECT_FACTOR;

    p_vout->p_sys->p_vout = vout_Create( p_vout, &fmt );
    if( p_vout->p_sys->p_vout == NULL )
    {
        msg_Err( p_vout, "failed to create vout" );
        intf_UserFatal( p_vout, VLC_FALSE, _("Cropping failed"),
                        _("VLC could not open the video output module.") );
        return VLC_EGENERIC;
    }

    p_vout->p_sys->b_changed = VLC_FALSE;
    p_vout->p_sys->i_lastchange = 0;

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Render: display previously rendered output
 *****************************************************************************
 * This function sends the currently rendered image to Crop image, waits
 * until it is displayed and switches the two rendering buffers, preparing next
 * frame.
 *****************************************************************************/
static void Render( vout_thread_t *p_vout, picture_t *p_pic )
{
    picture_t *p_outpic = NULL;
    int i_plane;

    if( p_vout->p_sys->b_changed )
    {
        return;
    }

    while( ( p_outpic =
                 vout_CreatePicture( p_vout->p_sys->p_vout, 0, 0, 0 )
           ) == NULL )
    {
        if( p_vout->b_die || p_vout->b_error )
        {
            vout_DestroyPicture( p_vout->p_sys->p_vout, p_outpic );
            return;
        }

        msleep( VOUT_OUTMEM_SLEEP );
    }

    vout_DatePicture( p_vout->p_sys->p_vout, p_outpic, p_pic->date );
    vout_LinkPicture( p_vout->p_sys->p_vout, p_outpic );

    for( i_plane = 0 ; i_plane < p_pic->i_planes ; i_plane++ )
    {
        uint8_t *p_in, *p_out, *p_out_end;
        int i_in_pitch = p_pic->p[i_plane].i_pitch;
        const int i_out_pitch = p_outpic->p[i_plane].i_pitch;
        const int i_copy_pitch = p_outpic->p[i_plane].i_visible_pitch;

        p_in = p_pic->p[i_plane].p_pixels
                /* Skip the right amount of lines */
                + i_in_pitch * ( p_pic->p[i_plane].i_visible_lines *
                                 p_vout->p_sys->i_y / p_vout->output.i_height )
                /* Skip the right amount of columns */
                + i_in_pitch * p_vout->p_sys->i_x / p_vout->output.i_width;

        p_out = p_outpic->p[i_plane].p_pixels;
        p_out_end = p_out + i_out_pitch * p_outpic->p[i_plane].i_visible_lines;

        while( p_out < p_out_end )
        {
            p_vout->p_libvlc->pf_memcpy( p_out, p_in, i_copy_pitch );
            p_in += i_in_pitch;
            p_out += i_out_pitch;
        }
    }

    vout_UnlinkPicture( p_vout->p_sys->p_vout, p_outpic );
    vout_DisplayPicture( p_vout->p_sys->p_vout, p_outpic );

    /* The source image may still be in the cache ... parse it! */
    if( p_vout->p_sys->b_autocrop )
    {
        UpdateStats( p_vout, p_pic );
    }
}

#ifdef BEST_AUTOCROP
static vlc_bool_t NonBlackLine(uint8_t *p_in, int i_line, int i_pitch,
                               int i_visible_pitch, int i_lines,
                               int i_lumThreshold, int i_skipCountPercent,
                               int i_nonBlackPixel, int i_chroma)
{
    const int i_col = i_line * i_pitch / i_lines;
    int i_index, i_count = 0;
    int i_skipCount = 0;

    switch(i_chroma)
    {
    // planar YUV
        case VLC_FOURCC('I','4','4','4'):
        case VLC_FOURCC('I','4','2','2'):
        case VLC_FOURCC('I','4','2','0'):
        case VLC_FOURCC('Y','V','1','2'):
        case VLC_FOURCC('I','Y','U','V'):
        case VLC_FOURCC('I','4','1','1'):
        case VLC_FOURCC('I','4','1','0'):
        case VLC_FOURCC('Y','V','U','9'):
        case VLC_FOURCC('Y','U','V','A'):
            i_skipCount = (i_pitch * i_skipCountPercent) / 100;
            for (i_index = i_col/2 + i_skipCount/2;
                 i_index <= i_visible_pitch/2 + i_col/2 - i_skipCount/2;
                 i_index++)
            {
                 i_count += (p_in[i_index] > i_lumThreshold);
            if (i_count > i_nonBlackPixel) break;
            }
            break;
    // packed RGB
        case VLC_FOURCC('R','G','B','2'):    // packed by 1
            i_skipCount = (i_pitch * i_skipCountPercent) / 100;
            for (i_index = i_col/2 + i_skipCount/2;
                 i_index <= i_visible_pitch/2 + i_col/2 - i_skipCount/2;
                 i_index++)
            {
                 i_count += (p_in[i_index] > i_lumThreshold);
            if (i_count > i_nonBlackPixel) break;
            }
            break;
        case VLC_FOURCC('R','V','1','5'):    // packed by 2
        case VLC_FOURCC('R','V','1','6'):    // packed by 2
            i_skipCount = (i_pitch * i_skipCountPercent) / 100;
            for (i_index = i_col/2 + i_skipCount/2 -
                                (i_col/2 + i_skipCount/2) % 2;
                 i_index <= i_visible_pitch/2 + i_col/2 - i_skipCount/2;
                 i_index+=2)
            {
                 i_count += (p_in[i_index] > i_lumThreshold) &&
                            (p_in[i_index + 1] > i_lumThreshold);
            if (i_count > i_nonBlackPixel) break;
            }
            break;
        case VLC_FOURCC('R','V','2','4'):    // packed by 3
            i_skipCount = (i_pitch * i_skipCountPercent) / 100;
            for (i_index = i_col/2 + i_skipCount/2 - (i_col/2 + i_skipCount/2) % 3; i_index <= i_visible_pitch/2 + i_col/2 - i_skipCount/2; i_index+=3)
            {
                 i_count += (p_in[i_index] > i_lumThreshold) &&
                            (p_in[i_index + 1] > i_lumThreshold) &&
                            (p_in[i_index + 2] > i_lumThreshold);
            if (i_count > i_nonBlackPixel) break;
            }
            break;
        case VLC_FOURCC('R','V','3','2'):    // packed by 4
            i_skipCount = (i_pitch * i_skipCountPercent) / 100;
            for (i_index = i_col/2 + i_skipCount/2 - (i_col/2 + i_skipCount/2) % 4; i_index <= i_visible_pitch/2 + i_col/2 - i_skipCount/2; i_index+=4)
            {
                 i_count += (uint32_t)(*(p_in + i_index)) > (uint32_t)i_lumThreshold;
            if (i_count > i_nonBlackPixel) break;
            }
            break;
    // packed YUV
        case VLC_FOURCC('Y','U','Y','2'):    // packed by 2
        case VLC_FOURCC('Y','U','N','V'):    // packed by 2
        case VLC_FOURCC('U','Y','V','Y'):    // packed by 2
        case VLC_FOURCC('U','Y','N','V'):    // packed by 2
        case VLC_FOURCC('Y','4','2','2'):    // packed by 2
            i_skipCount = (i_pitch * i_skipCountPercent) / 100;
            for (i_index = (i_col/2 + i_skipCount/2) -
                           (i_col/2 + i_skipCount/2) % 2;
                 i_index <= i_visible_pitch/2 + i_col/2 - i_skipCount/2;
                 i_index+=2)
            {
                 i_count += (p_in[i_index] > i_lumThreshold);
            if (i_count > i_nonBlackPixel) break;
            }
            break;
        default :
            break;
    }
    return (i_count > i_nonBlackPixel);
}
#endif

static void UpdateStats( vout_thread_t *p_vout, picture_t *p_pic )
{
   uint8_t *p_in = p_pic->p[0].p_pixels;
    int i_pitch = p_pic->p[0].i_pitch;
    int i_visible_pitch = p_pic->p[0].i_visible_pitch;
    int i_lines = p_pic->p[0].i_visible_lines;
    int i_firstwhite = -1, i_lastwhite = -1, i;
#ifdef BEST_AUTOCROP
    int i_time = p_vout->p_sys->i_time;
    int i_diff = p_vout->p_sys->i_diff;

    if (!p_vout->p_sys->i_ratio)
    {
        /* Determine where black borders are */
        for( i = 0 ; i < i_lines ; i++)
        {
                   if (NonBlackLine(p_in, i, i_pitch, i_visible_pitch, i_lines,
                            p_vout->p_sys->i_threshold,
                            p_vout->p_sys->i_skipPercent,
                            p_vout->p_sys->i_nonBlackPixel,
                            p_vout->output.i_chroma))
                {
                    i_firstwhite = i;
                    i_lastwhite = i_lines - i;
                    break;
                }
                p_in += i_pitch;
        }

        /* Decide whether it's worth changing the size */
        if( i_lastwhite == -1 )
        {
            p_vout->p_sys->i_lastchange = 0;
            return;
        }

        if( (i_lastwhite - i_firstwhite) < (int) (p_vout->p_sys->i_height / 2) )
        {
            p_vout->p_sys->i_lastchange = 0;
            return;
        }

        if (p_vout->output.i_aspect
                            * p_vout->output.i_height /
                                (i_lastwhite - i_firstwhite + 1)
                            * p_vout->p_sys->i_width /
                               p_vout->output.i_width >
                                    p_vout->p_sys->i_ratio_max * 432)
        {
            int i_height = ((p_vout->output.i_aspect / 432) *
                           p_vout->output.i_height * p_vout->p_sys->i_width) /
                          (p_vout->output.i_width * p_vout->p_sys->i_ratio_max);
            i_firstwhite = (p_vout->output.i_height - i_height) / 2;
            i_lastwhite =  p_vout->output.i_height - i_firstwhite;
/*
            p_vout->p_sys->i_lastchange = 0;
            return;
*/
        }

        if( (i_lastwhite - i_firstwhite) <
                        (int) (p_vout->p_sys->i_height + i_diff)
             && (i_lastwhite - i_firstwhite + i_diff) >
                        (int) p_vout->p_sys->i_height )
        {
            p_vout->p_sys->i_lastchange = 0;
            return;
        }

        /* We need at least 'i_time' images to make up our mind */
        p_vout->p_sys->i_lastchange++;
        if( p_vout->p_sys->i_lastchange < (unsigned int)i_time )
        {
            return;
        }
    }
    else
    {
        if ( p_vout->p_sys->i_lastchange >= (unsigned int)i_time )
        {
            p_vout->p_sys->i_aspect    =  p_vout->p_sys->i_ratio * 432;
            int i_height = p_vout->output.i_aspect
                                    * p_vout->output.i_height /
                                        p_vout->p_sys->i_aspect
                                    * p_vout->p_sys->i_width /
                                        p_vout->output.i_width;
            i_firstwhite = (p_vout->output.i_height - i_height) / 2;
            i_lastwhite =  p_vout->output.i_height - i_firstwhite;
        }
        else
        {
            return;
        }
    }

#else
    /* Determine where black borders are */
    switch( p_vout->output.i_chroma )
    {
    case VLC_FOURCC('I','4','2','0'):
        /* XXX: Do not laugh ! I know this is very naive. But it's just a
         *      proof of concept code snippet... */
        for( i = i_lines ; i-- ; )
        {
            const int i_col = i * i_pitch / i_lines;

            if( p_in[i_col/2] > 40
                 && p_in[i_visible_pitch/2] > 40
                 && p_in[i_visible_pitch/2 + i_col/2] > 40 )
            {
                if( i_lastwhite == -1 )
                {
                    i_lastwhite = i;
                }
                i_firstwhite = i;
            }
            p_in += i_pitch;
        }
        break;

    default:
        break;
    }

    /* Decide whether it's worth changing the size */
    if( i_lastwhite == -1 )
    {
        p_vout->p_sys->i_lastchange = 0;
        return;
    }

    if( (unsigned int)(i_lastwhite - i_firstwhite)
                                           < p_vout->p_sys->i_height / 2 )
    {
        p_vout->p_sys->i_lastchange = 0;
        return;
    }

    if( (unsigned int)(i_lastwhite - i_firstwhite)
                                          < p_vout->p_sys->i_height + 16
         && (unsigned int)(i_lastwhite - i_firstwhite + 16)
                                                > p_vout->p_sys->i_height )
    {
        p_vout->p_sys->i_lastchange = 0;
        return;
    }

    /* We need at least 25 images to make up our mind */
    p_vout->p_sys->i_lastchange++;
    if( p_vout->p_sys->i_lastchange < 25 )
    {
        return;
    }
#endif //BEST_AUTOCROP

    /* Tune a few values */
    if( i_firstwhite & 1 )
    {
        i_firstwhite--;
    }

    if( !(i_lastwhite & 1) )
    {
        i_lastwhite++;
    }

    /* Change size */
    p_vout->p_sys->i_y = i_firstwhite;
    p_vout->p_sys->i_height = i_lastwhite - i_firstwhite + 1;
#ifdef BEST_AUTOCROP
    // check p_vout->p_sys->i_height <= p_vout->output.i_height
    if (p_vout->p_sys->i_height > p_vout->output.i_height)
        p_vout->p_sys->i_height = p_vout->output.i_height;
#endif

    p_vout->p_sys->i_aspect = p_vout->output.i_aspect
                            * p_vout->output.i_height / p_vout->p_sys->i_height
                            * p_vout->p_sys->i_width / p_vout->output.i_width;

    p_vout->p_sys->b_changed = VLC_TRUE;
}

/*****************************************************************************
 * SendEvents: forward mouse and keyboard events to the parent p_vout
 *****************************************************************************/
static int SendEvents( vlc_object_t *p_this, char const *psz_var,
                       vlc_value_t oldval, vlc_value_t newval, void *_p_vout )
{
    vout_thread_t *p_vout = (vout_thread_t *)_p_vout;
    vlc_value_t sentval = newval;

    /* Translate the mouse coordinates */
    if( !strcmp( psz_var, "mouse-x" ) )
    {
        sentval.i_int += p_vout->p_sys->i_x;
    }
    else if( !strcmp( psz_var, "mouse-y" ) )
    {
        sentval.i_int += p_vout->p_sys->i_y;
    }

    var_Set( p_vout, psz_var, sentval );

    return VLC_SUCCESS;
}

/*****************************************************************************
 * SendEventsToChild: forward events to the child/children vout
 *****************************************************************************/
static int SendEventsToChild( vlc_object_t *p_this, char const *psz_var,
                       vlc_value_t oldval, vlc_value_t newval, void *p_data )
{
    vout_thread_t *p_vout = (vout_thread_t *)p_this;
    var_Set( p_vout->p_sys->p_vout, psz_var, newval );
    return VLC_SUCCESS;
}

#ifdef BEST_AUTOCROP
/*****************************************************************************
 * FilterCallback: called when changing the ratio on the fly.
 *****************************************************************************/
static int FilterCallback( vlc_object_t *p_this, char const *psz_var,
                           vlc_value_t oldval, vlc_value_t newval,
                           void *p_data )
{
    vout_thread_t * p_vout = (vout_thread_t *)p_this;

    if( !strcmp( psz_var, "ratio-crop" ) )
    {
        if ( !strcmp( newval.psz_string, "Auto" ) )
            p_vout->p_sys->i_ratio = 0;
        else
        {
            p_vout->p_sys->i_ratio = (unsigned int)atoi(newval.psz_string);
            p_vout->p_sys->i_lastchange = p_vout->p_sys->i_time;
            p_vout->p_sys->b_autocrop = VLC_TRUE;
        }
        if (p_vout->p_sys->i_ratio)
        {
            if (p_vout->p_sys->i_ratio < (p_vout->output.i_width * 1000) /
                                    p_vout->output.i_height)
                p_vout->p_sys->i_ratio = (p_vout->output.i_width * 1000) /
                                    p_vout->output.i_height;
            if (p_vout->p_sys->i_ratio < p_vout->output.i_aspect / 432)
                p_vout->p_sys->i_ratio = p_vout->output.i_aspect / 432;
        }
     }
    return VLC_SUCCESS;
}
#endif
