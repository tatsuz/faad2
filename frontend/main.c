/*
** FAAD2 - Freeware Advanced Audio (AAC) Decoder including SBR decoding
** Copyright (C) 2003-2005 M. Bakker, Nero AG, http://www.nero.com
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
**
** Any non-GPL usage of this software or parts of this software is strictly
** forbidden.
**
** The "appropriate copyright message" mentioned in section 2c of the GPLv2
** must read: "Code from FAAD2 is copyright (c) Nero AG, www.nero.com"
**
** Commercial non-GPL licensing of this software is possible.
** For more info contact Nero AG through Mpeg4AAClicense@nero.com.
**
** $Id: main.c,v 1.86 2009/06/05 16:32:26 menno Exp $
**/

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
//#ifndef __MINGW32__
//#define off_t __int64
//#endif
#include <io.h>
#else
#define _FILE_OFFSET_BITS 64
#include <time.h>
#endif

#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <getopt.h>

#include <neaacdec.h>
#include <mp4ff.h>

#include "audio.h"

#ifndef min
#define min(a,b) ( (a) < (b) ? (a) : (b) )
#endif

#define MAX_CHANNELS 6 /* make this higher to support files with
                          more channels */

/* Macros for ADTS header*/
/* ADTS fixed header */
#define ADTS_IS_SYNCWORD(b) ( ((b)[0] == 0xFF)&&(((b)[1] & 0xF6) == 0xF0) )
#define ADTS_ID(b) ( ((b)[1] & 0x08) >> 3 )
#define ADTS_PROFILE(b) ( ((b)[2] & 0xC0) >> 6 )
#define ADTS_SAMPLING_FREQUENCY_INDEX(b) ( ((b)[2]&0x3c) >> 2 )
#define ADTS_CHANNEL_CONFIGURATION(b) ( (((b)[2] & 0x01) << 2) | (((b)[3] & 0xC0) >> 6) )
/* ADTS variable header */
#define ADTS_FRAME_LENGTH(b) ( ((((b)[3] & 0x03)) << 11)| (((b)[4] & 0xFF) << 3) | (((b)[5] & 0xE0) >> 5) )
#define ADTS_BUFFER_FULLNESS(b) ( (((b)[5] & 0x1F) << 6) | (((b)[6] & 0xFC) >> 2) )
#define ADTS_NUMBER_OF_RAW_DATA_BLOCKS_IN_FRAME(b) ( (b)[6] & 0x03 )

/* FAAD control flags */
#define FAADC_CHERROR_INIT  0x01 /* ON */
#define FAADC_CHERROR_SP    0x02 /* OFF */

#define FAADC_CHCHANGE_INIT 0x04 /* ON */
#define FAADC_CHCHANGE_SP   0x08 /* ON */

#define FAADC_HDCHANGE_INIT 0x10 /* OFF */
#define FAADC_HDCHANGE_SP   0x20 /* OFF */

#define FAADC_SRCHANGE_INIT 0x40 /* OFF */
#define FAADC_SRCHANGE_SP   0x80 /* OFF */

#define FAADC_ERROR_INIT    0x0100 /* ON */
#define FAADC_ERROR_OUT     0x0200 /* ON */
#define FAADC_ERROR_SPF     0x0400 /* OFF */
#define FAADC_ERROR_SPR     0x0800 /* OFF */

#define FAADC_BROKEN_INIT   0x1000 /* ON */
#define FAADC_BROKEN_OUT    0x2000 /* ON */
#define FAADC_BROKEN_SPF    0x4400 /* 0x4000 ON */
#define FAADC_BROKEN_SPR    0x8800 /* 0x8000 ON */

#define FAADC_FORCE_READ    0x10000 /* OFF */
#define FAADC_NO_TITLE      0x20000 /* OFF */
#define FAADC_ADJUST_PTS    0x30000 /* ON */

#define FAADC_DEFAULT_FLAGS 0x3F30D

#include "ts_buffer.h"

static int quiet = 0;

typedef struct {
	NeAACDecConfiguration config;
	/*int defObjectType;
	  int defSampleRate;
	  int outputFormat;
	  int downMatrix;
	  int useOldADTSFormat;*/
	int infoOnly;
	int writeToStdio;
	int fileType;
	int outfile_set;
	int adts_out;
	int showHelp;
	int noGapless;
	int delay;
	int delay_set;
	unsigned long flags;
	long first_frame;
	long last_frame;
	char aacFileName[256];
	char audioFileName[256];
	char adtsFileName[256];

	int program_number;
	int stream_index;
	unsigned short PID;
	unsigned short video_PID;
	double PTS;
	double video_PTS;
	int video_PTS_mode;
	ts_buffer *ts_buffer;
} faad_option;

static int faad_fprintf(FILE *stream, const char *fmt, ...)
{
	int result = 0;
    va_list ap;

    if (!quiet)
    {
        va_start(ap, fmt);

        result = vfprintf(stream, fmt, ap);

        va_end(ap);
    }
	return result;
}

/* FAAD file buffering routines */
typedef struct {
    long bytes_into_buffer;
    long bytes_consumed;
    off_t file_offset;
	off_t file_size;
    unsigned char *buffer;
    int at_eof;
	int frames;
	int total_frames;
	int total_frame_length;
    FILE *infile;

	ts_buffer *tsb;
	double PTS[2];
	unsigned char *pes_buffer;
	size_t pes_buffer_size;
	int pes_payload_bytes;
} aac_buffer;

#define TS_AAC_BUFFER_SIZE 131072

static int init_buffer(aac_buffer *b){
	memset(b, 0, sizeof(aac_buffer));

	b->pes_buffer_size = 65536;
	if (!(b->pes_buffer = (unsigned char*)malloc(b->pes_buffer_size)))
	{
		faad_fprintf(stderr, "Memory allocation error\n");
		return 0;
	}
	memset(b->pes_buffer, 0, b->pes_buffer_size);
	if (!(b->buffer = (unsigned char*)malloc(max(TS_AAC_BUFFER_SIZE, FAAD_MIN_STREAMSIZE*MAX_CHANNELS))))
	{
		faad_fprintf(stderr, "Memory allocation error\n");
		free(b->pes_buffer);
		return 0;
	}
	memset(b->buffer, 0, max(TS_AAC_BUFFER_SIZE, FAAD_MIN_STREAMSIZE*MAX_CHANNELS));
	return 1;
}
static void del_buffer(aac_buffer *b){
	if (b->infile) fclose(b->infile);
	free(b->pes_buffer);
	free(b->buffer);
}

static int load_pes_buffer(aac_buffer *b){
	int bread;
	const char *error;
	const unsigned char *payload;

	b->PTS[0] = b->PTS[1];
	b->PTS[1] = 0.0;
	do {
		bread = tsb_read_payload_unit(b->tsb, &(b->pes_buffer), &(b->pes_buffer_size), 0, 0, 0);
		if ( (error = tsb_last_error_message(b->tsb)) ){
			b->at_eof = tsb_is_eof(b->tsb);
			/* Ignore incomplete packet */
			if (!b->at_eof) {
				faad_fprintf(stderr, "Error reading file: %s\n", error);
			}
			return 0;
		}
		payload = skip_PES_header(b->pes_buffer, bread, &(b->pes_payload_bytes));
		if (!payload) {
			faad_fprintf(stderr, "Error in PES header\n");
			return 0;
		}
		if (b->bytes_consumed + b->bytes_into_buffer + b->pes_payload_bytes > TS_AAC_BUFFER_SIZE) {
			faad_fprintf(stderr, "Error full of buffer\n");
			return 0;
		}
		memcpy(b->buffer + b->bytes_consumed + b->bytes_into_buffer, payload, b->pes_payload_bytes);
		b->bytes_into_buffer += b->pes_payload_bytes;
	} while (!PESH_HAS_PTS(b->pes_buffer));
	b->PTS[1] = get_PTS(b->pes_buffer);
	return 1;
}

static int open_buffer(aac_buffer *b, const char *aacFileName, ts_buffer *tsb, unsigned short PID){
	int bread;
	
	/* TS file */
	if (tsb) {
		const unsigned char *sync = b->buffer + b->bytes_consumed;

		b->tsb = tsb;
		b->infile = NULL;
		b->file_size = tsb->file_size;
		tsb_set_PID(b->tsb, PID);
		if (!tsb_seek_head(b->tsb)) {
			tsb_last_error_message(b->tsb);
			faad_fprintf(stderr, "Error seeking file: %s\n", aacFileName);
			return 0;
		}
		if (!load_pes_buffer(b)) {
			return 0;
		}
		/* Check and adjust syncword */
		while ( (sync = memchr(sync, 0xFF, b->buffer+b->bytes_consumed+b->bytes_into_buffer - sync))
			&& ((sync[1] & 0xF6) != 0xF0)) {++sync;}
		if (!sync) {
			faad_fprintf(stderr, "Error syncword is not found: %s\n", aacFileName);
			return 0;
		}
		if (b->buffer != sync) 
			faad_fprintf(stderr, "Adjust syncword\n");
		memmove(b->buffer, sync, b->buffer+b->bytes_consumed+b->bytes_into_buffer - sync);
		b->bytes_consumed = 0;
		b->bytes_into_buffer -= sync - b->buffer;
		/* 2 PES packets in the aac buffer */
		if (!load_pes_buffer(b)) {
			return 0;
		}
		b->at_eof = tsb_is_eof(b->tsb);
		return 1;
	}

	/* AAC file */
	b->infile = fopen(aacFileName, "rb");
	if (!b->infile)
	{
		/* unable to open file */
		faad_fprintf(stderr, "Error opening file: %s\n", aacFileName);
		return 0;
	}

	fseek(b->infile, 0, SEEK_END);
	b->file_size = ftell(b->infile);
	fseek(b->infile, 0, SEEK_SET);

	bread = fread(b->buffer, 1, FAAD_MIN_STREAMSIZE*MAX_CHANNELS, b->infile);
	b->bytes_into_buffer = bread;
	b->bytes_consumed = 0;
	b->file_offset = 0;
	b->frames = 0;

	if (bread != FAAD_MIN_STREAMSIZE*MAX_CHANNELS)
		b->at_eof = 1;
	return 1;
}

