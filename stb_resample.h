/* stb_resample - v0.1 - public domain image resampling
no warranty implied; use at your own risk

Do this:
#define STB_RESAMPLE_IMPLEMENTATION
before you include this file in *one* C or C++ file to create the implementation.

#define STBR_ASSERT(x) to avoid using assert.h.

Latest revisions:

See end of file for full revision history.

Initial implementation by Jorge L Rodriguez
*/

#ifndef STBR_INCLUDE_STB_RESAMPLE_H
#define STBR_INCLUDE_STB_RESAMPLE_H

// Basic usage:
//    result = stbr_resize(input_data, input_w, input_h, 0, output_data, output_w, output_h, 0, channels, alpha_channel, STBR_TYPE_UINT8, STBR_FILTER_BILINEAR, STBR_EDGE_CLAMP, STBR_COLORSPACE_SRGB);
//
//    input_data is your supplied texels.
//    output_data will be the resized texels. It should be of size output_w * output_h * input_components (or output_h * output_stride if you provided a stride.)
//    If input_stride or output_stride is 0 (as in this example) the stride will be automatically calculated as width*components.
//    Returned result is 1 for success or 0 in case of an error.

typedef enum
{
	STBR_FILTER_NEAREST = 1,
} stbr_filter;

typedef enum
{
	STBR_EDGE_CLAMP = 1,
} stbr_edge;

typedef enum
{
	STBR_COLORSPACE_LINEAR = 1,
	STBR_COLORSPACE_SRGB = 2,
} stbr_colorspace;

typedef enum
{
	STBR_TYPE_UINT8 = 1,
} stbr_type;

typedef unsigned char stbr_uc;
typedef unsigned int stbr_size_t; // to avoid including a header for size_t

#ifdef __cplusplus
extern "C" {
#endif

#ifdef STB_RESAMPLE_STATIC
#define STBRDEF static
#else
#define STBRDEF extern
#endif

	//////////////////////////////////////////////////////////////////////////////
	//
	// PRIMARY API - resize an image
	//

	STBRDEF stbr_size_t stbr_calculate_memory(int input_w, int input_h, int input_stride_in_bytes,
		int output_w, int output_h, int output_stride_in_bytes,
		int channels, stbr_filter filter);

	STBRDEF int stbr_resize_arbitrary(const void* input_data, int input_w, int input_h, int input_stride_in_bytes,
		void* output_data, int output_w, int output_h, int output_stride_in_bytes,
		//int channels, int alpha_channel, stbr_type type, stbr_filter filter, stbr_edge edge, stbr_colorspace colorspace,
		int channels, stbr_type type, stbr_filter filter,
		void* tempmem, stbr_size_t tempmem_size_in_bytes);


#ifdef __cplusplus
}
#endif

//
//
////   end header file   /////////////////////////////////////////////////////
#endif // STBR_INCLUDE_STB_RESAMPLE_H

#ifdef STB_RESIZE_IMPLEMENTATION

#ifndef STBR_ASSERT
#include <assert.h>
#define STBR_ASSERT(x) assert(x)
#endif

#ifdef STBR_DEBUG
#define STBR_DEBUG_ASSERT STBR_ASSERT
#else
#define STBR_DEBUG_ASSERT
#endif

// If you hit this it means I haven't done it yet.
#define STBR_UNIMPLEMENTED(x) STBR_ASSERT(!(x))

#ifdef STBR_DEBUG_OVERWRITE_TEST
#include <string.h>
#endif


#include <math.h>


#ifndef _MSC_VER
#ifdef __cplusplus
#define stbr_inline inline
#else
#define stbr_inline
#endif
#else
#define stbr_inline __forceinline
#endif


#ifdef _MSC_VER
typedef unsigned short stbr__uint16;
typedef   signed short stbr__int16;
typedef unsigned int   stbr__uint32;
typedef   signed int   stbr__int32;
#else
#include <stdint.h>
typedef uint16_t stbr__uint16;
typedef int16_t  stbr__int16;
typedef uint32_t stbr__uint32;
typedef int32_t  stbr__int32;
#endif

// should produce compiler error if size is wrong
typedef unsigned char stbr__validate_uint32[sizeof(stbr__uint32) == 4 ? 1 : -1];

#ifdef _MSC_VER
#define STBR_NOTUSED(v)  (void)(v)
#else
#define STBR_NOTUSED(v)  (void)sizeof(v)
#endif

