#ifndef MPEG_HEADER_INCLUDED
#define MPEG_HEADER_INCLUDED

/*
TS packet header
		 header[0] == 0x47		Syncword
		 header[1] & 0x80		Transport error
		 header[1] & 0x40		Payload unit start PES or PSI
		 header[1] & 0x20		Transport priority
		(header[1] & 0x1F) << 8 | header[2];		PID
		(header[3] & 0xC0) >> 6	Transport scramble
		(header[3] & 0x30) >> 4	Adaptation field control
		 header[3] & 0x0F		Continuity counter
*/
#define TS_HEADER_SIZE 4

#define TSH_CHECK_SYNC(h)         ((h)[0] == 0x47)
#define TSH_IS_ERROR              ((h)[1] & 0x80)
//#define TSH_IS_PAYLOAD_START(h)   ((h)[1] & 0x40)
#define TSH_IS_PAYLOAD_UNIT_START(h) ((h)[1] & 0x40)
#define TSH_IS_PRIORITY(h)        ((h)[1] & 0x20)
#define TSH_PID(h)              ( ((h)[1] & 0x1F) << 8 | (h)[2])
#define TSH_SCRAMBLED(h)        ( ((h)[3] & 0xC0) >> 6)
#define TSH_ADAPTATION_CONTROL   (((h)[3] & 0x30) >> 4)
#define TSH_HAS_ADAPTATION(h)     ((h)[3] & 0x20)
#define TSH_HAS_PAYLOAD(h)        ((h)[3] & 0x10)
#define TSH_CONTINUITY_COUNTER(h) ((h)[3] & 0x0F)

/*
PID
		0x0000 Program Association Table
		0x0001 Conditional Access Table
		0x0002 Transport Stream Description Table
		0x0003-0x000F Reserved
		0x00010-0x1FFE May be assigned as network_PID, Program_map_PID, elementary_PID, or for other purposes
		0x1FFF Null packet
*/
#define PID_PAT  0x0000
#define PID_CAT  0x0001
#define PID_TSDT 0x0002
#define PID_NULL 0x1FFF

/*
 TS adaptation field
 		field[0]		adaptation field length (not including this 1 byte)
		field[1]		discontinuity_indicator
						random_access_indicator
						elementary_stream_priority_indicator
						PCR_flag
						OPCR_flag
						splicing_point_flag
						transport_private_data_flag
						adaptation_field_extension_flag

	PCR OPCR
		pcr[0] << 25
		pcr[1] << 17
		pcr[2] <<  9
		pcr[3] >>  1
		(pcr[4] & 0x80) >> 7	program clock reference base
		(pcr[4] & 0x01) << 8
		pcr[5]					program clock reference extention
 */
/* TS header and adaptation field length */
#define TSH_HEADER_FIELD_LENGTH(ts) (TSH_HAS_ADAPTATION(ts)? TS_HEADER_SIZE+1+(ts)[4]:4)
#define TSH_GET_PAYLOAD_SIZE(ts)    (TS_PACKET_SIZE - TSH_HEADER_FIELD_LENGTH(ts))
#define TSH_GET_PAYLOAD(ts)         ((ts) + TSH_HEADER_FIELD_LENGTH(ts))
#define TSAH_HAS_PCR(a)   ((a) & 0x10)
#define TSAH_HAS_OPCR(a)  ((a) & 0x08)

#define TSA_PCR_BASE(p)  ((__int64)(p)[0]<<25 | (p)[1]<<17 | (p)[2]<<9 | (p)[3]<<1 | ((p)[4]&0x80)>>7)
#define TSA_PCR_BASE_DOUBLE(p) ((double)(p)[0]*0x02000000L + ((p)[1]<<17 | (p)[2]<<9 | (p)[3]<<1 | ((p)[4]&0x80)>>7) )
#define TSA_PCR_EXTENTION(p) (((p)[4]&0x01)<<8 | (p)[5])

 /*
  MPEG system clock frequency
 		 27,000,000 - 810 <= system_clock_frequency <= 27,000,000 + 810
 */
#define SYSTEM_CLOCK_FREQUENCY 27000000
#define TIME_STAMP_FREQUENCY      90000
#define TIME_STAMP2MS(ts)       ((double)(ts)/(TIME_STAMP_FREQUENCY/1000))