static int fill_buffer(aac_buffer *b)
{
    int bread;

    if (b->bytes_consumed > 0)
    {
        if (b->bytes_into_buffer)
        {
            memmove((void*)b->buffer, (void*)(b->buffer + b->bytes_consumed),
                b->bytes_into_buffer*sizeof(unsigned char));
        }
		b->PTS[0] = 0.0;

        if (!b->at_eof)
        {
			if (b->infile) {
				bread = fread((void*)(b->buffer + b->bytes_into_buffer), 1,
					b->bytes_consumed, b->infile);

				if (bread != b->bytes_consumed)
					b->at_eof = 1;

				b->bytes_into_buffer += bread;

			} else if (b->tsb) {
				/* 2 PES packets in the aac buffer */
				b->bytes_consumed = 0;
				while (b->pes_payload_bytes >= b->bytes_into_buffer 
					&& load_pes_buffer(b)) {}
			}
        }

        b->bytes_consumed = 0;

        if (b->bytes_into_buffer > 3)
        {
            if (memcmp(b->buffer, "TAG", 3) == 0)
                b->bytes_into_buffer = 0;
        }
        if (b->bytes_into_buffer > 11)
        {
            if (memcmp(b->buffer, "LYRICSBEGIN", 11) == 0)
                b->bytes_into_buffer = 0;
        }
        if (b->bytes_into_buffer > 8)
        {
            if (memcmp(b->buffer, "APETAGEX", 8) == 0)
                b->bytes_into_buffer = 0;
        }
    }

    return 1;
}

static void advance_buffer(aac_buffer *b, int bytes)
{
    b->file_offset += bytes;
	if (b->tsb)
		b->file_offset = tsb_get_file_pos(b->tsb);
    b->bytes_consumed = bytes;
    b->bytes_into_buffer -= bytes;
	if (b->bytes_into_buffer < 0)
		b->bytes_into_buffer = 0;
}

static int adts_sample_rates[] = {96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350,0,0,0};

static int adts_parse(aac_buffer *b, int *bitrate, float *length)
{
    int frames, frame_length;
    int t_framelength = 0;
    int samplerate;
    float frames_per_sec, bytes_per_frame;

    /* Read all frames to ensure correct time and bitrate */
    for (frames = 0; /* */; frames++)
    {
        fill_buffer(b);

        if (b->bytes_into_buffer > 7)
        {
            /* check syncword */
            if (!ADTS_IS_SYNCWORD(b->buffer))
                break;

            if (frames == 0)
                samplerate = adts_sample_rates[ADTS_SAMPLING_FREQUENCY_INDEX(b->buffer)];

            frame_length = ADTS_FRAME_LENGTH(b->buffer);

            t_framelength += frame_length;

			if (frame_length > b->bytes_into_buffer){
				faad_fprintf(stderr, "The last frame (%d) may be broken.\n", frames);
                break;
			}

            advance_buffer(b, frame_length);
        } else {
            break;
        }
    }

	b->total_frames = frames;
	b->total_frame_length = t_framelength;
    frames_per_sec = (float)samplerate/1024.0f;
    if (frames != 0)
        bytes_per_frame = (float)t_framelength/(float)(frames*1000);
    else
        bytes_per_frame = 0;
    *bitrate = (int)(8. * bytes_per_frame * frames_per_sec + 0.5);
    if (frames_per_sec != 0)
        *length = (float)frames/frames_per_sec;
    else
        *length = 1;

    return 1;
}



uint32_t read_callback(void *user_data, void *buffer, uint32_t length)
{
    return fread(buffer, 1, length, (FILE*)user_data);
}

uint32_t seek_callback(void *user_data, uint64_t position)
{
    return fseek((FILE*)user_data, position, SEEK_SET);
}

/* MicroSoft channel definitions */
#define SPEAKER_FRONT_LEFT             0x1
#define SPEAKER_FRONT_RIGHT            0x2
#define SPEAKER_FRONT_CENTER           0x4
#define SPEAKER_LOW_FREQUENCY          0x8
#define SPEAKER_BACK_LEFT              0x10
#define SPEAKER_BACK_RIGHT             0x20
#define SPEAKER_FRONT_LEFT_OF_CENTER   0x40
#define SPEAKER_FRONT_RIGHT_OF_CENTER  0x80
#define SPEAKER_BACK_CENTER            0x100
#define SPEAKER_SIDE_LEFT              0x200
#define SPEAKER_SIDE_RIGHT             0x400
#define SPEAKER_TOP_CENTER             0x800
#define SPEAKER_TOP_FRONT_LEFT         0x1000
#define SPEAKER_TOP_FRONT_CENTER       0x2000
#define SPEAKER_TOP_FRONT_RIGHT        0x4000
#define SPEAKER_TOP_BACK_LEFT          0x8000
#define SPEAKER_TOP_BACK_CENTER        0x10000
#define SPEAKER_TOP_BACK_RIGHT         0x20000
#define SPEAKER_RESERVED               0x80000000

static long aacChannelConfig2wavexChannelMask(NeAACDecFrameInfo *hInfo)
{
    if (hInfo->channels == 6 && hInfo->num_lfe_channels)
    {
        return SPEAKER_FRONT_LEFT + SPEAKER_FRONT_RIGHT +
            SPEAKER_FRONT_CENTER + SPEAKER_LOW_FREQUENCY +
            SPEAKER_BACK_LEFT + SPEAKER_BACK_RIGHT;
    } else {
        return 0;
    }
}

static char *position2string(int position)
{
    switch (position)
    {
    case FRONT_CHANNEL_CENTER: return "Center front";
    case FRONT_CHANNEL_LEFT:   return "Left front";
    case FRONT_CHANNEL_RIGHT:  return "Right front";
    case SIDE_CHANNEL_LEFT:    return "Left side";
    case SIDE_CHANNEL_RIGHT:   return "Right side";
    case BACK_CHANNEL_LEFT:    return "Left back";
    case BACK_CHANNEL_RIGHT:   return "Right back";
    case BACK_CHANNEL_CENTER:  return "Center back";
    case LFE_CHANNEL:          return "LFE";
    case UNKNOWN_CHANNEL:      return "Unknown";
    default: return "";
    }

    return "";
}

static void print_channel_info(NeAACDecFrameInfo *frameInfo)
{
    /* print some channel info */
    int i;
    long channelMask = aacChannelConfig2wavexChannelMask(frameInfo);

    faad_fprintf(stderr, "  ---------------------\n");
    if (frameInfo->num_lfe_channels > 0)
    {
        faad_fprintf(stderr, " | Config: %2d.%d Ch     |", frameInfo->channels-frameInfo->num_lfe_channels, frameInfo->num_lfe_channels);
    } else {
        faad_fprintf(stderr, " | Config: %2d Ch       |", frameInfo->channels);
    }
    if (channelMask)
        faad_fprintf(stderr, " WARNING: channels are reordered according to\n");
    else
        faad_fprintf(stderr, "\n");
    faad_fprintf(stderr, "  ---------------------");
    if (channelMask)
        faad_fprintf(stderr, "  MS defaults defined in WAVE_FORMAT_EXTENSIBLE\n");
    else
        faad_fprintf(stderr, "\n");
    faad_fprintf(stderr, " | Ch |    Position    |\n");
    faad_fprintf(stderr, "  ---------------------\n");
    for (i = 0; i < frameInfo->channels; i++)
    {
        faad_fprintf(stderr, " | %.2d | %-14s |\n", i, position2string((int)frameInfo->channel_position[i]));
    }
    faad_fprintf(stderr, "  ---------------------\n");
    faad_fprintf(stderr, "\n");
}

static int FindAdtsSRIndex(int sr)
{
    int i;

    for (i = 0; i < 16; i++)
    {
        if (sr == adts_sample_rates[i])
            return i;
    }
    return 16 - 1;
}

static unsigned char *MakeAdtsHeader(int *dataSize, NeAACDecFrameInfo *hInfo, int old_format)
{
    unsigned char *data;
    int profile = (hInfo->object_type - 1) & 0x3;
    int sr_index = ((hInfo->sbr == SBR_UPSAMPLED) || (hInfo->sbr == NO_SBR_UPSAMPLED)) ?
        FindAdtsSRIndex(hInfo->samplerate / 2) : FindAdtsSRIndex(hInfo->samplerate);
    int skip = (old_format) ? 8 : 7;
    int framesize = skip + hInfo->bytesconsumed;

    if (hInfo->header_type == ADTS)
        framesize -= skip;

    *dataSize = 7;

    data = malloc(*dataSize * sizeof(unsigned char));
    memset(data, 0, *dataSize * sizeof(unsigned char));

    data[0] += 0xFF; /* 8b: syncword */

    data[1] += 0xF0; /* 4b: syncword */
    /* 1b: mpeg id = 0 */
    /* 2b: layer = 0 */
    data[1] += 1; /* 1b: protection absent */

    data[2] += ((profile << 6) & 0xC0); /* 2b: profile */
    data[2] += ((sr_index << 2) & 0x3C); /* 4b: sampling_frequency_index */
    /* 1b: private = 0 */
    data[2] += ((hInfo->channels >> 2) & 0x1); /* 1b: channel_configuration */

    data[3] += ((hInfo->channels << 6) & 0xC0); /* 2b: channel_configuration */
    /* 1b: original */
    /* 1b: home */
    /* 1b: copyright_id */
    /* 1b: copyright_id_start */
    data[3] += ((framesize >> 11) & 0x3); /* 2b: aac_frame_length */

    data[4] += ((framesize >> 3) & 0xFF); /* 8b: aac_frame_length */

    data[5] += ((framesize << 5) & 0xE0); /* 3b: aac_frame_length */
    data[5] += ((0x7FF >> 6) & 0x1F); /* 5b: adts_buffer_fullness */

    data[6] += ((0x7FF << 2) & 0x3F); /* 6b: adts_buffer_fullness */
    /* 2b: num_raw_data_blocks */

    return data;
}

/* globals */
char *progName;

static const char *file_ext[] =
{
    NULL,
    ".wav",
    ".aif",
    ".au",
    ".au",
    ".pcm",
    NULL
};

