#include <string.h>
#include <stdlib.h>
#include <assert.h>

#define _FILE_OFFSET_BITS 64
#include <sys/stat.h>
#include <fcntl.h>

#ifdef _WIN32
#include <io.h>
#define stat _stat64
#define fstat _fstat64
#ifndef off_t
#define off_t __int64
#endif
#endif

#include "mpeg_header.h"

#define TS_BUFFER_SIZE (192 * 17 * 13 * 47)
#ifdef _MSC_VER
#define INLINE __inline
#elif defined(__GNUC__) || defined( __cplusplus)
#define INLINE inline
#else
#define INLINE
#endif
#ifndef __cplusplus
typedef int bool;
#endif

typedef struct PAT_SECTION PAT_SECTION;
typedef struct ts_buffer{
	FILE *file;
	unsigned char *buffer;
	unsigned char *packet;
	unsigned char *eob;
	const char *error_message;
	PAT_SECTION *pat;
	off_t file_pos;
	off_t file_size;
	size_t packet_size;
	unsigned short PID;
	bool pat_ready;
} ts_buffer;

INLINE bool tsb_construct(ts_buffer *tsb){
	memset(tsb, 0, sizeof(ts_buffer));
	tsb->buffer = tsb->packet = tsb->eob = (unsigned char *)malloc(TS_BUFFER_SIZE);
	return tsb->buffer != NULL;
}
INLINE void tsb_destruct(ts_buffer *tsb){
	if (tsb->file) fclose(tsb->file);
	free(tsb->buffer);
	tsb->buffer = tsb->packet = tsb->eob = NULL;
}
INLINE const char *tsb_last_error_message(ts_buffer *tsb){
	const char *msg = tsb->error_message;
	tsb->error_message = NULL;
	return msg;
}
INLINE bool tsb_is_eof(const ts_buffer *tsb){
	return feof(tsb->file);
}
INLINE bool tsb_is_open(const ts_buffer *tsb){
	return (bool)tsb->file;
}
INLINE void tsb_fill_buffer(ts_buffer *tsb){
	size_t read;
	size_t bufsize = tsb->eob - tsb->packet;

	if (TS_BUFFER_SIZE == bufsize) return;
	memmove(tsb->buffer, tsb->packet, bufsize);
	tsb->packet = tsb->buffer;
	tsb->eob = tsb->buffer + bufsize;
	read = fread(tsb->eob, 1, TS_BUFFER_SIZE-bufsize, tsb->file);
	tsb->eob += read;
	tsb->file_pos += read;
}
INLINE bool tsb_seek_head(ts_buffer *tsb){
	if (fseek(tsb->file, 0, SEEK_SET)) {
		tsb->error_message = "File seek error";
		return 0;
	}
	tsb->packet = tsb->eob = tsb->buffer;
	tsb->file_pos = 0;
	tsb_fill_buffer(tsb);
	return 1;
}
INLINE bool tsb_find_sync(ts_buffer *tsb){
	const unsigned char *sync;

	tsb_fill_buffer(tsb);
	sync = (unsigned char *)memchr(tsb->packet, 0x47, tsb->eob-tsb->packet);
	if (!sync) {
		tsb->error_message = "sync_byte is not found";
		tsb->packet = tsb->eob;
		return 0;
	}
	tsb->packet = (unsigned char *)sync;
	return 1;
}
INLINE size_t tsb_get_packet_size(const ts_buffer *tsb){
	return tsb->packet_size;
}
INLINE size_t tsb_check_packet_size(ts_buffer *tsb){
	/*At least 2 packets required*/
	const unsigned char *sync1, *sync2 = tsb->packet;
	size_t packet_size;

	for (;;) {
		sync1 = (unsigned char *)memchr(sync2, 0x47, tsb->eob-sync2);
		/*Buffer is enough to find sync*/
		if (!sync1 || tsb->eob-sync1 < 188*2){
			tsb->error_message = "sync_byte is not found";
			return 0;
		}
		sync2 = (unsigned char *)memchr(sync1+188, 0x47, tsb->eob-(sync1+188));
		if (!sync2){
			tsb->error_message = "Second sync_byte is not found";
			return 0;
		}
		packet_size = sync2 - sync1;
		if (packet_size >= 188*2) {
			continue;
		} else {
			if (tsb->eob < sync2 + 4*packet_size) {
				return 0;
			} else if (
				sync2[packet_size] == 0x47 && sync2[packet_size*2] == 0x47
				&& sync2[packet_size*3] == 0x47 && sync2[packet_size*4] == 0x47
				) {
					return packet_size;
			}
		}
	}
	
}