#define STBR_ARRAY_SIZE(a) (sizeof((a))/sizeof((a)[0]))

// Kernel function centered at 0
typedef float (stbr__kernel_fn)(float x);

typedef struct
{
	stbr__kernel_fn* kernel;
	float support;
} stbr__filter_info;

typedef struct
{
	int n0; // First contributing source texel
	int n1; // Last contributing source texel
} stbr__contributors;

typedef struct
{
	int total_coefficients;
	int kernel_texel_width;

	stbr__contributors* horizontal_contributors;
	float* horizontal_coefficients;

	float* decode_buffer;
} stbr__info;


float stbr__filter_nearest(float x)
{
	if (fabs(x) < 0.5)
		return 1;
	else
		return 0;
}

stbr__filter_info stbr__filter_info_table[] = {
		{ NULL,                 0.0f },
		{ stbr__filter_nearest, 0.5f },
};

// This is the maximum number of input samples that can affect an output sample
// with the given filter
int stbr__get_filter_texel_width(stbr_filter filter, int upsample)
{
	STBR_UNIMPLEMENTED(!upsample);

	STBR_ASSERT(filter != 0);
	STBR_ASSERT(filter < STBR_ARRAY_SIZE(stbr__filter_info_table));

	return (int)ceil(stbr__filter_info_table[filter].support * 2);
}

int stbr__get_total_coefficients(stbr_filter filter, int input_w, int output_w)
{
	return output_w * stbr__get_filter_texel_width(filter, output_w > input_w ? 1 : 0);
}

// i0 is a texel in [0, n0-1]
// What's the nearest texel center to i0's center in [0, n1-1] ?
// Remapping [0, n0-1] to [0, n1-1] gives (i0 + 0.5)*n1/n0 but we want to avoid
// floating point math so we rearrange it as (n1*i0 + n1/2)/n0
stbr_inline static int stbr__nearest_texel(int i0, int n0, int n1)
{
	return (n1*i0 + n1/2) / n0;
}

stbr_inline static stbr_size_t stbr__texel_index(int x, int y, int c, int width_stride, int num_c, int w, int h)
{
	STBR_DEBUG_ASSERT(x >= 0 && x < w);
	STBR_DEBUG_ASSERT(y >= 0 && y < h);

	return y*width_stride + x*num_c + c;
}

stbr_inline static stbr__contributors* stbr__get_contributor(stbr__info* stbr_info, int n)
{
	STBR_DEBUG_ASSERT(n >= 0 /*&& n < output_w*/);
	return &stbr_info->horizontal_contributors[n];
}

stbr_inline static float* stbr__get_coefficient(stbr__info* stbr_info, int n, int c)
{
	return &stbr_info->horizontal_coefficients[stbr_info->kernel_texel_width*n + c];
}

// Each scan line uses the same kernel values so we should calculate the kernel
// values once and then we can use them for every scan line.
static void stbr__calculate_horizontal_filters(stbr__info* stbr_info, stbr_filter filter, int input_w, int output_w)
{
	int n, i;
	float scale_ratio = (float)output_w / input_w;

	float out_pixels_radius = stbr__filter_info_table[filter].support * scale_ratio;

	STBR_UNIMPLEMENTED(output_w < input_w);

	for (n = 0; n < output_w; n++)
	{
		// What input texels contribute to this output texel?
		float out_texel_center = (float)n + 0.5f;
		float out_texel_influence_lowerbound = out_texel_center - out_pixels_radius;
		float out_texel_influence_upperbound = out_texel_center + out_pixels_radius;

		float in_center_of_out = out_texel_center / scale_ratio;
		float in_texel_influence_lowerbound = out_texel_influence_lowerbound / scale_ratio;
		float in_texel_influence_upperbound = out_texel_influence_upperbound / scale_ratio;

		int in_first_texel = (int)(floor(in_texel_influence_lowerbound + 0.5));
		int in_last_texel = (int)(floor(in_texel_influence_upperbound - 0.5));

		float total_filter = 0;
		float filter_scale;

		STBR_DEBUG_ASSERT(in_last_texel - in_first_texel <= stbr_info->kernel_texel_width);
		STBR_DEBUG_ASSERT(in_first_texel >= 0);
		STBR_DEBUG_ASSERT(in_last_texel < input_w);

		stbr__get_contributor(stbr_info, n)->n0 = in_first_texel;
		stbr__get_contributor(stbr_info, n)->n1 = in_last_texel;

		for (i = 0; i <= in_last_texel - in_first_texel; i++)
		{
			float in_texel_center = (float)(i + in_first_texel) + 0.5f;
			total_filter += *stbr__get_coefficient(stbr_info, n, i) = stbr__filter_info_table[filter].kernel(in_center_of_out - in_texel_center);
		}

		STBR_DEBUG_ASSERT(total_filter > 0);
		STBR_DEBUG_ASSERT(fabs(1-total_filter) < 0.1f); // Make sure it's not way off.

		// Make sure the sum of all coefficients is 1.
		filter_scale = 1 / total_filter;

		for (i = 0; i <= in_last_texel - in_first_texel; i++)
			*stbr__get_coefficient(stbr_info, n, i) *= filter_scale;
	}
}