static void usage(void)
{
    faad_fprintf(stdout, "\nUsage:\n");
    faad_fprintf(stdout, "%s [options] infile.aac\n", progName);
    faad_fprintf(stdout, "Options:\n");
    faad_fprintf(stdout, " -h    Shows this help screen.\n");
    faad_fprintf(stdout, " -i    Shows info about the input file.\n");
    faad_fprintf(stdout, " -a X  Write MPEG-4 AAC ADTS output file.\n");
    faad_fprintf(stdout, " -t    Assume old ADTS format.\n");
    faad_fprintf(stdout, " -o X  Set output filename.\n");
    faad_fprintf(stdout, " -f X  Set output format. Valid values for X are:\n");
    faad_fprintf(stdout, "        1:  Microsoft WAV format (default).\n");
    faad_fprintf(stdout, "        2:  RAW PCM data.\n");
    faad_fprintf(stdout, " -F X  Set control flags (default: 0x3F30D) (EXPERIMENTAL).\n");
    faad_fprintf(stdout, "        0x1: Reinitialize at the channel error (default).\n");
    faad_fprintf(stdout, "        0x2: Split at the channel error (-S).\n");
    faad_fprintf(stdout, "        0x4: Reinitialize at the channel change (default).\n");
    faad_fprintf(stdout, "        0x8: Split at the channel change (default).\n");
    faad_fprintf(stdout, "        0x10: Reinitialize at the ADTS channel change (-S).\n");
    faad_fprintf(stdout, "        0x20: Split at the ADTS channel change (-S).\n");
    faad_fprintf(stdout, "        0x40: Reinitialize at the samplerate change.\n");
    faad_fprintf(stdout, "        0x80: Split at the samplerate change.\n");
    faad_fprintf(stdout, "        0x100: Reinitialize at error frames (default).\n");
    faad_fprintf(stdout, "        0x200: Output error frames (default).\n");
    faad_fprintf(stdout, "        0x400: Split before error frames.\n");
    faad_fprintf(stdout, "        0x800: Split after error frames.\n");
    faad_fprintf(stdout, "        0x1000: Reinitialize at broken frames (default).\n");
    faad_fprintf(stdout, "        0x2000: Output broken frames (default).\n");
    faad_fprintf(stdout, "        0x4000: Split before broken frames (default).\n");
    faad_fprintf(stdout, "        0x8000: Split after broken frames (default).\n");
    faad_fprintf(stdout, "        0x10000: Force to read the first frame.\n");
    faad_fprintf(stdout, "        0x20000: Don't print console title (Windows).\n");
    faad_fprintf(stdout, "        0x20000: Don't print console title (Windows).\n");
    faad_fprintf(stdout, "        0x30000: Adjust length to PTS (default).\n");
    faad_fprintf(stdout, " -b X  Set output sample format. Valid values for X are:\n");
    faad_fprintf(stdout, "        1:  16 bit PCM data (default).\n");
    faad_fprintf(stdout, "        2:  24 bit PCM data.\n");
    faad_fprintf(stdout, "        3:  32 bit PCM data.\n");
    faad_fprintf(stdout, "        4:  32 bit floating point data.\n");
    faad_fprintf(stdout, "        5:  64 bit floating point data.\n");
    faad_fprintf(stdout, " -s X  Force the samplerate to X (for RAW files).\n");
	faad_fprintf(stdout, " -S    Split every channel change.\n");
    faad_fprintf(stdout, " -l X  Set object type. Supported object types:\n");
    faad_fprintf(stdout, "        1:  Main object type.\n");
    faad_fprintf(stdout, "        2:  LC (Low Complexity) object type.\n");
    faad_fprintf(stdout, "        4:  LTP (Long Term Prediction) object type.\n");
    faad_fprintf(stdout, "        23: LD (Low Delay) object type.\n");
    faad_fprintf(stdout, " -d    Down matrix 5.1 to 2 channels\n");
    faad_fprintf(stdout, " -D X  Delay in ms\n");
    faad_fprintf(stdout, " -w    Write output to stdio instead of a file.\n");
    faad_fprintf(stdout, " -g    Disable gapless decoding.\n");
    faad_fprintf(stdout, " -q    Quiet - suppresses status messages.\n");
	faad_fprintf(stdout, " -E X:Y Extract between X frame and Y frame (default 1:0)\n");
	faad_fprintf(stdout, "\nOptions for TS:\n");
	faad_fprintf(stdout, " --program X    Decode aac of the program_number. (default: auto)\n");
	faad_fprintf(stdout, " --stream  X    Decode aac of the program_number No.X stream. (default: 1)\n");
	faad_fprintf(stdout, " -P X           Decode aac of the PID X. (default: auto)\n");
	faad_fprintf(stdout, " --video-pid X  Use PID X video for delay calculation.\n");
	faad_fprintf(stdout, " --pts-base X   Set video PTS base point.\n");
	faad_fprintf(stdout, "                 0: First PES packet\n");
	faad_fprintf(stdout, "                 1: PES packet of 1st frame of 1st GOP (default)\n");
	faad_fprintf(stdout, "                 2: PES packet of 1st B of 1st GOP\n");
	faad_fprintf(stdout, "                 3: PES packet of 1st B of 1st GOP from I\n");
	faad_fprintf(stdout, "                 4: PES packet of 1st I frame\n");
    faad_fprintf(stdout, "Example:\n");
    faad_fprintf(stdout, "       %s infile.aac\n", progName);
    faad_fprintf(stdout, "       %s infile.mp4\n", progName);
	faad_fprintf(stdout, "       %s infile.ts\n", progName);
    faad_fprintf(stdout, "       %s -o outfile.wav infile.aac\n", progName);
    faad_fprintf(stdout, "       %s -w infile.aac > outfile.wav\n", progName);
    faad_fprintf(stdout, "       %s -a outfile.aac infile.aac\n", progName);
    return;
}

char *make_new_filename(char *new_fn, const char *old_fn, int number)
{
	char *extension;
	char buf[256];
	
	sprintf(buf, "[%d]", number);
	sprintf(new_fn, "%.*s", 256-1-strlen(buf), old_fn);

	extension = strrchr(new_fn, '.');
	if (extension) {
		*extension = '\0';
		sprintf(buf, "%s[%d].%s", new_fn, number, extension+1);
	} else {
		sprintf(buf, "%s[%d]", new_fn, number);
	}
	
	strcpy(new_fn, buf);
	
	return new_fn;
}

size_t find_next_syncword(aac_buffer *b, FILE *skip_data)
{
	size_t skip = 0;

	fill_buffer(b);

	while (b->bytes_into_buffer >= 2) {
		const unsigned char *sync = memchr(b->buffer, 0xFF, b->bytes_into_buffer-1);
		if (sync && ((sync[1] & 0xF6) == 0xF0) && !(skip == 0 && b->buffer == sync)) {
			/*syncword*/
			long length = sync - b->buffer;
			if (skip_data) {
				fwrite(b->buffer, 1, length, skip_data);
			}
			skip += length;
			advance_buffer(b, length);
			fill_buffer(b);
			return skip;
		} else {
			long length;
			if (sync) {
				length = sync - b->buffer + 1;
			} else {
				length = b->bytes_into_buffer - 1;
			}
			if (b->at_eof && b->bytes_into_buffer - length < 2) {
				length = b->bytes_into_buffer;
			}
			if (skip_data) {
				fwrite(b->buffer, 1, length, skip_data);
			}
			skip += length;
			advance_buffer(b, length);
			fill_buffer(b);
		}
	}
	return 0;
}

int adjust_PTS(audio_file *aufile, const aac_buffer *b, double pts_diff, unsigned long decode_samplerate, unsigned char decode_channels) {
	if (pts_diff || (aufile->base_PTS && b->PTS[0])) {
		int samples;
		if (!pts_diff) {
			/* PTS is available */
			pts_diff = b->PTS[0] - (aufile->base_PTS+length_of_audio_file(aufile));

			if (b->PTS[0] < aufile->base_PTS) {
				/* PTS wrap around */
				pts_diff += PTS_MAX_TIME;
				//faad_fprintf(stderr, "Frame %d : PTS wrap around. \n", b->frames);
			}
		}
		samples = (int)(decode_samplerate * pts_diff / 1000.0 + 0.5);

		if (0 < samples && pts_diff < 60000.0) {
			faad_fprintf(stderr, "Frame %d : PTS difference %d samples (%dms) were inserted. \n", b->frames, samples, (int)(pts_diff+0.5));
			if (write_blank_audio_file(aufile, samples*decode_channels) != samples*decode_channels) {
				return 0;
			}
		} else if (pts_diff < -1.0){
			faad_fprintf(stderr, "Frame %d : PTS difference %d samples (%dms) were invalid. \n", b->frames, samples, (int)(pts_diff+0.5));
			return 0;
		}
		return 1;
	}
	return 0;
}

