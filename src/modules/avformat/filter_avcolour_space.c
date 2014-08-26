/*
 * filter_avcolour_space.c -- Colour space filter
 * Copyright (C) 2004-2014 Ushodaya Enterprises Limited
 * Author: Charles Yates <charles.yates@pandora.be>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <framework/mlt_filter.h>
#include <framework/mlt_frame.h>
#include <framework/mlt_log.h>
#include <framework/mlt_profile.h>
#include <framework/mlt_producer.h>

// ffmpeg Header files
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#include <stdio.h>
#include <stdlib.h>

#if 0 // This test might come in handy elsewhere someday.
static int is_big_endian( )
{
	union { int i; char c[ 4 ]; } big_endian_test;
	big_endian_test.i = 1;

	return big_endian_test.c[ 0 ] != 1;
}
#endif

static int convert_mlt_to_av_cs( mlt_image_format format )
{
	int value = 0;

	switch( format )
	{
		case mlt_image_rgb24:
			value = PIX_FMT_RGB24;
			break;
		case mlt_image_rgb24a:
		case mlt_image_opengl:
			value = PIX_FMT_RGBA;
			break;
		case mlt_image_yuv422:
			value = PIX_FMT_YUYV422;
			break;
		case mlt_image_yuv420p:
			value = PIX_FMT_YUV420P;
			break;
		default:
			mlt_log_error( NULL, "[filter avcolor_space] Invalid format %s\n",
				mlt_image_format_name( format ) );
			break;
	}

	return value;
}

static int set_luma_transfer( struct SwsContext *context, int src_colorspace, int dst_colorspace, int src_full_range, int dst_full_range )
{
	const int *src_coefficients = sws_getCoefficients( SWS_CS_DEFAULT );
	const int *dst_coefficients = sws_getCoefficients( SWS_CS_DEFAULT );
	int brightness = 0;
	int contrast = 1 << 16;
	int saturation = 1  << 16;
	int src_range = src_full_range ? 1 : 0;
	int dst_range = dst_full_range ? 1 : 0;

	switch ( src_colorspace )
	{
	case 170:
	case 470:
	case 601:
	case 624:
		src_coefficients = sws_getCoefficients( SWS_CS_ITU601 );
		break;
	case 240:
		src_coefficients = sws_getCoefficients( SWS_CS_SMPTE240M );
		break;
	case 709:
		src_coefficients = sws_getCoefficients( SWS_CS_ITU709 );
		break;
	default:
		break;
	}
	switch ( dst_colorspace )
	{
	case 170:
	case 470:
	case 601:
	case 624:
		src_coefficients = sws_getCoefficients( SWS_CS_ITU601 );
		break;
	case 240:
		src_coefficients = sws_getCoefficients( SWS_CS_SMPTE240M );
		break;
	case 709:
		src_coefficients = sws_getCoefficients( SWS_CS_ITU709 );
		break;
	default:
		break;
	}
	return sws_setColorspaceDetails( context, src_coefficients, src_range, dst_coefficients, dst_range,
		brightness, contrast, saturation );
}

// returns set_lumage_transfer result
static int av_convert_image( uint8_t *out, uint8_t *in, int out_fmt, int in_fmt,
	int width, int height, int src_colorspace, int dst_colorspace, int src_full_range, int dst_full_range )
{
	AVPicture input;
	AVPicture output;
	int flags = SWS_BICUBIC | SWS_ACCURATE_RND;
	int error = -1;

	if ( out_fmt == PIX_FMT_YUYV422 )
		flags |= SWS_FULL_CHR_H_INP;
	else
		flags |= SWS_FULL_CHR_H_INT;
#ifdef USE_MMX
	flags |= SWS_CPU_CAPS_MMX;
#endif
#ifdef USE_SSE
	flags |= SWS_CPU_CAPS_MMX2;
#endif

	avpicture_fill( &input, in, in_fmt, width, height );
	avpicture_fill( &output, out, out_fmt, width, height );
	struct SwsContext *context = sws_getContext( width, height, in_fmt,
		width, height, out_fmt, flags, NULL, NULL, NULL);
	if ( context )
	{
		// libswscale wants the RGB colorspace to be SWS_CS_DEFAULT, which is = SWS_CS_ITU601.
		if ( out_fmt == PIX_FMT_RGB24 || out_fmt == PIX_FMT_RGBA )
			dst_colorspace = 601;
		error = set_luma_transfer( context, src_colorspace, dst_colorspace, src_full_range, dst_full_range );
		sws_scale( context, (const uint8_t* const*) input.data, input.linesize, 0, height,
			output.data, output.linesize);
		sws_freeContext( context );
	}
	return error;
}

/** Do it :-).
*/