#define TS_PACKET_SIZE 188

/*
PSI
		header[0]			pointer_field

PES header
 		header[0] == 0x00
		header[1] == 0x00
		header[2] == 0x01		sync
		header[3] 			stream id
		header[4] << 8
		header[5]			PES packet length

		header[6] & 0xC0 == 0x80	'10'
		(header[6] & 0x30) >> 4		PES_scrambling_control
		header[6] & 0x08			PES_priority
		header[6] & 0x04			data_alignment_indicator
		header[6] & 0x02			copyright
		header[6] & 0x01			original_or_copy
		(header[7] & 0xC0) >> 6		PTS_DTS_flags
		header[7] & 0x20			ESCR_flag
		header[7] & 0x10			ES_rate_flag
		header[7] & 0x08			DSM_trick_mode_flag
		header[7] & 0x04			additional_copy_info_flag
		header[7] & 0x02			PES_CRC_flag
		header[7] & 0x01			PES_extension_flag
		header[8] 					PES_header_data_length
	
	PTS
		pts[0] & 0xF0 == 0x30 or 0x20
		(pts[0] & 0x0E) << 29
		pts[1] << 22
		(pts[2] & 0xFE) << 14
		pts[3] << 7
		(pts[4] & 0xFE) >> 1		PTS
		pts[0] & 0x01 == 1			marker bit
		pts[2] & 0x01 == 1			marker bit
		pts[4] & 0x01 == 1			marker bit

 */
#define PES_HEADER_SIZE  9
#define PESH_CHECK_SYNC(h)    ((h)[0] == 0 && (h)[1] == 0 && (h)[2] == 1)
#define PESH_STREAM_ID(h)     ((h)[3])
#define PESH_PACKET_LENGTH(h) ((h)[4] << 8 | (h)[5])

#define PESH_IS_DATA_ALIGNED(h) ((h)[6] & 0x04)

#define PESH_HAS_PTS(h) ((h)[7] & 0x80)
#define PESH_HAS_DTS(h) ((h)[7] & 0x40)
#define PESH_HEADER_LENGTH(h) ((h)[8])

#define PES_PTS(p) (((__int64)(p)[0]&0x0E)<<29 | (p)[1]<<22 | ((p)[2]&0xFE)<<14 | (p)[3]<<7 | ((p)[4]&0xFE)>>1)
#define PTS_MAX      0x1FFFFFFFFLL
#define PTS_MAX_TIME 95443717.677777777

/*
PES stream id
1011 1100 program_stream_map
1011 1101 private_stream_1
1011 1110 padding_stream
1011 1111 private_stream_2
110x xxxx ISO/IEC 13818-3 or ISO/IEC 11172-3 or ISO/IEC 13818-7 or ISO/IEC14496-3 audio stream number x xxxx
1110 xxxx ITU-T Rec. H.262 | ISO/IEC 13818-2 or ISO/IEC 11172-2 or ISO/IEC14496-2 video stream number xxxx
1111 0000 ECM_stream
1111 0001 EMM_stream
1111 0010 ITU-T Rec. H.222.0 | ISO/IEC 13818-1 Annex A or ISO/IEC 13818-6_DSMCC_stream
1111 0011 ISO/IEC_13522_stream
1111 0100 ITU-T Rec. H.222.1 type A
1111 0101 ITU-T Rec. H.222.1 type B
1111 0110 ITU-T Rec. H.222.1 type C
1111 0111 ITU-T Rec. H.222.1 type D
1111 1000 ITU-T Rec. H.222.1 type E
1111 1001 ancillary_stream
1111 1010 ISO/IEC14496-1_SL-packetized_stream
1111 1011 ISO/IEC14496-1_FlexMux_stream
1111 1100 Åc 1111 1110 reserved data stream
1111 1111 program_stream_directory
*/
#define SID_PROGRAM_STREAM_MAP 0xBC
#define SID_PRIVATE_STREAM_1   0xBD
#define SID_PADDING_STREAM     0xBE
#define SID_PRIVATE_STREAM_2   0xBF

#define SID_ECM_STREAM         0xF0
#define SID_EMM_STREAM         0xF1
#define SID_DSMCC_STREAM       0xF2

#define SID_H222_1_E           0xF8