static int decodeAACfile(const faad_option *o, float *song_length)
{
    int tagsize;
    unsigned long samplerate, decode_samplerate = 0, previous_adts_header_sr = -1;
    unsigned char channels, decode_channels = 0, previous_adts_header_ch = -1;
    void *sample_buffer;

    audio_file *aufile = NULL;

    FILE *adtsFile = NULL;
    //unsigned char *adtsData;
    int adtsDataSize;

    NeAACDecHandle hDecoder;
    NeAACDecFrameInfo frameInfo = {0};
    NeAACDecConfigurationPtr config;

	char sndnewfile[256];
	char adtsnew_fn[256];
	int file_number = 0;
	long delay_samples;
	int skip_frames = 0;
    char percents[200];
    int percent, old_percent = -1;
    int bread;
    int header_type = 0;
    int bitrate = 0;
    float length = 0;

    int first_time = 1;

	int reinit_libfaad = 0, switch_newfile = 0, adjust_pts = 0;
	double pts_diff = 0.0, last_pts = 0.0;
	enum {error_ok = 0, error_skip, error_seek, error_retry} error_handling = error_ok;

    aac_buffer b;

	if (!o->adts_out && !o->writeToStdio) strcpy(sndnewfile, o->audioFileName);

    if (!init_buffer(&b))
    {
        // faad_fprintf(stderr, "Memory allocation error\n");
        return 0;
    }
    if (!open_buffer(&b, o->aacFileName, o->ts_buffer, o->PID))
    {
        /* unable to open file */
        // faad_fprintf(stderr, "Error opening file: %s\n", o->aacFileName);
        return 1;
    }

    if (o->adts_out)
    {
        adtsFile = fopen(o->adtsFileName, "wb");
        if (adtsFile == NULL)
        {
            faad_fprintf(stderr, "Error opening file: %s\n", o->adtsFileName);
            return 1;
        }
    }

    tagsize = 0;
    if (!memcmp(b.buffer, "ID3", 3))
    {
        /* high bit is not used */
        tagsize = (b.buffer[6] << 21) | (b.buffer[7] << 14) |
            (b.buffer[8] <<  7) | (b.buffer[9] <<  0);

        tagsize += 10;
        advance_buffer(&b, tagsize);
        fill_buffer(&b);
    }

    hDecoder = NeAACDecOpen();

    /* Set the default object type and samplerate */
    /* This is useful for RAW AAC files */
    config = NeAACDecGetCurrentConfiguration(hDecoder);
    if (o->config.defSampleRate)
        config->defSampleRate = o->config.defSampleRate;
    config->defObjectType = o->config.defObjectType;
    config->outputFormat = o->config.outputFormat;
    config->downMatrix = o->config.downMatrix;
    config->useOldADTSFormat = o->config.useOldADTSFormat;
    //config->dontUpSampleImplicitSBR = 1;
    NeAACDecSetConfiguration(hDecoder, config);

    /* get AAC infos for printing */
    header_type = 0;
	if (b.tsb)
	{
		header_type = 3;
	} else if (ADTS_IS_SYNCWORD(b.buffer)){
        adts_parse(&b, &bitrate, &length);
        fseek(b.infile, tagsize, SEEK_SET);

        bread = fread(b.buffer, 1, FAAD_MIN_STREAMSIZE*MAX_CHANNELS, b.infile);
        if (bread != FAAD_MIN_STREAMSIZE*MAX_CHANNELS)
            b.at_eof = 1;
        else
            b.at_eof = 0;
        b.bytes_into_buffer = bread;
        b.bytes_consumed = 0;
        b.file_offset = tagsize;

        header_type = 1;
    } else if (memcmp(b.buffer, "ADIF", 4) == 0) {
        int skip_size = (b.buffer[4] & 0x80) ? 9 : 0;
        bitrate = ((unsigned int)(b.buffer[4 + skip_size] & 0x0F)<<19) |
            ((unsigned int)b.buffer[5 + skip_size]<<11) |
            ((unsigned int)b.buffer[6 + skip_size]<<3) |
            ((unsigned int)b.buffer[7 + skip_size] & 0xE0);

        length = (float)b.file_size;
        if (length != 0)
        {
            length = ((float)length*8.f)/((float)bitrate) + 0.5f;
        }

        bitrate = (int)((float)bitrate/1000.0f + 0.5f);

        header_type = 2;
    }

    *song_length = length;

    fill_buffer(&b);
    if ((bread = NeAACDecInit(hDecoder, b.buffer,
        b.bytes_into_buffer, &samplerate, &channels)) < 0)
    {
        /* If some error initializing occured, skip the file */
        faad_fprintf(stderr, "Error initializing decoder library.\n");
        NeAACDecClose(hDecoder);
		del_buffer(&b);
        return 1;
    }
    advance_buffer(&b, bread);
    fill_buffer(&b);

	/*calc delay*/
	delay_samples = o->delay * (long)samplerate / 1000;

    /* print AAC file info */
    faad_fprintf(stderr, "%s file info:\n", o->aacFileName);
    switch (header_type)
    {
    case 0:
        faad_fprintf(stderr, "RAW\n\n");
        break;
    case 1:
        faad_fprintf(stderr, "ADTS, %.3f sec (%d frames), %d kbps, %d Hz\n\n",
            length, b.total_frames, bitrate, samplerate);
        break;
    case 2:
        faad_fprintf(stderr, "ADIF, %.3f sec, %d kbps, %d Hz\n\n",
            length, bitrate, samplerate);
        break;
	case 3:
		faad_fprintf(stderr, "TS ADTS\n");
		faad_fprintf(stderr, "Program Association Table:\n");
		print_PAT(faad_fprintf, stderr, b.tsb->pat, 0);
		faad_fprintf(stderr, "\n");
		faad_fprintf(stderr, "Program  : %d (0x%X)\n", o->program_number, o->program_number);
		faad_fprintf(stderr, "AAC   PID: 0x%-4X PTS: %.1f [ms]\n", o->PID, o->PTS);
		faad_fprintf(stderr, "Video PID: 0x%-4X PTS: %.1f [ms]\n\n", o->video_PID, o->video_PTS);
		header_type = 1;
		break;
    }

	if (o->delay) faad_fprintf(stderr, "Delay        : %d [ms]\n", o->delay);
	faad_fprintf(stderr, "Decode range : %ld to %ld frame.\n\n", o->first_frame, o->last_frame);
	
    if (o->infoOnly)
    {
        NeAACDecClose(hDecoder);
		del_buffer(&b);
        return 0;
    }


	previous_adts_header_ch = ADTS_CHANNEL_CONFIGURATION(b.buffer);
	previous_adts_header_sr = ADTS_SAMPLING_FREQUENCY_INDEX(b.buffer);
    do
    {
		unsigned char adts_header_ch = ADTS_CHANNEL_CONFIGURATION(b.buffer);
		unsigned char adts_header_sr = ADTS_SAMPLING_FREQUENCY_INDEX(b.buffer);
		int frame_length = 0;

		if (o->last_frame && o->last_frame <= b.frames) break;
		/* Reinitialize libfaad */
		assert(error_handling == error_ok || (error_handling != error_ok && reinit_libfaad));
		if (reinit_libfaad) { //if (error_handling != ok) {
			int bread;

			faad_fprintf(stderr, "Frame %d : Reinitializing LIBFAAD2 \n", b.frames);

			NeAACDecClose(hDecoder);
			hDecoder = NeAACDecOpen();

			/* Set the default object type and samplerate */
			/* This is useful for RAW AAC files */
			config = NeAACDecGetCurrentConfiguration(hDecoder);
			if (o->config.defSampleRate)
				config->defSampleRate = o->config.defSampleRate;
			config->defObjectType = o->config.defObjectType;
			config->outputFormat = o->config.outputFormat;
			config->downMatrix = o->config.downMatrix;
			config->useOldADTSFormat = o->config.useOldADTSFormat;
			//config->dontUpSampleImplicitSBR = 1;
			NeAACDecSetConfiguration(hDecoder, config);
			if ((bread = NeAACDecInit(hDecoder, b.buffer,
				b.bytes_into_buffer, &samplerate, &channels)) >= 0)
			{
				advance_buffer(&b, bread);
				fill_buffer(&b);
			} else {
				/* If some error initializing occured, skip the file */
				faad_fprintf(stderr, "Error initializing decoder library.\n");
				goto skip_frame;
				break;
			}
			reinit_libfaad = 0;
		}
		/* Reinitialize libfaad */

		/* Decode AAC */
		if (o->first_frame-1 <= b.frames || header_type != 1) {
#ifdef _WIN32
			__try {
#endif
        sample_buffer = NeAACDecDecode(hDecoder, &frameInfo,
            b.buffer, b.bytes_into_buffer);
#ifdef _WIN32
			} __except(EXCEPTION_EXECUTE_HANDLER) {
				DWORD error_code = GetExceptionCode();
				faad_fprintf(stderr, "libfaad critical error ExceptionCode : 0x%lx", error_code);
				sample_buffer = NULL;
				reinit_libfaad = 1;
				if (error_handling == error_retry) {
					break;
				} else {
					error_handling = error_retry;
					continue;
				}
			}
#endif
		}
		/* Decode AAC */
       
		/* Error check */
		if (frameInfo.error) {
			faad_fprintf(stderr, "Frame %d : Error '%s' \n", b.frames,
				NeAACDecGetErrorMessage(frameInfo.error));
		} else {
			if (decode_channels != frameInfo.channels && decode_channels && error_handling != error_retry) {
				faad_fprintf(stderr, "Frame %d : Decode channels changed from %dch to %dch \n",
					b.frames, decode_channels, frameInfo.channels);
			}
			if (decode_samplerate != frameInfo.samplerate && decode_samplerate && error_handling != error_retry) {
				faad_fprintf(stderr, "Frame %d : Decode samplerate changed from %dHz to %dHz \n",
					b.frames, decode_samplerate, frameInfo.samplerate);
			}
		}
		if (header_type == 1 && previous_adts_header_ch != adts_header_ch && error_handling != error_retry) {
			faad_fprintf(stderr, "Frame %d : Frame header ch changed from %dch to %dch \n",
				b.frames, previous_adts_header_ch, adts_header_ch);
		}
		if (header_type == 1 && previous_adts_header_sr != adts_header_sr && error_handling != error_retry) {
			assert(previous_adts_header_sr < 16 && adts_header_sr < 16);
			faad_fprintf(stderr, "Frame %d : Frame header samplerate changed from %dHz to %dHz \n",
				b.frames, adts_sample_rates[previous_adts_header_sr], adts_sample_rates[adts_header_sr]);
		}

		if (o->first_frame > b.frames) {
			if (header_type == 1) {
				if (
					b.bytes_into_buffer > 7 &&
					ADTS_IS_SYNCWORD(b.buffer) &&
					ADTS_FRAME_LENGTH(b.buffer) <= FAAD_MIN_STREAMSIZE*MAX_CHANNELS
				) {
					frame_length = ADTS_FRAME_LENGTH(b.buffer);
				} else {
					find_next_syncword(&b, NULL);
					frame_length = 0;
				}
			} else {
				frame_length = frameInfo.bytesconsumed;
				if (frameInfo.error) break;
			}
			advance_buffer(&b, min(frame_length, b.bytes_into_buffer));
			fill_buffer(&b);
			b.frames++;
			previous_adts_header_ch = adts_header_ch;
			previous_adts_header_sr = adts_header_sr;
			continue;

		} else if (frameInfo.error == 0) {
			if ((decode_samplerate != frameInfo.samplerate && decode_samplerate)
				|| (header_type == 1 && adts_header_sr != previous_adts_header_sr)
			) {
				/* Samplerate change */
				if (!first_time
					&& (o->flags & FAADC_SRCHANGE_SP)
				) {
					switch_newfile = 1;
				}
				if (o->flags & FAADC_ADJUST_PTS) {
					adjust_pts = 1;
				}
				if (error_handling != error_retry && (o->flags & FAADC_SRCHANGE_INIT)) {
					error_handling = error_retry;
					reinit_libfaad = 1;
					continue;
				}
			} else if (decode_channels != frameInfo.channels && decode_channels) {
				/* Channel configuration change */
				// for error_retry from frameInfo.error == 21
				if (!first_time
					&& ((o->adts_out && (o->flags & FAADC_CHCHANGE_SP))
						|| !o->adts_out)
				) {
					switch_newfile = 1;
				}
				if (o->flags & FAADC_ADJUST_PTS) {
					adjust_pts = 1;
				}
				if (error_handling != error_retry && (o->flags & FAADC_CHCHANGE_INIT)) {
					error_handling = error_retry;
					reinit_libfaad = 1;
					continue;
				}
			} else if (header_type == 1 && adts_header_ch != previous_adts_header_ch) {
				/* Channel configuration change */
				// for error_retry from frameInfo.error == 21
				if (!first_time && (o->flags & FAADC_HDCHANGE_SP)) {
					switch_newfile = 1;
				}
				if (o->flags & FAADC_ADJUST_PTS) {
					adjust_pts = 1;
				}
				if (error_handling != error_retry && (o->flags & FAADC_HDCHANGE_INIT)) {
					error_handling = error_retry;
					reinit_libfaad = 1;
					continue;
				}
			}
			/* OK */
			if (!(frameInfo.samples/frameInfo.channels == 1024 || frameInfo.samples== 0
				|| (frameInfo.object_type==LD && frameInfo.samples/frameInfo.channels == 512)
				|| (frameInfo.object_type==HE_AAC && frameInfo.samples/frameInfo.channels == 2048))
			) {
				faad_fprintf(stderr, "Frame %d : Invalid samples (%d). \n", b.frames, frameInfo.samples);
			}
			error_handling = error_ok;
			frame_length = frameInfo.bytesconsumed;

		} else { //if (frameInfo.error > 0 || !sample_buffer)
			/* Skip or retry error frame */
			assert(frameInfo.error > 0 && !sample_buffer);
			assert(frameInfo.bytesconsumed == 0);
			assert(frameInfo.error != 21 || (frameInfo.error == 21 && error_handling != error_retry));

			skip_frame: /* Jump from error initializing decoder library */
			if (o->flags & FAADC_ADJUST_PTS) {
				adjust_pts = 1;
			}
			if (frameInfo.error == 21 && error_handling != error_retry) {
				/* Channel configuration change */
				if (!first_time && (o->flags & FAADC_CHERROR_SP)) {
					switch_newfile = 1;
				}
				if (error_handling != error_retry && (o->flags & FAADC_CHERROR_INIT)) {
					error_handling = error_retry;
					reinit_libfaad = 1;
					continue;
				}

			} else if (
				header_type == 1 &&
				b.bytes_into_buffer > 7 &&
				ADTS_IS_SYNCWORD(b.buffer) &&
				ADTS_FRAME_LENGTH(b.buffer) <= FAAD_MIN_STREAMSIZE*MAX_CHANNELS
			) {
				/* ADTS frame header may be valid */
				if (error_handling == error_skip || error_handling == error_seek) {
					switch_newfile = 0;
				} else if (!first_time && (o->flags & FAADC_ERROR_SPF)) {
					switch_newfile = 1;
				}
				if (error_handling != error_seek) error_handling = error_skip;
				if (o->flags & FAADC_ERROR_INIT) reinit_libfaad = 1;
				frame_length = ADTS_FRAME_LENGTH(b.buffer);

			} else if (header_type == 1) {
				/* ADTS frame header is broken */
				if (error_handling == error_seek || (error_handling == error_skip && (o->flags & FAADC_ERROR_SPF))) {
					switch_newfile = 0;
				} else if (!first_time && (o->flags & FAADC_BROKEN_SPF)) {
					switch_newfile = 1;
				}
				error_handling = error_seek;
				if (o->flags & FAADC_BROKEN_INIT) reinit_libfaad = 1;
				frame_length = 0;
			} else {
				/* Can't skip this frame */
				// Error exit
				break;
			}
		}
		/* Error check */

		/* Write blank frames */
		if (switch_newfile && !first_time && !o->adts_out && error_handling == error_ok) {
			if (adjust_pts && adjust_PTS(aufile, &b, pts_diff, decode_samplerate, decode_channels)) {
				adjust_pts = 0;
				pts_diff = 0.0;
				skip_frames = 0;
			}
			adjust_pts = 0;
			if (skip_frames) {
				faad_fprintf(stderr, "Frame %d : %d frames were inserted instead of error frames. \n", b.frames, skip_frames);
				if (write_blank_audio_file(aufile, skip_frames*1024*decode_channels) != skip_frames*1024*decode_channels) {
					//break;
					goto exit_loop;
				}
				skip_frames = 0;
			}
		}
		/* Write blank frames */

		/* Prepare new file */
		if (switch_newfile) {
			if (o->adts_out) {
				assert(adtsFile);
				fclose(adtsFile);
				make_new_filename(adtsnew_fn, o->adtsFileName, ++file_number);
				adtsFile = fopen(adtsnew_fn, "wb");

				if (adtsFile != NULL) {
					faad_fprintf(stderr, "\nFrom frame %d : %s\n", b.frames, adtsnew_fn);
				} else {
					faad_fprintf(stderr, "Error opening file: %s\n", adtsnew_fn);
					//Error exit
					break;
				}
			} else {
				assert(aufile);
				last_pts = aufile->base_PTS+length_of_audio_file(aufile);
				close_audio_file(aufile);
				aufile = NULL;
				make_new_filename(sndnewfile, o->audioFileName, ++file_number);

				faad_fprintf(stderr, "\nFrom frame %d : %s\n", b.frames, sndnewfile);
			}
			first_time = 1;
			switch_newfile = 0;
		}
		/* Prepare new file */

		/* Prepare wave header */
		if (
			first_time
			&& (error_handling == error_ok
				|| (error_handling == error_skip && (o->flags & FAADC_ERROR_OUT)
					&& ((o->flags & FAADC_ERROR_SPR) || o->adts_out) )
				|| (error_handling == error_seek && (o->flags & FAADC_BROKEN_OUT)
					&& ((o->flags & FAADC_BROKEN_SPR) || o->adts_out) )
			)
		) {
			if (frameInfo.error != 0) {
				if (decode_channels == 0) {
					faad_fprintf(stderr, "Number of channels is invalid. use 2. \n");
					frameInfo.channels = decode_channels = 2;
				}
				if (decode_samplerate == 0) {
					faad_fprintf(stderr, "Samplerate is invalid. use 48000. \n");
					frameInfo.samplerate = decode_samplerate = 48000;
				}
				frameInfo.channels = decode_channels;
				frameInfo.samplerate = decode_samplerate;
			} else {
				decode_channels = frameInfo.channels;
				decode_samplerate = frameInfo.samplerate;
			}
            /* print some channel info */
			faad_fprintf(stderr, "   %d Hz  %d ch \n", decode_samplerate, decode_channels);
			if (header_type == 1) faad_fprintf(stderr, "   Frame Header : %dch\n", adts_header_ch);
            print_channel_info(&frameInfo);

            if (!o->adts_out)
            {
                /* open output file */
                if (!o->writeToStdio)
                {
                    aufile = open_audio_file(sndnewfile, frameInfo.samplerate, frameInfo.channels,
                        o->config.outputFormat, o->fileType, aacChannelConfig2wavexChannelMask(&frameInfo));
                } else {
                    aufile = open_audio_file("-", frameInfo.samplerate, frameInfo.channels,
                        o->config.outputFormat, o->fileType, aacChannelConfig2wavexChannelMask(&frameInfo));
                }
				if (aufile == NULL) {
					//Error exit
					if (!o->writeToStdio) faad_fprintf(stderr, "Error opening file: %s\n", sndnewfile);
					break;
				}
				if (!delay_samples) {
					aufile->base_PTS = b.PTS[0]? b.PTS[0]: last_pts;
				}
            } else {
                faad_fprintf(stderr, "Writing output MPEG-4 AAC ADTS file.\n\n");
            }
			first_time = 0;
        }
		/* Prepare wave header */


		/* Show progress */
        percent = min((int)(b.file_offset/(double)b.file_size*100.0), 100);
        if (percent > old_percent)
        {
            old_percent = percent;
            sprintf(percents, "%d%% (%d) decoding %s.", percent, b.frames, o->aacFileName);
            faad_fprintf(stderr, "%s\r", percents);
#ifdef _WIN32
            if (!(o->flags & FAADC_NO_TITLE)) SetConsoleTitle(percents);
#endif
        }
		/* Show progress */

		/* Write output */
		if (error_handling == error_ok
			|| (frame_length && o->adts_out && (o->flags & FAADC_ERROR_OUT))
		) {
			assert(!(switch_newfile && first_time));

			if (o->adts_out == 1) {
				int skip = (o->config.useOldADTSFormat) ? 8 : 7;
				unsigned char *adtsData = MakeAdtsHeader(&adtsDataSize, &frameInfo, o->config.useOldADTSFormat);

				/* write the adts header */
				if (header_type != 1 && frame_length)
					fwrite(adtsData, 1, adtsDataSize, adtsFile);

				/* write the frame data */
				fwrite(b.buffer, 1, min(frame_length, b.bytes_into_buffer), adtsFile);

				free(adtsData);
			} else {
				/* PCM out */
				int spf = 1024;
				long write_samples = frameInfo.samples;

				if (frameInfo.object_type == LD) spf =  512;
				if (frameInfo.object_type == HE_AAC) spf *= 2;

				assert(frameInfo.samples == 0 || frameInfo.samples == spf*frameInfo.channels);
				if (frameInfo.samples == 0) {
					write_samples = spf * decode_channels;
					if (o->flags & FAADC_FORCE_READ) {
						//sample_buffer = sample_buffer;
					} else {
						sample_buffer = NULL;
					}
				}

				if (!aufile->base_PTS && b.PTS[0]) {
					/* Set base PTS */
					aufile->base_PTS = b.PTS[0] - length_of_audio_file(aufile);
					shift_PTS_of_audio_file(aufile, -delay_samples - skip_frames*1024);
				}
				
				if (aufile->base_PTS && b.PTS[0]) {
					pts_diff = b.PTS[0] - (aufile->base_PTS+length_of_audio_file(aufile));
					if (b.PTS[0] < aufile->base_PTS) {
						/* PTS wrap around */
						pts_diff += PTS_MAX_TIME;
						//faad_fprintf(stderr, "Frame %d : PTS wrap around. \n", b.frames);
					}
#ifndef NDEBUG
					if (fabs(pts_diff) > 0.1)
						fprintf(stderr, "PTS diff(%d): %lf, %lf, %lf, %lf\n", b.frames, pts_diff, 
							b.PTS[0], aufile->base_PTS, length_of_audio_file(aufile));
#endif
				}
				if (delay_samples && skip_frames) {
					/* skip_frames are calculated as 1024 samples per frame */
					faad_fprintf(stderr, "Frame %d : %d frames will be inserted with delay correction. \n", b.frames, skip_frames);
					delay_samples += skip_frames*1024;
					skip_frames = 0;
				}
				if (adjust_pts && adjust_PTS(aufile, &b, pts_diff, decode_samplerate, decode_channels)) {
					adjust_pts = 0;
					pts_diff = 0.0;
					skip_frames = 0;
				}
				if (skip_frames) {
					faad_fprintf(stderr, "Frame %d : %d frames were inserted. \n", b.frames, skip_frames);
					if (write_blank_audio_file(aufile, skip_frames*spf*decode_channels) != skip_frames*spf*decode_channels) {
						//break;
						goto exit_loop;
					}
					skip_frames = 0;
				}

				if (delay_samples > 0) {
					faad_fprintf(stderr, "Frame %d : %d samples were inserted. (delay correction) \n", b.frames, delay_samples);
					if (write_blank_audio_file(aufile, decode_channels*delay_samples) != decode_channels*delay_samples) {
						//break;
						goto exit_loop;
					}
					delay_samples = 0;

				} else if(delay_samples < 0) {
					if (spf < -delay_samples) {
						//faad_fprintf(stderr, "Frame %d : removed. (delay correction) \n", b.frames);
						write_samples = 0;
						delay_samples += spf;
					} else {
						faad_fprintf(stderr, "Frame %d : %d frames and %d samples were removed. (delay correction) \n",
							b.frames, b.frames-o->first_frame, -delay_samples);
						write_samples -= -delay_samples*decode_channels;
						if (sample_buffer) (char*)sample_buffer += -delay_samples*decode_channels * aufile->bits_per_sample/8;
						delay_samples = 0;
					}
				}

				if (write_samples/decode_channels > 0) {
					if (write_audio_file(aufile, sample_buffer, write_samples, 0) == 0) {
						break;
					}
				}
			}
		}
		/* Write output */

		/* Advance frame */
		switch (error_handling) {
			case error_ok:
				advance_buffer(&b, frame_length);
				fill_buffer(&b);
				b.frames++;
				break;
			case error_seek:
			if (frame_length == 0) {
				/* Frame header is broken */
				size_t skip;

				faad_fprintf(stderr, "Frame %d : Error Frame header is broken. Try to find next header...\n", b.frames);

				skip = find_next_syncword(&b, (o->flags & FAADC_BROKEN_OUT)? adtsFile : NULL);

				if (skip) {
					int broken_frames;
					if (bitrate == 0) {
						faad_fprintf(stderr, "Bitrate is invalid. use 192. \n");
						bitrate = 192;
					}
					assert(decode_samplerate);
					if (decode_samplerate == 0) {
						if (o->config.defSampleRate) {
							samplerate = o->config.defSampleRate;
						} else {
							faad_fprintf(stderr, "Samplerate is invalid. use 48000. \n");
							samplerate = 48000;
						}
					}
					broken_frames = (int)ceil((double)skip / (bitrate * 1000.0 / 8.0 * 1024.0 / samplerate));
					faad_fprintf(stderr, "Syncword found. %d bytes skipped. (Corresponding to %d frames?)\n", skip, broken_frames);
					if (o->flags & FAADC_BROKEN_OUT) skip_frames += broken_frames;
					b.frames++;
				} else {
					/* Not found */
					faad_fprintf(stderr, "EOF found.\n");
					assert(b.bytes_into_buffer == 0);
				}
				//b.frames++;
				break;
			}
			/* FALLTHROUGH */
			case error_skip:
			{
				if (frame_length <= b.bytes_into_buffer) {
					advance_buffer(&b, frame_length);
					fill_buffer(&b);
					if (o->flags & FAADC_ERROR_OUT) skip_frames++;
					b.frames++;
				} else {
					/* EOF */
					assert(b.at_eof);

					advance_buffer(&b, b.bytes_into_buffer);
					fill_buffer(&b);
					//skip_frames++;
					frameInfo.error = 0;
				}
				//b.frames++;
				break;
			}
			case error_retry:
				assert(0);
				break;
		} 
		//b.frames++;
		if (!first_time
			&& ((error_handling == error_skip && (o->flags & FAADC_ERROR_SPR))
				|| (error_handling == error_seek && (o->flags & FAADC_BROKEN_SPR)))
		) {
			switch_newfile = 1;
		}
		previous_adts_header_ch = adts_header_ch;
		previous_adts_header_sr = adts_header_sr;
		/* Advancd frame */
    } while (b.bytes_into_buffer);
exit_loop:

    NeAACDecClose(hDecoder);

    if (o->adts_out == 1 && adtsFile)
    {
        fclose(adtsFile);
    }


    if (!first_time && !o->adts_out)
        close_audio_file(aufile);

	if (!length) {
		length = (float)(b.frames * 1024.0 / decode_samplerate);
		*song_length = length;
	}

	del_buffer(&b);

    return frameInfo.error;
}