INLINE unsigned char *tsb_get_packet(const ts_buffer *tsb){
	return tsb->packet;
}
INLINE unsigned char *tsb_get_next_packet(ts_buffer *tsb){
	if (tsb->eob < tsb->packet + tsb->packet_size) {
		tsb_fill_buffer(tsb);
		if (tsb->eob <= tsb->packet + tsb->packet_size) {
			tsb->error_message = "EOF";
			tsb->packet = tsb->eob;
			assert(tsb_is_eof(tsb));
			return NULL;
		}
	}

	tsb->packet += tsb->packet_size;
	if (tsb->eob < tsb->packet + tsb->packet_size) {
		tsb_fill_buffer(tsb);
		if (tsb->eob - tsb->packet == 0) {
			tsb->error_message = "EOF";
			return NULL;
		}
	}
	return tsb->packet;
}
INLINE unsigned char *tsb_get_next_start_packet(ts_buffer *tsb, int limit){
	int count = 0;
	while (tsb_get_next_packet(tsb)){
		count++;
		if (!TSH_CHECK_SYNC(tsb->packet)){
			if (!tsb_find_sync(tsb)){
				return NULL;
			}
		}
		if (TSH_IS_PAYLOAD_UNIT_START(tsb->packet)
			&& TSH_HAS_PAYLOAD(tsb->packet)) return tsb->packet;
		if (limit && count > limit){
			tsb->error_message = "Not found in the packet limit";
			return NULL;
		}
	}
	return NULL;
}
INLINE unsigned char *tsb_get_next_PID_packet(ts_buffer *tsb, int limit){
	int count = 0;
	while (tsb_get_next_packet(tsb)){
		count++;
		if (!TSH_CHECK_SYNC(tsb->packet)){
			if (!tsb_find_sync(tsb)){
				return NULL;
			}
		}
		if (TSH_PID(tsb->packet) == tsb->PID) {
			return tsb->packet;
		}
		if (limit && count > limit){
			tsb->error_message = "Not found in the packet limit";
			return NULL;
		}
	}
	return NULL;	
}
INLINE unsigned char *tsb_get_next_PID_start_packet(ts_buffer *tsb, int limit){
	while (tsb_get_next_PID_packet(tsb, limit)){
		if (TSH_IS_PAYLOAD_UNIT_START(tsb->packet)
			&& TSH_HAS_PAYLOAD(tsb->packet)) return tsb->packet;
	}
	return NULL;
}

INLINE __int64 tsb_get_file_pos(const ts_buffer *tsb){
	return tsb->file_pos;
}
INLINE void tsb_set_PID(ts_buffer *tsb, unsigned short PID){
	tsb->PID = PID;
}
INLINE bool tsb_open(ts_buffer *tsb, const char *filename){
	struct stat st;
	int fd;
	fd = open(filename, O_RDONLY);
	if (fstat(fd, &st)) {
		tsb->error_message = "File stat error";
		return 0;
	}
	close(fd);
	tsb->file_size = st.st_size;

	tsb->file = fopen(filename, "rb");
	if (!tsb->file) {
		tsb->error_message = "File open error";
		return 0;
	}
	tsb_fill_buffer(tsb);
	tsb->packet_size = tsb_check_packet_size(tsb);
	if (!tsb->packet_size) return 0;
	return 1;
}

