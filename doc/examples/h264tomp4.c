#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libavformat/avformat.h"
#include "libavutil/mathematics.h"
#include "libavutil/bswap.h"
#include "libavutil/parseutils.h"
#include "libavutil/avstring.h"

#define SEPARATOR 0xFF
#define TRAILER_LEN	8

typedef struct FrameInfo {
	uint32_t frameid;
	uint64_t timestamp;
	char *nextfilename;
} FrameInfo;

uint64_t start_timestamp = 0;
uint64_t last_timestamp = 0;


static char* getmp4filename(char* filename, char* outfilename)
{
	char *file, *ext, tmp[1024];

	*outfilename = 0x00;
	file = strrchr(filename, '/');
	if (file) {
		if ((file - filename + 1) == strlen(filename)) {		
			return outfilename;
		}
		strncpy(outfilename, filename, file - filename);
	}
	else {
		file = filename;
	}
	
	ext = strrchr(file, '.');
	if (ext) {
		if (strrchr(ext, '/')) {
			*outfilename = 0x00;
			return outfilename;
		}
		strncpy(tmp, file, (int)(ext - file));
	}
	else {
		strcpy(tmp, file);
	}
	strcat(tmp, ".mp4");
	return strcat(outfilename, tmp);
}

static int alldigits(uint8_t *str, const int len)
{
	int i, count = 0;
	for (i = 0; i < len; i++) {
		if (av_isdigit(str[i])) count++;
	}
	return (len == count)?1:0;
}

static int hasextrainfo(AVPacket *pkt) {
	uint8_t *src_buf;
	uint8_t trailer[TRAILER_LEN];
	int trailercount = 0;

	if (!pkt) return trailercount;

	src_buf = pkt->data + pkt->size - TRAILER_LEN;
	memcpy(trailer, src_buf, TRAILER_LEN);

	if (alldigits(trailer,TRAILER_LEN)) {
		trailercount = atoi(trailer);
	}
	return trailercount;
}

static int8_t *getextrainfo(AVPacket *pkt, const int trailercount) {
	uint8_t *src_buf, *des_buf, *src_prt, *des_prt;
	int i, len, count;

	if (!pkt) return NULL;
	if (trailercount <= 0) return NULL;

	len = trailercount + TRAILER_LEN + 2;
	src_buf = pkt->data + pkt->size - (TRAILER_LEN + 1);
	if (*src_buf != SEPARATOR) {
		av_log(NULL,AV_LOG_ERROR,"separate char error.");
		return NULL;
	}
	src_buf = pkt->data + pkt->size - len;
	if (*src_buf != SEPARATOR) {
		av_log(NULL,AV_LOG_ERROR,"separate char error.");
		return NULL;
	}

	len = trailercount + 2;
	des_buf = malloc(len);
	if (!des_buf) return NULL;
	memset(des_buf, 0, len);

	count = 1;
	i = 1;
	des_prt = des_buf;
	src_prt = src_buf + 1;
	while (i + 2 < len)
	{
		if ((src_prt[0] == 0x00) && (src_prt[1] == 0x00) && (src_prt[2] == 0x03)) {
			count += 2;
			memcpy(des_prt, src_buf, count);
			des_prt += count; 
			src_prt += 3;
			src_buf = src_prt;
			count = 0;
			i += 3;
		}
		else {
			count += 1;
			src_prt += 1;
			i += 1;
		}
	}
	count += (len - i);
	if (count) {
		memcpy(des_prt, src_buf, count);
	}
	return des_buf;
}