static int GetAACTrack(mp4ff_t *infile)
{
    /* find AAC track */
    int i, rc;
    int numTracks = mp4ff_total_tracks(infile);

    for (i = 0; i < numTracks; i++)
    {
        unsigned char *buff = NULL;
        int buff_size = 0;
        mp4AudioSpecificConfig mp4ASC;

        mp4ff_get_decoder_config(infile, i, &buff, &buff_size);

        if (buff)
        {
            rc = NeAACDecAudioSpecificConfig(buff, buff_size, &mp4ASC);
            free(buff);

            if (rc < 0)
                continue;
            return i;
        }
    }

    /* can't decode this */
    return -1;
}

static const unsigned long srates[] =
{
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000,
    12000, 11025, 8000
};

static int decodeMP4file(char *mp4file, char *sndfile, char *adts_fn, int to_stdout,
                  int outputFormat, int fileType, int downMatrix, int noGapless,
                  int infoOnly, int adts_out, float *song_length, faad_option *o)
{
    int track;
    unsigned long samplerate;
    unsigned char channels;
    void *sample_buffer;

    mp4ff_t *infile;
    long sampleId, numSamples;

    audio_file *aufile;

    FILE *mp4File;
    FILE *adtsFile;
    //unsigned char *adtsData;
    int adtsDataSize;

    NeAACDecHandle hDecoder;
    NeAACDecConfigurationPtr config;
    NeAACDecFrameInfo frameInfo;
    mp4AudioSpecificConfig mp4ASC;

    unsigned char *buffer;
    int buffer_size;

    char percents[200];
    int percent, old_percent = -1;

    int first_time = 1;

    /* for gapless decoding */
    unsigned int useAacLength = 1;
    unsigned int initial = 1;
    unsigned int framesize;
    unsigned long timescale;


    /* initialise the callback structure */
    mp4ff_callback_t *mp4cb = malloc(sizeof(mp4ff_callback_t));

    mp4File = fopen(mp4file, "rb");
    mp4cb->read = read_callback;
    mp4cb->seek = seek_callback;
    mp4cb->user_data = mp4File;


    hDecoder = NeAACDecOpen();

    /* Set configuration */
    config = NeAACDecGetCurrentConfiguration(hDecoder);
    config->outputFormat = outputFormat;
    config->downMatrix = downMatrix;
    //config->dontUpSampleImplicitSBR = 1;
    NeAACDecSetConfiguration(hDecoder, config);

    if (adts_out)
    {
        adtsFile = fopen(adts_fn, "wb");
        if (adtsFile == NULL)
        {
            faad_fprintf(stderr, "Error opening file: %s\n", adts_fn);
            return 1;
        }
    }

    infile = mp4ff_open_read(mp4cb);
    if (!infile)
    {
        /* unable to open file */
        faad_fprintf(stderr, "Error opening file: %s\n", mp4file);
        return 1;
    }

    if ((track = GetAACTrack(infile)) < 0)
    {
        faad_fprintf(stderr, "Unable to find correct AAC sound track in the MP4 file.\n");
        NeAACDecClose(hDecoder);
        mp4ff_close(infile);
        free(mp4cb);
        fclose(mp4File);
        return 1;
    }

    buffer = NULL;
    buffer_size = 0;
    mp4ff_get_decoder_config(infile, track, &buffer, &buffer_size);

    if(NeAACDecInit2(hDecoder, buffer, buffer_size,
                    &samplerate, &channels) < 0)
    {
        /* If some error initializing occured, skip the file */
        faad_fprintf(stderr, "Error initializing decoder library.\n");
        NeAACDecClose(hDecoder);
        mp4ff_close(infile);
        free(mp4cb);
        fclose(mp4File);
        return 1;
    }

    timescale = mp4ff_time_scale(infile, track);
    framesize = 1024;
    useAacLength = 0;

    if (buffer)
    {
        if (NeAACDecAudioSpecificConfig(buffer, buffer_size, &mp4ASC) >= 0)
        {
            if (mp4ASC.frameLengthFlag == 1) framesize = 960;
            if (mp4ASC.sbr_present_flag == 1) framesize *= 2;
        }
        free(buffer);
    }

    /* print some mp4 file info */
    faad_fprintf(stderr, "%s file info:\n\n", mp4file);
    {
        char *tag = NULL, *item = NULL;
        int k, j;
        char *ot[6] = { "NULL", "MAIN AAC", "LC AAC", "SSR AAC", "LTP AAC", "HE AAC" };
        long samples = mp4ff_num_samples(infile, track);
        float f = 1024.0;
        float seconds;
        if (mp4ASC.sbr_present_flag == 1)
        {
            f = f * 2.0f;
        }
        seconds = (float)samples*(float)(f-1.0)/(float)mp4ASC.samplingFrequency;

        *song_length = seconds;

        faad_fprintf(stderr, "%s\t%.3f secs, %d ch, %d Hz\n\n", ot[(mp4ASC.objectTypeIndex > 5)?0:mp4ASC.objectTypeIndex],
            seconds, mp4ASC.channelsConfiguration, mp4ASC.samplingFrequency);

#define PRINT_MP4_METADATA
#ifdef PRINT_MP4_METADATA
        j = mp4ff_meta_get_num_items(infile);
        for (k = 0; k < j; k++)
        {
            if (mp4ff_meta_get_by_index(infile, k, &item, &tag))
            {
                if (item != NULL && tag != NULL)
                {
                    faad_fprintf(stderr, "%s: %s\n", item, tag);
                    free(item); item = NULL;
                    free(tag); tag = NULL;
                }
            }
        }
        if (j > 0) faad_fprintf(stderr, "\n");
#endif
    }

    if (infoOnly)
    {
        NeAACDecClose(hDecoder);
        mp4ff_close(infile);
        free(mp4cb);
        fclose(mp4File);
        return 0;
    }

    numSamples = mp4ff_num_samples(infile, track);

    for (sampleId = 0; sampleId < numSamples; sampleId++)
    {
        int rc;
        long dur;
        unsigned int sample_count;
        unsigned int delay = 0;

        /* get acces unit from MP4 file */
        buffer = NULL;
        buffer_size = 0;

        dur = mp4ff_get_sample_duration(infile, track, sampleId);
        rc = mp4ff_read_sample(infile, track, sampleId, &buffer,  &buffer_size);
        if (rc == 0)
        {
            faad_fprintf(stderr, "Reading from MP4 file failed.\n");
            NeAACDecClose(hDecoder);
            mp4ff_close(infile);
            free(mp4cb);
            fclose(mp4File);
            return 1;
        }

        sample_buffer = NeAACDecDecode(hDecoder, &frameInfo, buffer, buffer_size);

        if (adts_out == 1)
        {
			unsigned char *adtsData = MakeAdtsHeader(&adtsDataSize, &frameInfo, 0);

            /* write the adts header */
            fwrite(adtsData, 1, adtsDataSize, adtsFile);

            fwrite(buffer, 1, frameInfo.bytesconsumed, adtsFile);
			free(adtsData);
        }

        if (buffer) free(buffer);

        if (!noGapless)
        {
            if (sampleId == 0) dur = 0;

            if (useAacLength || (timescale != samplerate)) {
                sample_count = frameInfo.samples;
            } else {
                sample_count = (unsigned int)(dur * frameInfo.channels);
                if (sample_count > frameInfo.samples)
                    sample_count = frameInfo.samples;

                if (!useAacLength && !initial && (sampleId < numSamples/2) && (sample_count != frameInfo.samples))
                {
                    faad_fprintf(stderr, "MP4 seems to have incorrect frame duration, using values from AAC data.\n");
                    useAacLength = 1;
                    sample_count = frameInfo.samples;
                }
            }

            if (initial && (sample_count < framesize*frameInfo.channels) && (frameInfo.samples > sample_count))
                delay = frameInfo.samples - sample_count;
        } else {
            sample_count = frameInfo.samples;
        }

        /* open the sound file now that the number of channels are known */
        if (first_time && !frameInfo.error)
        {
            /* print some channel info */
            print_channel_info(&frameInfo);

            if (!adts_out)
            {
                /* open output file */
                if(!to_stdout)
                {
                    aufile = open_audio_file(sndfile, frameInfo.samplerate, frameInfo.channels,
                        outputFormat, fileType, aacChannelConfig2wavexChannelMask(&frameInfo));
                } else {
#ifdef _WIN32
                    setmode(fileno(stdout), O_BINARY);
#endif
                    aufile = open_audio_file("-", frameInfo.samplerate, frameInfo.channels,
                        outputFormat, fileType, aacChannelConfig2wavexChannelMask(&frameInfo));
                }
                if (aufile == NULL)
                {
                    NeAACDecClose(hDecoder);
                    mp4ff_close(infile);
                    free(mp4cb);
                    fclose(mp4File);
                    return 0;
                }
            }
            first_time = 0;
        }

        if (sample_count > 0) initial = 0;

        percent = min((int)(sampleId*100)/numSamples, 100);
        if (percent > old_percent)
        {
            old_percent = percent;
            sprintf(percents, "%d%% decoding %s.", percent, mp4file);
            faad_fprintf(stderr, "%s\r", percents);
#ifdef _WIN32
			if (!(o->flags & FAADC_NO_TITLE)) SetConsoleTitle(percents);
#endif
        }

        if ((frameInfo.error == 0) && (sample_count > 0) && (!adts_out))
        {
            if (write_audio_file(aufile, sample_buffer, sample_count, delay) == 0){
                break;
			}
        }

        if (frameInfo.error > 0)
        {
            faad_fprintf(stderr, "Warning: %s\n",
                NeAACDecGetErrorMessage(frameInfo.error));
        }
    }

    NeAACDecClose(hDecoder);

    if (adts_out == 1)
    {
        fclose(adtsFile);
    }

    mp4ff_close(infile);

    if (!first_time && !adts_out)
        close_audio_file(aufile);

    free(mp4cb);
    fclose(mp4File);

    return frameInfo.error;
}