static int convert_image( mlt_frame frame, uint8_t **image, mlt_image_format *format, mlt_image_format output_format )
{
	mlt_properties properties = MLT_FRAME_PROPERTIES( frame );
	int width = mlt_properties_get_int( properties, "width" );
	int height = mlt_properties_get_int( properties, "height" );
	int error = 0;

	if ( *format != output_format )
	{
		mlt_profile profile = mlt_service_profile(
			MLT_PRODUCER_SERVICE( mlt_frame_get_original_producer( frame ) ) );
		int profile_colorspace = profile ? profile->colorspace : 601;
		int colorspace = mlt_properties_get_int( properties, "colorspace" );
		int src_full_range = ( *format == mlt_image_yuv420p || *format == mlt_image_yuv422 )
				&& mlt_properties_get_int( properties, "full_luma" );
		int dst_full_range = ( output_format == mlt_image_yuv420p || output_format == mlt_image_yuv422 )
				&& ( mlt_properties_get_int( properties, "consumer_color_range" ) == AVCOL_RANGE_JPEG
					 || ( mlt_properties_get( properties, "consumer_color_range" )
						  &&  ( !strcmp( "jpeg", mlt_properties_get( properties, "consumer_color_range" ) )
							 || !strcmp( "JPEG", mlt_properties_get( properties, "consumer_color_range" ) )
				   ) ) );

		if ( output_format == mlt_image_rgb24
				|| output_format == mlt_image_rgb24a
				|| output_format == mlt_image_opengl )
			src_full_range = 0;
		else if ( output_format == mlt_image_yuv420p || output_format == mlt_image_yuv422 )
			dst_full_range = dst_full_range || src_full_range;
		
		mlt_log_info( NULL, "[filter avcolor_space] %s -> %s @ %dx%d space %d->%d src_full_range %d dst_full_range %d\n",
			mlt_image_format_name( *format ), mlt_image_format_name( output_format ),
			width, height, colorspace, profile_colorspace, src_full_range, dst_full_range );

		int in_fmt = convert_mlt_to_av_cs( *format );
		int out_fmt = convert_mlt_to_av_cs( output_format );
		int size = FFMAX( avpicture_get_size( out_fmt, width, height ),
			mlt_image_format_size( output_format, width, height, NULL ) );
		uint8_t *output = mlt_pool_alloc( size );

		if ( *format == mlt_image_rgb24a || *format == mlt_image_opengl )
		{
			register int len = width * height;
			uint8_t *alpha = mlt_pool_alloc( len );

			if ( alpha )
			{
				// Extract the alpha mask from the RGBA image using Duff's Device
				register uint8_t *s = *image + 3; // start on the alpha component
				register uint8_t *d = alpha;
				register int n = ( len + 7 ) / 8;

				switch ( len % 8 )
				{
					case 0:	do { *d++ = *s; s += 4;
					case 7:		 *d++ = *s; s += 4;
					case 6:		 *d++ = *s; s += 4;
					case 5:		 *d++ = *s; s += 4;
					case 4:		 *d++ = *s; s += 4;
					case 3:		 *d++ = *s; s += 4;
					case 2:		 *d++ = *s; s += 4;
					case 1:		 *d++ = *s; s += 4;
							}
							while ( --n > 0 );
				}
				mlt_frame_set_alpha( frame, alpha, len, mlt_pool_release );
			}
		}

		// Update the output
		if ( !av_convert_image( output, *image, out_fmt, in_fmt, width, height,
				colorspace, profile_colorspace, src_full_range, dst_full_range ) )
		{
			// The new colorspace is only valid if destination is YUV.
			if ( output_format == mlt_image_yuv422 || output_format == mlt_image_yuv420p )
				mlt_properties_set_int( properties, "colorspace", profile_colorspace );
			mlt_properties_set_int( properties, "full_luma", dst_full_range );
		}
		*image = output;
		*format = output_format;
		mlt_frame_set_image( frame, output, size, mlt_pool_release );
		mlt_properties_set_int( properties, "format", output_format );

		if ( output_format == mlt_image_rgb24a || output_format == mlt_image_opengl )
		{
			register int len = width * height;
			int alpha_size = 0;
			uint8_t *alpha = mlt_frame_get_alpha_mask( frame );
			mlt_properties_get_data( properties, "alpha", &alpha_size );

			if ( alpha && alpha_size >= len )
			{
				// Merge the alpha mask from into the RGBA image using Duff's Device
				register uint8_t *s = alpha;
				register uint8_t *d = *image + 3; // start on the alpha component
				register int n = ( len + 7 ) / 8;

				switch ( len % 8 )
				{
					case 0:	do { *d = *s++; d += 4;
					case 7:		 *d = *s++; d += 4;
					case 6:		 *d = *s++; d += 4;
					case 5:		 *d = *s++; d += 4;
					case 4:		 *d = *s++; d += 4;
					case 3:		 *d = *s++; d += 4;
					case 2:		 *d = *s++; d += 4;
					case 1:		 *d = *s++; d += 4;
							}
							while ( --n > 0 );
				}
			}
		}
	}
	return error;
}