INLINE size_t  tsb_read_payload_unit(
		ts_buffer *tsb, unsigned char **buffer, size_t *bufsize, size_t unit_size, size_t max_size,
		int limit
){
	size_t read_size = 0;

	if (!(TSH_PID(tsb->packet) == tsb->PID && TSH_IS_PAYLOAD_UNIT_START(tsb->packet))) {
		if (!tsb_get_next_PID_start_packet(tsb, limit)) return 0;
	}
	/*if (!(TSH_PID(tsb->packet) == tsb->PID)) {
		if (!tsb_get_next_PID_packet(tsb, limit)) return 0;
	}*/
	do {
		size_t payload_size = TSH_GET_PAYLOAD_SIZE(tsb->packet);
		while (*bufsize - read_size < payload_size) {
			size_t new_size;
			unsigned char *tmp;

			new_size = *bufsize + unit_size;
			if (!(unit_size && max_size)){
				tsb->error_message = "Full buffer";
				return read_size;
			}
			if (new_size > max_size) {
				tsb->error_message = "Buffer max_size limit";
				return read_size;
			}
			tmp = (unsigned char *)realloc(*buffer, new_size);
			if (!tmp) {
				tsb->error_message = "Memory realloc error";
				return read_size;
			}
			*buffer = tmp;
			*bufsize = new_size;
		}
		memcpy(*buffer + read_size, TSH_GET_PAYLOAD(tsb->packet), payload_size); 
		read_size += payload_size;
	} while (tsb_get_next_PID_packet(tsb, limit) && !TSH_IS_PAYLOAD_UNIT_START(tsb->packet));
	return read_size;
}

INLINE unsigned char *skip_PES_header(const unsigned char *pesp, size_t pes_size, size_t *size){
	if (pes_size && pesp) {
		size_t skip = 9 + PESH_HEADER_LENGTH(pesp);
		if (pes_size > skip) {
			if (size) *size = pes_size - skip;
			return (unsigned char*)pesp + skip;
		}
	}
	if (size) *size = 0;
	return NULL;
}

typedef struct PMT_SECTION {
	/*unsigned char  table_id;*/
	unsigned short program_number;
	unsigned char  version_number;
	char           current_next_indicator;
	unsigned char  section_number;
	unsigned char  last_section_number;
	unsigned short PCR_PID;
	/*unsigned short program_info_length*/
	short          stream_length;
	struct {
		unsigned char  stream_type;
		unsigned short PID;
		/*unsigned short ES_info_length*/
	} streams[253];
} PMT_SECTION;

typedef struct PAT_SECTION {
	/*unsigned char  table_id;*/
	unsigned short transport_stream_id;
	unsigned char  version_number;
	char           current_next_indicator;
	unsigned char  section_number;
	unsigned char  last_section_number;
	short          program_length;
	struct {
		unsigned short program_number;
		unsigned short PID;
		PMT_SECTION *PMT;
	} programs[253];
} PAT_SECTION;

INLINE unsigned char *skip_PSI_pointer_field(const unsigned char *payload, size_t payload_size, size_t *size){
	if (payload_size && payload) {
		size_t skip = payload[0] + 1;
		if (payload_size > skip) {
			if (size) *size = payload_size - skip;
			return (unsigned char *)payload + skip;
		}
	}
	if (size) *size = 0;
	return NULL;
}