STBRDEF int stbr_resize_arbitrary(const void* input_data, int input_w, int input_h, int input_stride_in_bytes,
	void* output_data, int output_w, int output_h, int output_stride_in_bytes,
	int channels, stbr_type type, stbr_filter filter,
	void* tempmem, stbr_size_t tempmem_size_in_bytes)
{
	int width_stride_input = input_stride_in_bytes ? input_stride_in_bytes : channels * input_w;
	int width_stride_output = output_stride_in_bytes ? output_stride_in_bytes : channels * output_w;

#ifdef STBR_DEBUG_OVERWRITE_TEST
#define OVERWRITE_ARRAY_SIZE 64
	unsigned char overwrite_contents_pre[OVERWRITE_ARRAY_SIZE];

	stbr_size_t begin_forbidden = width_stride_output * (output_h - 1) + output_w * channels;
	memcpy(overwrite_contents_pre, &((unsigned char*)output_data)[begin_forbidden], OVERWRITE_ARRAY_SIZE);
#endif

	STBR_UNIMPLEMENTED(type != STBR_TYPE_UINT8);

	STBR_ASSERT(filter != 0);
	STBR_ASSERT(filter < STBR_ARRAY_SIZE(stbr__filter_info_table));

	if (!tempmem)
		return 0;

	if (tempmem_size_in_bytes < stbr_calculate_memory(input_w, input_h, input_stride_in_bytes, output_w, output_h, output_stride_in_bytes, channels, STBR_FILTER_NEAREST))
		return 0;

	memset(tempmem, 0, tempmem_size_in_bytes);

	stbr__info* stbr_info = (stbr__info*)tempmem;

	stbr_info->total_coefficients = stbr__get_total_coefficients(filter, input_w, output_w);
	stbr_info->kernel_texel_width = stbr__get_filter_texel_width(filter, output_w > input_w ? 1 : 0);

#define STBR__NEXT_MEMPTR(current, old, newtype) (newtype*)(((unsigned char*)current) + old)

	stbr_info->horizontal_contributors = STBR__NEXT_MEMPTR(stbr_info, sizeof(stbr__info), stbr__contributors);
	stbr_info->horizontal_coefficients = STBR__NEXT_MEMPTR(stbr_info->horizontal_contributors, output_w * sizeof(stbr__contributors), float);
	stbr_info->decode_buffer = STBR__NEXT_MEMPTR(stbr_info->horizontal_coefficients, stbr_info->total_coefficients * sizeof(stbr__contributors), float);

#undef STBR__NEXT_MEMPTR

	stbr__calculate_horizontal_filters(stbr_info, filter, input_w, output_w);

#ifdef STBR_DEBUG_OVERWRITE_TEST
	STBR_DEBUG_ASSERT(memcmp(overwrite_contents_pre, &((unsigned char*)output_data)[begin_forbidden], OVERWRITE_ARRAY_SIZE) == 0);
#endif

	return 1;
}


STBRDEF stbr_size_t stbr_calculate_memory(int input_w, int input_h, int input_stride_in_bytes,
	int output_w, int output_h, int output_stride_in_bytes,
	int channels, stbr_filter filter)
{
	STBR_ASSERT(filter != 0);
	STBR_ASSERT(filter < STBR_ARRAY_SIZE(stbr__filter_info_table));

	int info_size = sizeof(stbr__info);
	int decode_buffer_size = input_w * channels * sizeof(float);
	int contributors_size = output_w * sizeof(stbr__contributors);
	int coefficients_size = stbr__get_total_coefficients(filter, input_w, output_w) * sizeof(float);

	return info_size + decode_buffer_size + contributors_size + coefficients_size;
}

#endif // STB_RESAMPLE_IMPLEMENTATION

/*
revision history:
*/