/* TODO: Enable this to force colorspace conversion. Cost is heavy due to RGB conversions. */
#if 0
static int get_image( mlt_frame frame, uint8_t **image, mlt_image_format *format, int *width, int *height, int writable )
{
	int error = 0;
	mlt_profile profile = (mlt_profile) mlt_frame_pop_get_image( frame );
	mlt_properties properties = MLT_FRAME_PROPERTIES(frame);
	mlt_image_format format_from = *format;
	mlt_image_format format_to = mlt_image_rgb24;
	
	error = mlt_frame_get_image( frame, image, format, width, height, writable );
	
	int frame_colorspace = mlt_properties_get_int( properties, "colorspace" );
	
	if ( !error && *format == mlt_image_yuv422 && profile->colorspace > 0 &&
	     frame_colorspace > 0 && frame_colorspace != profile->colorspace )
	{
		mlt_log_debug( NULL, "[filter avcolor_space] colorspace %d -> %d\n",
			frame_colorspace, profile->colorspace );
		
		// Convert to RGB using frame's colorspace
		error = convert_image( frame, image, &format_from, format_to );

		// Convert to YUV using profile's colorspace
		if ( !error )
		{
			*image = mlt_properties_get_data( properties, "image", NULL );
			format_from = mlt_image_rgb24;
			format_to = *format;
			mlt_properties_set_int( properties, "colorspace", profile->colorspace );
			error = convert_image( frame, image, &format_from, format_to );
			*image = mlt_properties_get_data( properties, "image", NULL );
		}
	}
	
	return error;
}
#endif

/** Filter processing.
*/

static mlt_frame filter_process( mlt_filter filter, mlt_frame frame )
{
	// Set a default colorspace on the frame if not yet set by the producer.
	// The producer may still change it during get_image.
	// This way we do not have to modify each producer to set a valid colorspace.
	mlt_properties properties = MLT_FRAME_PROPERTIES(frame);
	if ( mlt_properties_get_int( properties, "colorspace" ) <= 0 )
		mlt_properties_set_int( properties, "colorspace", mlt_service_profile( MLT_FILTER_SERVICE(filter) )->colorspace );

	if ( !frame->convert_image )
		frame->convert_image = convert_image;

//	Not working yet - see comment for get_image() above.
//	mlt_frame_push_service( frame, mlt_service_profile( MLT_FILTER_SERVICE( filter ) ) );
//	mlt_frame_push_get_image( frame, get_image );

	return frame;
}

/** Constructor for the filter.
*/

mlt_filter filter_avcolour_space_init( void *arg )
{
	// Test to see if swscale accepts the arg as resolution
	if ( arg )
	{
		int *width = (int*) arg;
		if ( *width > 0 )
		{
			struct SwsContext *context = sws_getContext( *width, *width, PIX_FMT_RGB32, 64, 64, PIX_FMT_RGB32, SWS_BILINEAR, NULL, NULL, NULL);
			if ( context )
				sws_freeContext( context );
			else
				return NULL;
		}
	}
	mlt_filter filter = mlt_filter_new( );
	if ( filter != NULL )
		filter->process = filter_process;
	return filter;
}