INLINE PAT_SECTION *tsb_parse_PAT(ts_buffer *tsb, int limit){
	PAT_SECTION *pat;
	unsigned char *table, *buffer;
	size_t pat_size, bufsize = 1024;
	int i, k, sections;

	buffer = (unsigned char *)malloc(bufsize);
	if (!buffer) return NULL;

	tsb_set_PID(tsb, PID_PAT);
	pat_size = tsb_read_payload_unit(tsb, &buffer, &bufsize, 1024, 1024*1024, limit);
	if (tsb->error_message) {
		return NULL;
	}
	table = skip_PSI_pointer_field(buffer, pat_size, &pat_size);
	if (!table || pat_size < 16) {
		tsb->error_message = "Invalid PAT";
		return NULL;
	}

	sections = PATS_LAST_SECTION_NUMBER(table)+1;
	pat = (PAT_SECTION *)malloc(sections*sizeof(PAT_SECTION));
	if (!pat) {
		tsb->error_message = "Memory alloc error";
		return NULL;
	}
	memset(pat, 0, sections * sizeof(PAT_SECTION));
	
	for (i = 0; i < sections; i++) {
		if (!PATS_CHECK_TABLE_ID(table)) break;
		pat[i].transport_stream_id    = PATS_TS_ID(table);
		pat[i].version_number         = PATS_VERSION_NUMBER(table);
		pat[i].current_next_indicator = PATS_IS_CURRENT(table);
		pat[i].section_number         = PATS_SECTION_NUMBER(table);
		pat[i].last_section_number    = PATS_LAST_SECTION_NUMBER(table);
		pat[i].program_length         = (PATS_SECTION_LENGTH(table) - 5 - 4)/4;
		for (k = 0; k < pat[i].program_length; k++){
			pat[i].programs[k].program_number = PAT_PROGRAM_NUMBER(table+8+4*k);
			pat[i].programs[k].PID            = PAT_PID(table+8+4*k);
		}
		table += 8 + 4*pat[i].program_length + 4;
	}
	free(buffer);
	return pat;
}
INLINE PMT_SECTION *tsb_parse_PMT(ts_buffer *tsb, unsigned short PID, int limit){
	PMT_SECTION *pmt;
	unsigned char *table, *buffer;
	size_t pmt_size, bufsize = 1024;
	int i, k, sections;

	buffer = (unsigned char *)malloc(bufsize);
	if (!buffer) return NULL;

	tsb_set_PID(tsb, PID);
	pmt_size = tsb_read_payload_unit(tsb, &buffer, &bufsize, 1024, 1024*1024, limit);
	if (tsb->error_message) {
		return NULL;
	}
	table = skip_PSI_pointer_field(buffer, pmt_size, &pmt_size);
	if (!table || pmt_size < 16) {
		tsb->error_message = "Invalid PMT";
		return NULL;
	}

	sections = PMTS_LAST_SECTION_NUMBER(table)+1;
	pmt = (PMT_SECTION *)malloc(sections*sizeof(PMT_SECTION));
	if (!pmt) {
		tsb->error_message = "Memory alloc error";
		return NULL;
	}
	memset(pmt, 0, sections * sizeof(PMT_SECTION));
	
	for (i = 0; i < sections; i++) {
		size_t read_bytes, section_size;
		if (!PMTS_CHECK_TABLE_ID(table)) break;
		section_size = PMTS_SECTION_LENGTH(table) + 3;
		pmt[i].program_number         = PMTS_PROGRAM_NUMBER(table);
		pmt[i].version_number         = PMTS_VERSION_NUMBER(table);
		pmt[i].current_next_indicator = PMTS_IS_CURRENT(table);
		pmt[i].section_number         = PMTS_SECTION_NUMBER(table);
		pmt[i].last_section_number    = PMTS_LAST_SECTION_NUMBER(table);
		pmt[i].PCR_PID                = PMTS_PCR_PID(table);
		read_bytes = 12 + PMTS_PROGRAM_INFO_LENGTH(table);
		for (k = 0; read_bytes < section_size-4; k++){
			pmt[i].streams[k].stream_type = PMT_STREAM_TYPE(table+read_bytes);
			pmt[i].streams[k].PID         = PMT_EREMENTARY_PID(table+read_bytes);
			read_bytes += 5 + PMT_ES_INFO_LENGTH(table+read_bytes);
		}
		pmt[i].stream_length          = k;
		assert(read_bytes == section_size-4);
		table += section_size;
	}
	free(buffer);
	return pmt;
}
INLINE PAT_SECTION *tsb_make_PAT(ts_buffer *tsb, int limit){
	PAT_SECTION *pat;
	int i, k;

	pat = tsb_parse_PAT(tsb, limit);
	if (!pat) return NULL;
	for (i = 0; i <= pat[0].last_section_number; i++) {
		for (k = 0; k < pat[i].program_length; k++) {
			if (tsb_is_eof(tsb) && !tsb_seek_head(tsb)) return pat;
			tsb->error_message = NULL;
			pat[i].programs[k].PMT = tsb_parse_PMT(tsb, pat[i].programs[k].PID, limit);
		}
	}
	tsb->error_message = NULL;
	return pat;
}
INLINE void free_PAT(PAT_SECTION **ppat){
	PAT_SECTION *pat = *ppat;
	int i, k;
	if (!pat) return;
	for (i = 0; i <= pat[0].last_section_number; i++) {
		for (k = 0; k < pat[i].program_length; k++) {
			free(pat[i].programs[k].PMT);
			pat[i].programs[k].PMT = NULL;
		}
	}
	free(pat);
	*ppat = NULL;
}
INLINE PAT_SECTION *tsb_get_PAT(ts_buffer *tsb){
	if (tsb->pat_ready) {
		return tsb->pat;
	} else {
		tsb->pat_ready = 1;
		return tsb->pat = tsb_make_PAT(tsb, 100000);
	}
}