#ifndef NO_MAIN
int main(int argc, char *argv[])
{
    int result;
	faad_option o;
	enum {unknown_file = 0, aac_file, mp4_file, ts_file} file_format = unknown_file;
    char *fnp;
    unsigned char header[8];
    float length = 0;
    FILE *hMP4File;

	ts_buffer tsb;

/* System dependant types */
#ifdef _WIN32
    long begin;
#else
    clock_t begin;
#endif

	o.config.defObjectType = LC;
	o.config.defSampleRate = 0;
	o.config.outputFormat = FAAD_FMT_16BIT;
	o.config.downMatrix = 0;
	o.config.useOldADTSFormat = 0;
	o.infoOnly = 0;
	o.writeToStdio = 0;
	o.fileType = 1;
	o.outfile_set = 0;
	o.adts_out = 0;
	o.showHelp = 0;
	o.noGapless = 0;
	o.delay = 0;
	o.delay_set = 0;
	o.flags = FAADC_DEFAULT_FLAGS;
	o.first_frame = 1;
	o.last_frame = 0;
	
	o.program_number = 0;
	o.stream_index = 0;
	o.PID = 0;
	o.PTS = 0.0;
	o.video_PID = 0;
	o.video_PTS_mode = VIDEO_PTS_FIRST_GOP;
	o.video_PTS = 0.0;
	o.ts_buffer = NULL;


    //unsigned long cap = NeAACDecGetCapabilities();


    /* begin process command line */
    progName = argv[0];
    while (1) {
        int c = -1;
        int option_index = 0;
        static struct option long_options[] = {
            { "quiet",      0, 0, 'q' },
            { "outfile",    0, 0, 'o' },
            { "adtsout",    0, 0, 'a' },
            { "oldformat",  0, 0, 't' },
            { "format",     0, 0, 'f' },
            { "bits",       0, 0, 'b' },
            { "samplerate", 0, 0, 's' },
            { "objecttype", 0, 0, 'l' },
            { "downmix",    0, 0, 'd' },
            { "info",       0, 0, 'i' },
            { "stdio",      0, 0, 'w' },
            { "stdio",      0, 0, 'g' },
            { "help",       0, 0, 'h' },
			{ "delay",      required_argument, 0, 'D' },
			{ "extract",    required_argument, 0, 'E' },
			{ "flags",      required_argument, 0, 'F' },

			{ "program",    required_argument, 0, 'R' },
			{ "stream",     required_argument, 0, 'I' },
			{ "pid",        required_argument, 0, 'P' },
			{ "video-pid",  required_argument, 0, 'B' },
			{ "pts-base",   required_argument, 0, 'M' },

			{ "split",      0, 0, 'S' },
            { 0, 0, 0, 0 }
        };

		c = getopt_long(argc, argv, "o:a:s:f:b:l:D:E:F:P:wgdhitqS",
            long_options, &option_index);

        if (c == -1)
            break;

        switch (c) {
        case 'o':
            if (optarg)
            {
                o.outfile_set = 1;
				sprintf(o.audioFileName, "%.255s", optarg);
            }
            break;
        case 'a':
            if (optarg)
            {
                o.adts_out = 1;
				sprintf(o.adtsFileName, "%.255s", optarg);
            }
            break;
        case 's':
            if (optarg)
            {
                char dr[10];
                if (sscanf(optarg, "%9s", dr) < 1) {
                    o.config.defSampleRate = 0;
                } else {
                    o.config.defSampleRate = atoi(dr);
                }
            }
            break;
        case 'f':
            if (optarg)
            {
                char dr[10];
                if (sscanf(optarg, "%9s", dr) < 1)
                {
                    o.fileType = 1;
                } else {
                    o.fileType = atoi(dr);
                    if ((o.fileType < 1) || (o.fileType > 2))
                        o.showHelp = 1;
                }
            }
            break;
        case 'b':
            if (optarg)
            {
                char dr[10];
                if (sscanf(optarg, "%9s", dr) < 1)
                {
                    o.config.outputFormat = FAAD_FMT_16BIT; /* just use default */
                } else {
                    o.config.outputFormat = atoi(dr);
                    if ((o.config.outputFormat < 1) || (o.config.outputFormat > 5))
                        o.showHelp = 1;
                }
            }
            break;
        case 'l':
            if (optarg)
            {
                char dr[10];
                if (sscanf(optarg, "%9s", dr) < 1)
                {
                    o.config.defObjectType = LC; /* default */
                } else {
                    o.config.defObjectType = atoi(dr);
                    if ((o.config.defObjectType != LC) &&
                        (o.config.defObjectType != MAIN) &&
                        (o.config.defObjectType != LTP) &&
                        (o.config.defObjectType != LD))
                    {
                        o.showHelp = 1;
                    }
                }
            }
            break;
		case 'D':
			if(optarg)
			{
				if(sscanf(optarg, "%d", &o.delay) < 1){
					o.delay = 0;
				}else{
					o.delay_set = 1;
				}
			}
			break;
		case 'E':
			if (optarg)
			{
				sscanf(optarg, "%ld:%ld", &o.first_frame, &o.last_frame);
			}
			break;
		case 'F':
			if (optarg)
			{
				if (sscanf(optarg, "%li", &o.flags) < 1) {
					o.flags = FAADC_DEFAULT_FLAGS;
				}
			}
			break;
		case 'R':
			if (optarg)
			{
				if (sscanf(optarg, "%i", &o.program_number) < 1){
					o.program_number = 0;
				}
			}
			break;
		case 'I':
			if (optarg)
			{
				if (sscanf(optarg, "%d", &o.stream_index) < 1){
					o.stream_index = 0;
				}
			}
			break;
		case 'P':
			if (optarg)
			{
				if (sscanf(optarg, "%hi", &o.PID) < 1){
					o.PID = 0;
				}
			}
			break;
		case 'B':
			if (optarg)
			{
				if (sscanf(optarg, "%hi", &o.video_PID) < 1){
					o.video_PID = 0;
				}
			}
			break;
		case 'M':
			if (optarg)
			{
				if (sscanf(optarg, "%i", &o.video_PTS_mode) < 1){
					o.video_PTS_mode = 1;
				}
				if (o.video_PTS_mode < 0 || 6 < o.video_PTS_mode) {
					o.video_PTS_mode = 1;
				}
			}
			break;
        case 't':
            o.config.useOldADTSFormat = 1;
            break;
        case 'd':
            o.config.downMatrix = 1;
            break;
        case 'w':
            o.writeToStdio = 1;
            break;
        case 'g':
            o.noGapless = 1;
            break;
        case 'i':
            o.infoOnly = 1;
            break;
        case 'h':
            o.showHelp = 1;
            break;
        case 'q':
            quiet = 1;
            break;
		case 'S':
			o.flags |= 0xBF;
			break;
        default:
            break;
        }
    }


    faad_fprintf(stderr, " *********** Ahead Software MPEG-4 AAC Decoder V%s Customized 0.7 *********\n\n", FAAD2_VERSION);
    faad_fprintf(stderr, " Build: %s\n", __DATE__);
	faad_fprintf(stderr, " Original: Ahead Software MPEG-4 AAC Decoder V%s\n", FAAD2_VERSION);
    faad_fprintf(stderr, "           Copyright 2002-2004: Ahead Software AG\n");
    faad_fprintf(stderr, "           http://www.audiocoding.com\n");
    //if (cap & FIXED_POINT_CAP)
    //    faad_fprintf(stderr, " Fixed point version\n");
    //else
    //    faad_fprintf(stderr, " Floating point version\n");
    faad_fprintf(stderr, "\n");
    faad_fprintf(stderr, " This program is free software; you can redistribute it and/or modify\n");
    faad_fprintf(stderr, " it under the terms of the GNU General Public License.\n");
    faad_fprintf(stderr, "\n");
    faad_fprintf(stderr, " **************************************************************************\n\n");


    /* check that we have at least two non-option arguments */
    /* Print help if requested */
    if (((argc - optind) < 1) || o.showHelp)
    {
        usage();
        return 1;
    }

#if 0
    /* only allow raw data on stdio */
    if (writeToStdio == 1)
    {
        o.fileType = 2;
    }
#endif

	sprintf(o.aacFileName, "%.255s", argv[optind]);
    /* check for mp4 file */
    //mp4file = 0;
    hMP4File = fopen(o.aacFileName, "rb");
    if (!hMP4File)
    {
        faad_fprintf(stderr, "Error opening file: %s\n", o.aacFileName);
        return 1;
    }
    fread(header, 1, 8, hMP4File);
    fclose(hMP4File);
    if (header[4] == 'f' && header[5] == 't' && header[6] == 'y' && header[7] == 'p')
        file_format = mp4_file;

	/* check for TS file */
	if (file_format == unknown_file && tsb_construct(&tsb) && tsb_open(&tsb, o.aacFileName)) {
    	file_format = ts_file;
		
		if (!tsb_get_PAT(&tsb)) {
			faad_fprintf(stderr, "No PAT found: %s\n", o.aacFileName);
		}
		tsb_last_error_message(&tsb);

		if (!o.PID) {
			unsigned long program_pid = tsb_get_stream_PID(&tsb, 0x0F, o.program_number, o.stream_index);
			o.PID = (unsigned short)program_pid;
			if (o.program_number == 0) o.program_number = GET_STREAM_PROGRAM(program_pid);
			if (!o.PID) {
				faad_fprintf(stderr, "No AAC PID found (%s): %s\n", tsb_last_error_message(&tsb), o.aacFileName); 
				tsb_destruct(&tsb);
				if (tsb.pat) {
					faad_fprintf(stderr, "Check Program Association Table \n");
					print_PAT(faad_fprintf, stderr, tsb.pat, 0);
					faad_fprintf(stderr, "\n");
				}
				return 1;
			}
		}
		if (!o.video_PID) {
			o.video_PID = (unsigned short)tsb_get_stream_PID(&tsb, 0x02, o.program_number, o.stream_index);
			if (!o.video_PID) {
				faad_fprintf(stderr, "No video PID found (%s): %s\n", tsb_last_error_message(&tsb), o.aacFileName);
			}
		}

		tsb_seek_head(&tsb);
		o.PTS = tsb_get_video_PTS(&tsb, o.PID, VIDEO_PTS_FIRST_PES);
		tsb_last_error_message(&tsb);
		if (o.video_PID) {
			tsb_seek_head(&tsb);
			o.video_PTS = tsb_get_video_PTS(&tsb, o.video_PID, o.video_PTS_mode);
			tsb_last_error_message(&tsb);
		}
		if (o.PTS && o.video_PTS && !o.delay_set) {
			double delay = o.PTS-o.video_PTS;
			if (fabs(delay) < 100000) {
				o.delay = (int)delay;
			} else if (fabs(delay - PTS_MAX_TIME) < 100000) {
				/* Wrap around */
				delay += (delay < 0)? PTS_MAX_TIME:-PTS_MAX_TIME;
				o.delay = (int)(delay);
			}
			o.delay_set = 1;
		}
		if (!tsb_seek_head(&tsb)) {
			tsb_last_error_message(&tsb);
			faad_fprintf(stderr, "Error seeking file: %s\n", o.aacFileName);
			return 1;
		}
		o.ts_buffer = &tsb;
	}

    /* point to the specified file name */
	if (!o.delay_set) {
		char tmp_buf[256];
		char *tmp_str = o.aacFileName;
		char *delay_str = NULL;
		while (tmp_str = strstr(tmp_str, "DELAY")) {
			delay_str = tmp_str;
			tmp_str += 5;
		}
		if (delay_str && 2 == sscanf(delay_str, "DELAY %dms%s", &o.delay, tmp_buf)) {
			o.delay_set = 1;
		} else {
			o.delay = 0;
		}
		if (o.delay_set && !o.outfile_set) {
			char *ext;
			strcpy(o.audioFileName, o.aacFileName);
			o.audioFileName[delay_str-o.aacFileName] = '\0';
			strcat(o.audioFileName, "DELAY 0ms");
			strcat(o.audioFileName, tmp_buf);
			ext = strrchr(o.audioFileName, '.');
			if (ext) *ext = '\0';
			strcat(o.audioFileName, file_ext[o.fileType]);
			o.outfile_set = 1;
		}
	}
	//if (o.delay) faad_fprintf(stderr, "Delay %dms\n", o.delay);

#ifdef _WIN32
    begin = GetTickCount();
#else
    begin = clock();
#endif

    /* Only calculate the path and open the file for writing if
       we are not writing to stdout.
     */
    if (!o.writeToStdio && !o.outfile_set)
    {
        strcpy(o.audioFileName, o.aacFileName);
        fnp = (char *)strrchr(o.audioFileName,'.');

        if (fnp)
            fnp[0] = '\0';
		
        strcat(o.audioFileName, file_ext[o.fileType]);

		if (ts_file == file_format) {

			fnp = (char *)strrchr(o.aacFileName,'.');
			if (fnp) fnp[0] = '\0';
			sprintf(o.audioFileName, "%.233s PID %3X DELAY 0ms%s",
					o.aacFileName, o.PID, file_ext[o.fileType]);
			if (fnp) fnp[0] = '.';
			if (o.adts_out) {
				// Add delay to o.adtsFileName
			}
		}
    }

    if (mp4_file == file_format)
    {
        result = decodeMP4file(o.aacFileName, o.audioFileName, o.adtsFileName, o.writeToStdio,
            o.config.outputFormat, o.fileType, o.config.downMatrix, o.noGapless, o.infoOnly, o.adts_out, &length, &o);
    } else {
        result = decodeAACfile(&o, &length);
    }

    if (!result && !o.infoOnly)
    {
#ifdef _WIN32
        float dec_length = (float)((GetTickCount()-begin)/1000.0);
        if (!(o.flags & FAADC_NO_TITLE)) SetConsoleTitle("FAAD");
#else
        /* clock() grabs time since the start of the app but when we decode
           multiple files, each file has its own starttime (begin).
         */
        float dec_length = (float)(clock() - begin)/(float)CLOCKS_PER_SEC;
#endif
        faad_fprintf(stderr, "Decoding %s took: %5.2f sec. %5.2fx real-time.\n", o.aacFileName,
            dec_length, length/dec_length);
    }

    return 0;
}
#endif /*NO_MAIN*/