#define SID_PROGRAM_STREAM_DIRECTORY 0xFF

/*
PAT

		pat[0]				table_id (0x00)
		pat[1] & 0x80		section_syntax_indicator (1)
		pat[1] & 0x40 == 0 '0'
		pat[1] & 0x30		reserved
		((pat[1] & 0x0F) << 8) |
		pat[2]				section_length 10bit (upper 2bit is 00)
		pat[3] << 8 |
		pat[4]				transport_stream_id
		pat[5] & 0xC0		reserved
		(pat[5] & 0x3E) >> 1	version_number
		pat[5] & 0x01		current_next_indicator (1:current 0:next)
		pat[6]				section_number
		pat[7]				last_section_number
		for{
			(table[0] << 8) |
			table[1]			program_number
			table[2] & 0xE0		reserved
			((table[2] & 0x1F) << 8) |
			table[3]			PID if(program_number == 0){network_PID}else{program_map_PID}
		}
		DWORD				CRC_32 32
*/
#define PATS_CHECK_TABLE_ID(s)          (  (s)[0] == 0x00)
#define PATS_CHECK_SECTION_SYNTAX(s)    ( ((s)[1] & 0xC0) == 0x80 )
#define PATS_SECTION_LENGTH(s)          ( ((s)[1] & 0x03) << 8 | (s)[2] )
#define PATS_TS_ID(s)                   (  (s)[3] << 8 | (s)[4] )
#define PATS_VERSION_NUMBER(s)          ( ((s)[5] & 0x3E) >> 1 )
#define PATS_IS_CURRENT(s)                ((s)[5] & 0x01)
#define PATS_SECTION_NUMBER(s)            ((s)[6])
#define PATS_LAST_SECTION_NUMBER(s)       ((s)[7])
#define PAT_PROGRAM_NUMBER(t)             ((t)[0] << 8 | (t)[1])
#define PAT_PID(t)                      ( ((t)[2] & 0x1F) << 8 | (t)[3] )

/*
PMT program map section

	pmt[0]					table_id (0x02)
	pmt[1] & 0x80			section_syntax_indicator (1)
	pmt[1] & 0x40 == 0x40	'0'
	pmt[1] & 0x30			reserved
	(pmt[1] & 0x0F) << 4 |
	pmt[2]					section_length 10bit (upper 2bit is 00)
	pmt[3] << 8 |
	pmt[4]					program_number
	pmt[5] & 0xC0			reserved
	(pmt[5] & 0x3E) >> 1	version_number
	pmt[5] & 0x01			current_next_indicator
	pmt[6]					section_number
	pmt[7]					last_section_number
	pmt[8] & 0xE0			reserved
	(pmt[8] & 0x1F) << 8 |
	pmt[9]					PCR_PID
	pmt[10] & 0xF0			reserved
	(pmt[10] & 0x0F) << 8 |	
	pmt[11]					program_info_length 10bit (upper 2bit is 00)
	for{
		descriptor()
	}
	for{
		table[0]			stream_type
		table[1] & 0xE0		reserved
		table[1] & 0x1F |
		table[2]			elementary_PID
		table[3] & 0xF0		reserved
		table[3] & 0x0F |
		table[4]			ES_info_length 10bit
		for{
			descriptor()
		}
	}
	CRC_32 32

descripter
	d[0]	tag
	d[1]	size
	.....
*/
#define PMTS_CHECK_TABLE_ID(s)          (  (s)[0] == 0x02)
#define PMTS_CHECK_SECTION_SYNTAX(s)    PATS_CHECK_SECTION_SYNTAX(s)
#define PMTS_SECTION_LENGTH(s)          PATS_SECTION_LENGTH(s)
#define PMTS_PROGRAM_NUMBER(s)          PATS_TS_ID(s)
#define PMTS_VERSION_NUMBER(s)          PATS_VERSION_NUMBER(s)
#define PMTS_IS_CURRENT(s)              PATS_IS_CURRENT(s)
#define PMTS_SECTION_NUMBER(s)          PATS_SECTION_NUMBER(s)
#define PMTS_LAST_SECTION_NUMBER(s)     PATS_LAST_SECTION_NUMBER(s)
#define PMTS_PCR_PID(s)                 ( ((s)[8] & 0x1f) << 8 | (s)[9] )
#define PMTS_PROGRAM_INFO_LENGTH(s)     ( ((s)[10] & 0x03) << 8 | (s)[11] )