INLINE const char *st_text(unsigned char stream_type){
	static const char *st_table[22] = STREAM_TYPE_TABLE;
	if (stream_type > 21) stream_type = 21;
	return st_table[stream_type];
}
INLINE void print_PMT(int (*fprintf)(FILE *, const char *, ...), FILE *out, const PMT_SECTION *pmt, int mode){
	int i, k;
	if (!pmt) {
		fprintf(out, "    NULL\n");
		return;
	}
	for (i = 0; i <= pmt[0].last_section_number; i++) {
		fprintf(out, "    program_number:         %d (0x%X)\n", pmt[i].program_number, pmt[i].program_number);
		if (mode) fprintf(out, "    version_number:         %d\n", pmt[i].version_number);
		if (mode) fprintf(out, "    current_next_indicator: %s\n", pmt[i].current_next_indicator?"Yes":"No");
		if (mode) fprintf(out, "    section_number:         %d\n", pmt[i].section_number);
		if (mode) fprintf(out, "    last_section_number:    %d\n", pmt[i].last_section_number);
		fprintf(out, "    PCR_PID: 0x%X\n", pmt[i].PCR_PID);
	/*unsigned short program_info_length*/
		for (k = 0; k < pmt[i].stream_length; k++) {
			fprintf(out, "        PID: 0x%-4X stream_type: %s (0x%0.2X)\n",
				pmt[i].streams[k].PID, st_text(pmt[i].streams[k].stream_type), pmt[i].streams[k].stream_type);
		}
	}
}
INLINE void print_PAT(int (fprintf)(FILE *, const char *, ...), FILE *out, const PAT_SECTION *pat, int mode){
	int i, k;
	
	if (!pat) return;
	for (i = 0; i <= pat[0].last_section_number; i++) {
		fprintf(out, "transport_stream_id:    %d (0x%X)\n", pat[i].transport_stream_id, pat[i].transport_stream_id);
		if (mode) fprintf(out, "version_number:         %d\n", pat[i].version_number);
		if (mode) fprintf(out, "current_next_indicator: %s\n", pat[i].current_next_indicator?"Yes":"No");
		if (mode) fprintf(out, "section_number:         %d\n", pat[i].section_number);
		if (mode) fprintf(out, "last_section_number:    %d\n", pat[i].last_section_number);
		for (k = 0; k < pat[i].program_length; k++) {
			print_PMT(fprintf, out, pat[i].programs[k].PMT, mode);
		}
	}
}