static void formatdatetime(char *formated_datetime, uint64_t timestamp)
{
    time_t now_time;
    struct tm tm;
	
    now_time  = timestamp / 1000000;
	tm = *gmtime_r(&now_time, &tm);

	// UTC time format
	sprintf(formated_datetime, "%4d-%02d-%02dT%02d:%02d:%02d.%06dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(timestamp % 1000000));
}

static void formattime(char *formated_time, uint64_t time, int decimals, enum AVRounding round)
{
    int seconds = time / AV_TIME_BASE;
    int fractions = time % AV_TIME_BASE;
    int minutes = seconds / 60;
    int hours = minutes / 60;
	int len = 0;

	char *l_formated_time = formated_time;
    fractions = av_rescale_rnd(fractions, pow(10, decimals), AV_TIME_BASE, round);
    seconds %= 60;
    minutes %= 60;

    if (hours)
        len = sprintf(l_formated_time, "%dH ", hours);
    if (hours || minutes) {
		l_formated_time = l_formated_time + len;
        len = sprintf(l_formated_time, "%dM ", minutes);
	}
	l_formated_time = l_formated_time + len;
    sprintf(l_formated_time, "%d.%0*dS", seconds, decimals, fractions);
}

static struct FrameInfo *get_frameinfo(uint8_t *buffer, struct FrameInfo *frame_info, const int8_t need_swap)
{
	uint8_t separate_char;

	if (frame_info == NULL) return NULL;

	if (buffer == NULL) {
		av_log(NULL,AV_LOG_ERROR,"buffer is nulll.");
		return NULL;
	}

	frame_info->nextfilename = NULL;

	separate_char = *buffer;
	buffer++;
	if (separate_char != SEPARATOR) {
		av_log(NULL,AV_LOG_ERROR,"separate char error.");
		return NULL;
	}

	memcpy(&frame_info->frameid, buffer, sizeof(uint32_t));
	buffer = buffer + sizeof(uint32_t);
	if (need_swap) {
		frame_info->frameid = av_bswap32(frame_info->frameid);
	}
	
	av_log(NULL,AV_LOG_DEBUG,"frame id : %d",frame_info->frameid);

	memcpy(&frame_info->timestamp, buffer, sizeof(uint64_t));
	buffer = buffer + sizeof(uint64_t);
	if (need_swap) {
		frame_info->timestamp = av_bswap64(frame_info->timestamp);
	}

	av_log(NULL,AV_LOG_DEBUG,"timestamp : %ld",frame_info->timestamp);

	separate_char = *buffer;
	buffer++;
	if (separate_char != SEPARATOR) {
		av_log(NULL,AV_LOG_ERROR,"separate char error.");
		return NULL;
	}

	return frame_info;
}

static int setTimeStamp(AVPacket *pkt, uint8_t *buffer, int8_t need_swap)
{
	struct FrameInfo frame_info;

	if (get_frameinfo(buffer, &frame_info, need_swap) == NULL) 
		return -1;

	//Write PTS
	//Parameters
	if (last_timestamp == 0) {
		pkt->pts = 0;
		pkt->dts=pkt->pts;
		pkt->duration = 0;
		start_timestamp = frame_info.timestamp;
	}
	else {
		pkt->pts = frame_info.timestamp - start_timestamp;
		pkt->dts=pkt->pts;
		pkt->duration = frame_info.timestamp - last_timestamp;
	}
	last_timestamp = frame_info.timestamp;
	pkt->pos = -1;
	pkt->time_base = (AVRational){1, AV_TIME_BASE};

	return 0;
}

static FILE *open_index(const char *index_filename)
{
	FILE *file = fopen(index_filename,"rb");
	if (file == NULL) {
		av_log(NULL,AV_LOG_ERROR,"Could not open index file : %s.\n", index_filename);
	}
	return file;
}

static void close_index(FILE *file)
{
	if (file) {
		fclose(file);
	}
}

int main(int argc, char **argv)
{
	AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
	AVPacket pkt;
	char in_filename[1024], out_filename[1024], index_filename[1024];
	char format_time[32];
	int ret, i;
	int stream_index = 0;
	int *stream_mapping = NULL;
	int stream_mapping_size = 0;
	int m_frame_index = 0;
	int trailercount = 0;
	uint8_t buffer[128];

	FILE *index_file = NULL;
	memset(in_filename, 0, 1024);
	memset(out_filename, 0, 1024);
	memset(index_filename, 0, 1024);
	
	strcpy(in_filename, "./0420/stream_with_pts.h264");
	// strcpy(in_filename, "./0420/stream_without_pts.h264");
	// strcpy(in_filename, "./0420/rec.h264");
	// strcpy(in_filename, "./0420/timestemp.h264");

	getmp4filename(in_filename, out_filename);

	// strcpy(index_filename, "./0420/stream_only_pts.h264");
	if (strlen(index_filename) != 0) {
		if ((index_file = open_index(index_filename)) == NULL) {
				printf("Could not open index file: %s.\n", index_filename); 
			}
	}
	 
	if ((ret = avformat_open_input(&ifmt_ctx, in_filename, 0, 0)) < 0) {
		printf("Could not open input file: %s.\n", in_filename); 
		goto end;
	}
	if ((ret = avformat_find_stream_info(ifmt_ctx, 0)) < 0) {
		printf("Failed to retrieve input stream information");
		goto end;
	}
 
	printf("===========Input Information==========\n"); 
	av_dump_format(ifmt_ctx, 0, in_filename, 0); 
	printf("======================================\n"); 
 
	avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, out_filename);
	if (!ofmt_ctx) {
		printf("Could not create output context\n"); 
		ret = AVERROR_UNKNOWN; 
		goto end;
	}
 
	stream_mapping_size = ifmt_ctx->nb_streams;
	stream_mapping = (int *)av_calloc(stream_mapping_size, sizeof(*stream_mapping));
	if (!stream_mapping) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
  
	for (i = 0; i < ifmt_ctx->nb_streams; i++) {
		AVStream *out_stream;
		AVStream *in_stream = ifmt_ctx->streams[i];
		AVCodecParameters *in_codecpar = in_stream->codecpar;
 
		if (in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
			in_codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
			in_codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
				stream_mapping[i] = -1;
				continue;
		}
 
		stream_mapping[i] = stream_index++;

		// add new stream to media file (ofmt_ctx)
		out_stream = avformat_new_stream(ofmt_ctx, NULL);
		if (!out_stream) {
            fprintf(stderr, "Failed allocating output stream\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }
 
		ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
    	if (ret < 0) {
            fprintf(stderr, "Failed to copy codec parameters\n");
            goto end;
        }
		out_stream->codecpar->codec_tag = 0;

		out_stream->time_base = (AVRational){1, AV_TIME_BASE};
	}
 
	if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
		ret = avio_open(&ofmt_ctx->pb, out_filename, AVIO_FLAG_WRITE);
		if (ret < 0) {
			fprintf(stderr, "Could not open output file '%s'", out_filename);
            goto end;
		}
	}

	ret = avformat_write_header(ofmt_ctx, NULL);
	if (ret < 0) {
		fprintf(stderr, "Error occurred when opening output file\n");
        goto end;
	}

	m_frame_index = 0;
	while (1) {
		AVStream *in_stream, *out_stream;
 
		ret = av_read_frame(ifmt_ctx, &pkt);
		if (ret < 0)
			break;
 
		in_stream  = ifmt_ctx->streams[pkt.stream_index];
		if (pkt.stream_index >= stream_mapping_size ||
			stream_mapping[pkt.stream_index] < 0) {
				av_packet_unref(&pkt);
				continue;
		}
 
		pkt.stream_index = stream_mapping[pkt.stream_index];
		out_stream = ofmt_ctx->streams[pkt.stream_index];
 
		//add timestamp
		if(pkt.pts==AV_NOPTS_VALUE){
			if (index_file == NULL) {
				
				if (!(trailercount = hasextrainfo(&pkt))) {
					//Write PTS
					AVRational time_base1=in_stream->time_base;
					//Duration between 2 frames (us)
					int64_t calc_duration=(double)AV_TIME_BASE/av_q2d(in_stream->r_frame_rate);

					//Parameters
					pkt.pts=(double)(m_frame_index*calc_duration)/(double)(av_q2d(time_base1)*AV_TIME_BASE);
					pkt.dts=pkt.pts;
					pkt.duration=(double)calc_duration/(double)(av_q2d(time_base1)*AV_TIME_BASE);

					/* copy packet */
					pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, out_stream->time_base, (enum AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
					pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, out_stream->time_base, (enum AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
					pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
					pkt.pos = -1;
				}
				else {
					uint8_t *des_buf = getextrainfo(&pkt, trailercount);
					if (des_buf) {
						setTimeStamp(&pkt, des_buf, 1);
						free(des_buf);
					}
				}		
			}
			else {
				if (fread(buffer, 14, 1, index_file) != 1) 
					return -1;
				setTimeStamp(&pkt, buffer, 1);
			}
		}
  
		ret = av_interleaved_write_frame(ofmt_ctx, &pkt);
		if (ret < 0) {
			break;
		}
		av_packet_unref(&pkt);
		m_frame_index++;
	}

	av_write_trailer(ofmt_ctx);

	printf("\n");
	if (strlen(index_filename)) {
		printf("Index: %s\n", index_filename);
	}
	printf("Output: %s\n", out_filename);
	formatdatetime(format_time, start_timestamp);
 	printf("start time: %s, ", format_time);
 	formatdatetime(format_time, last_timestamp);
 	printf("end time: %s\n", format_time);
	formattime(format_time, last_timestamp - start_timestamp, 1, AV_ROUND_DOWN);
	printf("total frames: %d, duration: %s, frame rate: %5.1f\n", m_frame_index, format_time, (float)m_frame_index*AV_TIME_BASE/(last_timestamp - start_timestamp));
	printf("\n");

end:
	avformat_close_input(&ifmt_ctx); 
	if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE))
		avio_closep(&ofmt_ctx->pb);
	avformat_free_context(ofmt_ctx);
 
	av_freep(&stream_mapping);
	close_index(index_file);
	
	return 0;
}