#define PMT_STREAM_TYPE(t)              ( (t)[0] )
#define PMT_EREMENTARY_PID(t)           ( ((t)[1] & 0x1F) << 8 | (t)[2] )
#define PMT_ES_INFO_LENGTH(t)           ( ((t)[3] & 0x03) << 8 | (t)[4] )

/*
Stream type
0x00 ITU-T | ISO/IEC Reserved
0x01 ISO/IEC 11172 Video
0x02 ITU-T Rec. H.262 | ISO/IEC 13818-2 Video or ISO/IEC 11172-2 constrained parameter video stream
0x03 ISO/IEC 11172 Audio
0x04 ISO/IEC 13818-3 Audio
0x05 ITU-T Rec. H.222.0 | ISO/IEC 13818-1 private_sections
0x06 ITU-T Rec. H.222.0 | ISO/IEC 13818-1 PES packets containing private data
0x07 ISO/IEC 13522 MHEG
0x08 ITU-T Rec. H.222.0 | ISO/IEC 13818-1 Annex A DSM-CC
0x09 ITU-T Rec. H.222.1
0x0A ISO/IEC 13818-6 type A
0x0B ISO/IEC 13818-6 type B
0x0C ISO/IEC 13818-6 type C
0x0D ISO/IEC 13818-6 type D
0x0E ITU-T Rec. H.222.0 | ISO/IEC 13818-1 auxiliary
0x0F ISO/IEC 13818-7 Audio with ADTS transport syntax
0x10 ISO/IEC 14496-2 Visual
0x11 ISO/IEC 14496-3 Audio with the LATM transport syntax as defined in ISO/IEC 14496-3 / AMD 1
0x12 ISO/IEC 14496-1 SL-packetized stream or FlexMux stream carried in PES packets
0x13 ISO/IEC 14496-1 SL-packetized stream or FlexMux stream carried in ISO/IEC14496_sections.
0x14 ISO/IEC 13818-6 Synchronized Download Protocol
0x15-0x7F ITU-T Rec. H.222.0 | ISO/IEC 13818-1 Reserved
0x80-0xFF User Private
*/

#define STREAM_TYPE_TABLE {\
"Reserved", \
"MPEG1 Video", \
"MPEG2 Video", \
"MPEG1 Audio", \
"MPEG2 Audio", \
"MPEG2 private_section", \
"MPEG2 Private PES", \
"MHEG", \
"DSM-CC", \
"H.222.1", \
"MPEG2 DSM-CC A", \
"MPEG2 DSM-CC B", \
"MPEG2 DSM-CC C", \
"MPEG2 DSM-CC D", \
"MPEG2 AUX", \
"MPEG2 AAC", \
"MPEG4 Video", \
"MPEG4 Audio", \
"MPEG4 PES", \
"MPEG4 stream", \
"MPEG2 SDP", \
"Other" \
}

/*
PS pack header
	header[0] == 0x00
	header[1] == 0x00
	header[2] == 0x01
	header[3] == 0xBA	pack_start_code
	header[4] & 0xC0 == 0x40	'01'
	header[4] & 0x38	system_clock_reference_base [32..30]
	header[4] & 0x04	marker_bit 1
	header[4] & 0x03
	header[5]
	header[6] & 0xF8	system_clock_reference_base [29..15]
	header[6] & 0x04	marker_bit 1
	header[6] & 0x03	
	header[7]
	header[8] & 0xF8	system_clock_reference_base [14..0]
	header[8] & 0x04	marker_bit 1
	header[8] & 0x03
	header[9] & 0xFE	system_clock_reference_extension
	header[9] & 0x01	marker_bit 1
	header[10]
	header[11]
	header[12] & 0xFC	program_mux_rate
	header[12] & 0x03	marker_bit 1
						marker_bit 1
	header[13] & 0xF8	reserved
	header[13] & 0x07	pack_stuffing_length
	for (i = 0; i < pack_stuffing_length; i++) {
		0xFF	stuffing_byte
	}
	if (nextbits() == system_header_start_code) {
		system_header ()
	}
*/
#endif