#define GET_STREAM_PROGRAM(x) (unsigned short)( (unsigned long)(x)>>16 )
/* If program_number isn't specified ( == 0):
 * Upper 16 bit : program_number
 * Lower 16 bit : Stream PID
 */
INLINE unsigned long get_stream_PID(
		const PAT_SECTION *pat, ts_buffer *tsb,
		unsigned char stream_type,
		unsigned short program_number, int index
){
	int i, k, m, n;
	if (!pat) {
		/* serch TS packet */
		const unsigned char *packet, *payload;

		if (!tsb) return 0;
		if (!(stream_type == 0x0F || stream_type == 0x02)) return 0;
		/* AAC or H.262 */

		while ( (packet = tsb_get_next_start_packet(tsb, 0)) ) {
			size_t payload_size = TSH_GET_PAYLOAD_SIZE(packet);
			payload = TSH_GET_PAYLOAD(packet);
			if (payload_size > 4
				&& PESH_CHECK_SYNC(payload)
			   ){
				if (
					   (stream_type == 0x0F && (PESH_STREAM_ID(payload) & 0xE0) == 0xC0)
					|| (stream_type == 0x02 && (PESH_STREAM_ID(payload) & 0xF0) == 0xE0)
				) {
					return TSH_PID(packet);
				}
			}
		}
		return 0;
	}
	for (i = 0; i <= pat[0].last_section_number; i++) {
		for (k = 0; k < pat[i].program_length; k++) {
			if (pat[i].programs[k].program_number != 0 &&
				(pat[i].programs[k].program_number == program_number
				|| program_number == 0)
			) {
				PMT_SECTION *pmt = pat[i].programs[k].PMT;

				if (!pat[i].programs[k].PMT) continue;
				for (m = 0; m <= pmt[0].last_section_number; m++){
					for (n = 0; n < pmt[m].stream_length; n++){
						if (stream_type == pmt[m].streams[n].stream_type
							&& !(index--)
						) {
							return (program_number? 0:(unsigned long)pmt[m].program_number<<16) | pmt[m].streams[n].PID;
						}
					}
				}
				/* No stream_type stream in the program*/
				if (program_number) return 0;
			}
		}
	}
	return 0;
}
INLINE unsigned long tsb_get_stream_PID(
		ts_buffer *tsb,
		unsigned char stream_type,
		unsigned short program_number, int index
){
	if (!tsb->pat_ready) tsb_get_PAT(tsb);
	return get_stream_PID(tsb->pat, tsb, stream_type, program_number, index);
}

INLINE unsigned char *find_next_start_code(const unsigned char *buf, size_t length){
	const unsigned char *cur, *eob;
	
	if (length < 4) return NULL;
	eob = buf + length - 3;
	cur = buf;
	while ((cur = memchr(cur, 0, eob-cur)) ){
		if (cur[1] == 0x00 && cur[2] == 0x01) return (unsigned char *)cur;
		cur++;
	}
	return NULL;
}
INLINE unsigned char *find_next_non_slice(const unsigned char *buf, size_t length){
	const unsigned char *cur, *eob;

	if (length < 4) return NULL;
	eob = buf + length - 3;
	cur = buf;
	while ((cur = memchr(cur, 0, eob-cur)) ){
		if (cur[1] == 0x00 && cur[2] == 0x01 && !(0x00 < cur[3] && cur[3] < 0xB0)) {
			return (unsigned char *)cur;
		}
		cur++;
	}
	return NULL;
}

#define VIDEO_PTS_FIRST_PES          0
#define VIDEO_PTS_FIRST_GOP          1
#define VIDEO_PTS_FIRST_GOP_B        2
#define VIDEO_PTS_FIRST_GOP_B_CALC   3
#define VIDEO_PTS_FIRST_I            4
#define VIDEO_PTS_FIRST_ALIGNMENT    5
#define VIDEO_PTS_SYNCWORD_ALIGNMENT 6
INLINE double get_PTS (unsigned char pesh[16]) {
	if (PESH_HAS_PTS(pesh)) {
		return (double) PES_PTS(pesh+9) / 90.0;
	} else {
		return 0;
	}
}

INLINE double tsb_get_video_PTS(ts_buffer *tsb, unsigned short PID, int mode){
	unsigned char *buf;
	double result = 0.0, frame_rate = 0.0;
	__int64 raw_pts;
	size_t payload_size, bufsize = 8*1024*1024;
	int frames[4] = {0}, gop = 0, skip_packets = 0;
	bool pts_set = 0;

	buf = (unsigned char*)malloc(bufsize);
	if (!buf) return 0.0;
	
	tsb_set_PID(tsb, PID);
	while ( (payload_size = tsb_read_payload_unit(tsb, &buf, &bufsize, 1024*1024, 100*1024*1024, 0))
		&& skip_packets < 1000){
		const unsigned char *data, *start_code, *eob;
		size_t data_size, packet_length = PESH_PACKET_LENGTH(buf);
		
		eob = buf + payload_size;
		if (payload_size < 9) {
			//printf("Too small payload unit\n");
			tsb->error_message = "Too small payload unit"; 
			skip_packets++;
			continue;
		}
		if (!PESH_CHECK_SYNC(buf)) {
			//printf("PES sync error\n");
			tsb->error_message = "PES sync error"; 
			skip_packets++;
			continue;
		}
		if (packet_length && packet_length + 6 != payload_size) {
			//printf("SIZE: PES %d, Payload %d\n", packet_length+6, payload_size);
			if (packet_length + 6 > payload_size) {
				tsb->error_message = "PES pakcet too large"; 
			} else {
				tsb->error_message = "PES packet too small"; 
			}
			/* Ignore this error */
		}
			
		raw_pts = PES_PTS(buf+9);
		if (VIDEO_PTS_FIRST_ALIGNMENT == mode) {
			if (!PESH_IS_DATA_ALIGNED(buf)) {
				skip_packets++;
				continue;
			}
		}
		if (VIDEO_PTS_FIRST_PES == mode || VIDEO_PTS_FIRST_ALIGNMENT == mode) {
			if (PESH_HAS_PTS(buf)) {
				result = raw_pts/90.0;
				if (raw_pts == 0) result = 0.01;
				pts_set = 1;
				break;
			} else {
				tsb->error_message = "PES packet has no PTS";
				/* No first byte of audio access unit in this packet */
				skip_packets++;
				continue;
			}
		}

		data = skip_PES_header(buf, payload_size, &data_size);
		if (!data) {
			//printf("skip_PES_header() failed\n");
			tsb->error_message = "Invalid PES_header_data_length"; 
			skip_packets++;
			continue;
		}
		if (data_size < 4) {
			//printf("Too small PES data\n");
			tsb->error_message = "Too small PES data";
			skip_packets++;
			continue;
		}

		/* For MPEG Audio */
		if (VIDEO_PTS_SYNCWORD_ALIGNMENT == mode) {
			if (ADTS_IS_SYNCWORD(data)) {
				if (!PESH_HAS_PTS(buf)) break;
				result = raw_pts/90.0;
				if (raw_pts == 0) result = 0.01;
				pts_set = 1;
				break;
			} else {
				skip_packets++;
				continue;
			}
		}

		/* For MPEG2 Video */
		if (!(data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01)) {
			//printf("start_code sync error\n");
			tsb->error_message = "ES start_code sync error";
			skip_packets++;
			continue;
		}

		start_code = data;
		while ( (start_code = find_next_start_code(start_code, eob-start_code)) ){
			if (start_code[3] == 0xB3) {
				/* sequence_header_code */
				int frame_rate_code;
				static const double frame_rate_table[] = {0.0, 24.0/1001.0, 24.0, 25.0, 30.0/1001.0, 30.0, 50.0, 60.0/1001, 60};
				frame_rate_code = (start_code[7] & 0x0F);
				if (frame_rate_code < 9) frame_rate = frame_rate_table[frame_rate_code];
			}
			if (start_code[3] == 0xB5) {
				/* extension_start_code */
				if ((start_code[4] & 0xF0) == 0x10) {
					/* Sequence extension 0x1 */
					int frame_rate_extension_n, frame_rate_extension_d;
					frame_rate_extension_n = (start_code[9] & 0x60)>>5;
					frame_rate_extension_d = (start_code[9] & 0x1F);
					frame_rate = frame_rate * (frame_rate_extension_n+1) / (frame_rate_extension_d+1);
				}
			}
			if (start_code[3] == 0xB8) {
				/* group_start_code */
				if (PESH_HAS_PTS(buf)) {
					result = raw_pts/90.0;
					if (raw_pts == 0) result = 0.01;
				}
				if (mode == VIDEO_PTS_FIRST_GOP) { 
					pts_set = 1;
					break;
				}
				gop++;
				frames[0] = frames[1] = frames[2] = frames[3] = 0;
			}
			if (start_code[3] == 0x00) {
				/* picture_start_code */
				int field, pct;
				const unsigned char *pce = start_code;
				if (start_code + 4 > eob
					|| !(pce = find_next_start_code(start_code+4, eob-start_code-4))
					|| pce[3] !=0xB5 || (pce[4] & 0xF0) != 0x80
				) {
					tsb->error_message = "Picture coding extension is not found";
					pce = start_code;
					field = 2;
				} else {
					/* extension_start_code */
					/* Picture coding extension 0x8 */
					/* picture_structure = (start_code[6] & 0x03)
						1 TOP 2 BOTTOM 3 FRAME */
					field = ((start_code[6] & 0x03)==3)? 2:1;
				}
				/* picture_coding_type = (start_code[5] & 0x38)>>3 */
				pct = (start_code[5] & 0x18)>>3;
				frames[pct] += field;
				
				if (pct == 1 && PESH_HAS_PTS(buf)) {
					result = raw_pts/90.0;
					if (raw_pts == 0) result = 0.01;
				}
				if (VIDEO_PTS_FIRST_I == mode && frames[1]) {
					pts_set = 1;
					break;
				}
				if (VIDEO_PTS_FIRST_GOP_B_CALC == mode && frames[1] && (frames[2] || frames[1]>field)) {
					if (frame_rate) {
						result -= frames[3] / 2.0 / frame_rate;
					} else {
						tsb->error_message = "Frame rate is unknown";
					}
					pts_set = 1;
					break;
				}
				if (VIDEO_PTS_FIRST_GOP_B == mode && gop && (frames[2] || frames[1]>field)) {
					pts_set = 1;
					break;
				}
				if (VIDEO_PTS_FIRST_GOP_B == mode && gop && !frames[2] && frames[3]) {
					if (PESH_HAS_PTS(buf)) {
						result = raw_pts/90.0;
						if (raw_pts == 0) result = 0.01;
					} else if (frame_rate) {
						/* same as VIDEO_PTS_FIRST_GOP_B_CALC */
						tsb->error_message = "B frame has no PTS. Calculate from I frame.";
						result -= frames[3] / 2.0 / frame_rate;
					} else {
						tsb->error_message = "Frame rate is unknown";
					}
					pts_set = 1;
					break;
				}
				start_code = pce;
			}
			if (0x00 < start_code[3] && start_code[3] < 0xB0) {
				/* slice_start_code */
				if (find_next_non_slice(start_code, eob-start_code)) {
					tsb->error_message = "Many picture in PES packet";
					start_code = find_next_non_slice(start_code, eob-start_code);
				} else {
					break;
				}
			}
			start_code += 4;
			assert(start_code < eob);
		}
		if (pts_set) {
			break;
		}
	}
	free(buf);
	return result;
}
