#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "libavformat/avformat.h"
#include "libavutil/mathematics.h"
#include "libavutil/bswap.h"
#include "libavutil/parseutils.h"
#include "libavutil/avstring.h"

#define SEPARATOR 0xFF
#define TRAILER_LEN	8
#define FILENAME_LEN 1024

typedef struct FrameInfo {
	uint32_t frame_id;
	uint64_t timestamp;
	char *next_filename;
} FrameInfo;

typedef struct VideoFileInfo
{
	char *filename;
	FrameInfo start_frame_info;
	FrameInfo last_frame_info;
	int min_delta_time;
	int max_delta_time;
	int32_t frame_count;
} VideoFileInfo;

typedef struct VideoFile
{
	char in_filename[FILENAME_LEN];
	char out_filename[FILENAME_LEN];
	char index_filename[FILENAME_LEN];
	AVFormatContext *in_fmt_ctx;
	AVFormatContext *out_fmt_ctx;
	FILE *index_file;
} VideoFile;

VideoFileInfo **video_file_infos = NULL;
char root_path[FILENAME_LEN];
int video_file_info_index = 0;

int frame_index = 0;
int display_msg = 1;

static void init_video_file(VideoFile *videofile) {
	memset(videofile->in_filename, 0, FILENAME_LEN);
	memset(videofile->out_filename, 0, FILENAME_LEN);
	memset(videofile->index_filename, 0, FILENAME_LEN);
	videofile->in_fmt_ctx = NULL;
	videofile->out_fmt_ctx = NULL;
	videofile->index_file = NULL;
}

static void copy_video_file(VideoFile *des_videofile, VideoFile *src_videofile) {
	strcpy(des_videofile->in_filename, src_videofile->in_filename);
	strcpy(des_videofile->out_filename, src_videofile->out_filename);
	strcpy(des_videofile->index_filename, src_videofile->index_filename);
	des_videofile->in_fmt_ctx = src_videofile->in_fmt_ctx;
	des_videofile->out_fmt_ctx = src_videofile->out_fmt_ctx;
	des_videofile->index_file = src_videofile->index_file;
}

static void free_video_file(VideoFileInfo *video_file_info) {
	if (video_file_info) {
		free(video_file_info->filename);
		free(video_file_info->start_frame_info.next_filename);
		free(video_file_info->last_frame_info.next_filename);
		free(video_file_info);
	}
}

static void free_video_file_infos(VideoFileInfo **video_file_infos, int count) {
	int i;

	if (video_file_infos) {
		for (i = 0; i < count; i++) {
			free_video_file(video_file_infos[i]);
		}
		free(video_file_infos);
	}
}

static void update_delta_time(VideoFileInfo *video_file_info, int deltatime) {
	if (video_file_info && (deltatime > 0)) {
		if (deltatime < video_file_info->min_delta_time) {
			video_file_info->min_delta_time = deltatime;
		}
		else if (deltatime > video_file_info->max_delta_time) {
			video_file_info->max_delta_time = deltatime;
		}
	}
	video_file_info->frame_count ++;
}

static void init_frame_info(FrameInfo *frame_info) {
	frame_info->frame_id = 0;
	frame_info->timestamp = 0;
	frame_info->next_filename = NULL;
}

static void init_video_file_info(VideoFileInfo *video_file_info, char *filename) {
	if (video_file_info && filename) {
		video_file_info->filename = malloc(strlen(filename) + 1);
		strcpy(video_file_info->filename, filename);

		video_file_info->frame_count = 0;
		video_file_info->max_delta_time = 0;
		video_file_info->min_delta_time = AV_TIME_BASE;
		init_frame_info(&video_file_info->start_frame_info);
		init_frame_info(&video_file_info->last_frame_info);
	}
}

static char* get_filename(char* filename) {
	char* file = strrchr(filename, '/');
	if (file) {
		file++;
	}
	else {
		file = filename;
	}
	return file;
}

static char* get_file_ext(char* filename) {
	char *file;

	file = strrchr(filename, '/');
	if (file) {
		if ((file - filename + 1) == strlen(filename)) {	
			return NULL;
		}
	}
	else {
		file = filename;
	}
	return strrchr(file, '.');
}

static char* chang_ext_name(char* filename, char* outfilename, const char* extname)
{
	char *file, *ext, tmp[FILENAME_LEN];

	file = strrchr(filename, '/');
	if (file) {
		if ((file - filename + 1) == strlen(filename)) {
			*outfilename = 0x00;		
			return outfilename;
		}
		memset(outfilename, 0, file - filename + 1);
		strncpy(outfilename, filename, file - filename);
	}
	else {
		file = filename;
	}
	
	ext = strrchr(file, '.');
	if (ext) {
		memset(tmp, 0, ext -  file + 1);
		strncpy(tmp, file, ext - file);
	}
	else {
		strcpy(tmp, file);
	}

	if (strlen(extname)) {
		ext = strrchr(extname, '.');
		if (ext) {
			strcat(tmp, ext);
		}
		else {
			strcat(tmp, ".");
			strcat(tmp, ext);
		}
	}
	
	return strcat(outfilename, tmp);
}

static int all_digits(uint8_t *str, const int len)
{
	int i, count = 0;
	for (i = 0; i < len; i++) {
		if (av_isdigit(str[i])) count++;
	}
	return (len == count)?1:0;
}

static int has_extra_info(AVPacket *pkt) {
	uint8_t *src_buf;
	uint8_t trailer[TRAILER_LEN + 1];
	int trailer_count = 0;

	if (!pkt) return trailer_count;
	memset(trailer, 0, TRAILER_LEN + 1);
	src_buf = pkt->data + pkt->size - TRAILER_LEN;
	memcpy(trailer, src_buf, TRAILER_LEN);

	if (all_digits(trailer,TRAILER_LEN)) {
		trailer_count = atoi(trailer);
	}
	return trailer_count;
}

static int8_t *get_extra_info(AVPacket *pkt, const int trailercount) {
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

static void format_datetime(char *formated_datetime, uint64_t timestamp)
{
    time_t now_time;
    struct tm tm;
	
    now_time  = timestamp / 1000000;
	tm = *gmtime_r(&now_time, &tm);

	// UTC time format
	sprintf(formated_datetime, "%4d-%02d-%02dT%02d:%02d:%02d.%06dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(timestamp % 1000000));
}

static void format_time(char *formated_time, uint64_t time, int decimals, enum AVRounding round)
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

static struct FrameInfo *get_frame_info(uint8_t *buffer, FrameInfo *frame_info, const int8_t need_swap)
{
	uint8_t separate_char, *trailer;
	int len;

	if (frame_info == NULL) return NULL;

	if (buffer == NULL) {
		av_log(NULL,AV_LOG_ERROR,"buffer is nulll.");
		return NULL;
	}

	frame_info->next_filename = NULL;

	separate_char = *buffer;
	buffer++;
	if (separate_char != SEPARATOR) {
		av_log(NULL,AV_LOG_ERROR,"separate char error.");
		return NULL;
	}

	memcpy(&frame_info->frame_id, buffer, sizeof(uint32_t));
	buffer = buffer + sizeof(uint32_t);
	if (need_swap) {
		frame_info->frame_id = av_bswap32(frame_info->frame_id);
	}
	
	av_log(NULL,AV_LOG_DEBUG,"frame id : %d",frame_info->frame_id);

	memcpy(&frame_info->timestamp, buffer, sizeof(uint64_t));
	buffer = buffer + sizeof(uint64_t);
	if (need_swap) {
		frame_info->timestamp = av_bswap64(frame_info->timestamp);
	}

	av_log(NULL,AV_LOG_DEBUG,"timestamp : %ld",frame_info->timestamp);

	separate_char = *buffer;
	trailer = strchr(buffer, SEPARATOR);
	if (trailer) {
		if (trailer > buffer) {
			len = trailer - buffer;
			frame_info->next_filename = calloc(sizeof(char), len + 1);
			strncpy(frame_info->next_filename, buffer, trailer - buffer);
		}
		else {
			if ((trailer != buffer) || (separate_char != SEPARATOR)) 
			{
				av_log(NULL,AV_LOG_ERROR,"separate char error.");
				return NULL;
			}
			len = 0;
		}
	}
	else if (len = strlen(buffer)) {
		frame_info->next_filename = calloc(sizeof(char), len + 1);
		strcpy(frame_info->next_filename, buffer);
		len++;
	}
	
	buffer = buffer + len + 1;
	return frame_info;
}

static int set_timeStamp(VideoFileInfo **video_file_infos, int video_index, AVPacket *pkt, uint8_t *buffer, int8_t need_swap)
{
	FrameInfo frame_info;
	VideoFileInfo *video_file_info = video_file_infos[video_index];
	
	if (get_frame_info(buffer, &frame_info, need_swap) == NULL) 
		return -1;

	//Write PTS
	//Parameters
	if ((video_file_info->frame_count == 0) && (video_file_info->last_frame_info.timestamp == 0)) {
		if (video_index == 0) {
			pkt->pts = 0;
			pkt->dts = pkt->pts;
			pkt->duration = 0;
		}
		else {
			pkt->pts = frame_info.timestamp - video_file_infos[0]->start_frame_info.timestamp;
			pkt->dts = pkt->pts;
			pkt->duration = frame_info.timestamp - video_file_infos[video_index -1]->last_frame_info.timestamp;
		}
		video_file_info->start_frame_info.frame_id = frame_info.frame_id;
		video_file_info->start_frame_info.timestamp = frame_info.timestamp;
	}
	else {
		pkt->pts = frame_info.timestamp - video_file_infos[0]->start_frame_info.timestamp;
		pkt->dts = pkt->pts;
		pkt->duration = frame_info.timestamp - video_file_info->last_frame_info.timestamp;
	}
	video_file_info->last_frame_info.frame_id = frame_info.frame_id;
	video_file_info->last_frame_info.timestamp = frame_info.timestamp;

	if (frame_info.next_filename) {
		video_file_info->last_frame_info.next_filename = calloc(sizeof(char), strlen(frame_info.next_filename) + 1);
		strcpy(video_file_info->last_frame_info.next_filename, frame_info.next_filename);
		free(frame_info.next_filename);
	}
	pkt->pos = -1;
	pkt->time_base = (AVRational){1, AV_TIME_BASE};

	update_delta_time(video_file_info, pkt->duration);

	return 0;
}

static FILE *open_index_file(const char *index_filename)
{
	FILE *file = fopen(index_filename,"rb");
	if (file == NULL) {
		av_log(NULL,AV_LOG_ERROR,"Could not open index file : %s.\n", index_filename);
	}
	return file;
}

static void close_index_file(FILE *file)
{
	if (file) {
		fclose(file);
	}
}

static int transform(VideoFile *videofile, VideoFile *next_videofile)
{
	AVFormatContext *in_fmt_ctx = NULL;
	AVFormatContext *out_fmt_ctx = NULL;
	AVPacket pkt;
	VideoFileInfo *video_file_info = NULL;
	char *fileext = NULL;
	char formated_time[32];
	int ret, i;
	int stream_index = 0;
	int *stream_mapping = NULL;
	int stream_mapping_size = 0;
	int trailer_count = 0;
	uint8_t buffer[128];
	char *in_filename, *out_filename, *index_filename;
	FILE *index_file = NULL;
	int new_output;

	in_filename = videofile->in_filename;
	out_filename = videofile->out_filename;
	index_filename = videofile->index_filename;

	new_output = (videofile->out_fmt_ctx == NULL);

	video_file_infos = realloc(video_file_infos, sizeof(VideoFileInfo *) * (video_file_info_index + 1));
	if (!video_file_infos) {
		ret = AVERROR(ENOMEM);
		goto end;
	}
	if (!(video_file_infos[video_file_info_index] = calloc(sizeof(VideoFileInfo), 1))) {
		ret = AVERROR(ENOMEM);
		goto end;
	}
	video_file_info = video_file_infos[video_file_info_index];	
	init_video_file_info(video_file_info, in_filename);

	if (fileext = get_file_ext(in_filename)) {

	}

	if (new_output) {
		if (strlen(out_filename) == 0) {
			chang_ext_name(in_filename, out_filename, ".mp4");
		}
	}
		
	if (strlen(index_filename) == 0) {
		chang_ext_name(in_filename, index_filename, ".idx");
	}

	if (access(index_filename, F_OK) == 0) {
		if ((videofile->index_file = open_index_file(index_filename)) == NULL) {
			printf("Could not open index file: %s.\n", index_filename); 
		}
		else {
			index_file = videofile->index_file;
		}
	}
	 
	if ((ret = avformat_open_input(&videofile->in_fmt_ctx, in_filename, 0, 0)) < 0) {
		printf("Could not open input file: %s.\n", in_filename); 
		goto end;
	}
	in_fmt_ctx = videofile->in_fmt_ctx;
	if ((ret = avformat_find_stream_info(in_fmt_ctx, 0)) < 0) {
		printf("Failed to retrieve input stream information");
		goto end;
	}
 
	if (display_msg) {
		printf("===========Input Information==========\n"); 
		av_dump_format(in_fmt_ctx, 0, in_filename, 0); 
		printf("======================================\n"); 
	}

	if (new_output) {
		avformat_alloc_output_context2(&videofile->out_fmt_ctx, NULL, NULL, out_filename);
		if (!videofile->out_fmt_ctx) {
			printf("Could not create output context\n"); 
			ret = AVERROR_UNKNOWN; 
			goto end;
		}
	}
	out_fmt_ctx = videofile->out_fmt_ctx;

	stream_mapping_size = in_fmt_ctx->nb_streams;
	stream_mapping = (int *)av_calloc(stream_mapping_size, sizeof(*stream_mapping));
	if (!stream_mapping) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
  
	for (i = 0; i < in_fmt_ctx->nb_streams; i++) {
		AVStream *out_stream;
		AVStream *in_stream = in_fmt_ctx->streams[i];
		AVCodecParameters *in_codecpar = in_stream->codecpar;
 
		if (in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
			in_codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
			in_codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
				stream_mapping[i] = -1;
				continue;
		}
 
		stream_mapping[i] = stream_index++;

		if (new_output) {
			// add new stream to media file (ofmt_ctx)
			out_stream = avformat_new_stream(out_fmt_ctx, NULL);
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
		else {
			out_stream = out_fmt_ctx->streams[stream_mapping[i]];
		}
	}
 
	if (new_output) {
		if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
			ret = avio_open(&out_fmt_ctx->pb, out_filename, AVIO_FLAG_WRITE);
			if (ret < 0) {
				fprintf(stderr, "Could not open output file '%s'", out_filename);
				goto end;
			}
		}

		ret = avformat_write_header(out_fmt_ctx, NULL);
		if (ret < 0) {
			fprintf(stderr, "Error occurred when opening output file\n");
			goto end;
		}
	}

	while (1) {
		AVStream *in_stream, *out_stream;
 
		ret = av_read_frame(in_fmt_ctx, &pkt);
		if (ret < 0)
			break;
 
		in_stream  = in_fmt_ctx->streams[pkt.stream_index];
		if (pkt.stream_index >= stream_mapping_size ||
			stream_mapping[pkt.stream_index] < 0) {
				av_packet_unref(&pkt);
				continue;
		}
 
		pkt.stream_index = stream_mapping[pkt.stream_index];
		out_stream = out_fmt_ctx->streams[pkt.stream_index];
 
		//add timestamp
		if(pkt.pts==AV_NOPTS_VALUE){
			if (index_file == NULL) {
				
				if (!(trailer_count = has_extra_info(&pkt))) {
					//Write PTS
					AVRational time_base1=in_stream->time_base;
					//Duration between 2 frames (us)
					int64_t calc_duration=(double)AV_TIME_BASE/av_q2d(in_stream->r_frame_rate);

					//Parameters
					pkt.pts=(double)(frame_index*calc_duration)/(double)(av_q2d(time_base1)*AV_TIME_BASE);
					pkt.dts=pkt.pts;
					pkt.duration=(double)calc_duration/(double)(av_q2d(time_base1)*AV_TIME_BASE);

					/* copy packet */
					pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, out_stream->time_base, (enum AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
					pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, out_stream->time_base, (enum AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
					pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
					pkt.pos = -1;
					video_file_info->frame_count++;
				}
				else {
					uint8_t *des_buf = get_extra_info(&pkt, trailer_count);
					if (des_buf) {
						set_timeStamp(video_file_infos, video_file_info_index, &pkt, des_buf, 1);
						free(des_buf);
					}
				}		
			}
			else {
				if (fread(buffer, 14, 1, index_file) != 1) 
					goto end;
				set_timeStamp(video_file_infos, video_file_info_index, &pkt, buffer, 1);
			}
		}
  
		ret = av_interleaved_write_frame(out_fmt_ctx, &pkt);
		if (ret < 0) {
			break;
		}
		av_packet_unref(&pkt);
		frame_index++;
	}

	if (display_msg) {
		printf("\n");
		if (strlen(index_filename) && (index_file)) {
			printf("Index: %s\n", index_filename);
		}
		format_datetime(formated_time, video_file_info->start_frame_info.timestamp);
		printf("start time: %s, ", formated_time);
		format_datetime(formated_time, video_file_info->last_frame_info.timestamp);
		printf("end time: %s\n", formated_time);
		printf("start frame %d, end frame: %d, frames: %d\n", video_file_info->start_frame_info.frame_id, video_file_info->last_frame_info.frame_id, video_file_info->frame_count);
		printf("min frame rate: %6.2f, max frame rate: %6.2f\n", (video_file_info->min_delta_time == AV_TIME_BASE) ? 0 : 1000000.0 / video_file_info->max_delta_time, 1000000.0 / video_file_info->min_delta_time);
		printf("\n");
	}

	// get next_filename
	init_video_file(next_videofile);
	if (video_file_info->last_frame_info.next_filename) {
		if (strlen(video_file_info->last_frame_info.next_filename)) {
			// copy to next_videofile
			strcpy(next_videofile->in_filename, root_path);
			strcat(next_videofile->in_filename, video_file_info->last_frame_info.next_filename);
			next_videofile->in_fmt_ctx = NULL;

			strcpy(next_videofile->out_filename, videofile->out_filename);
			next_videofile->out_fmt_ctx = out_fmt_ctx;

			memset(next_videofile->index_filename, 0, FILENAME_LEN);
			next_videofile->index_file = NULL;
		}
		free(video_file_info->last_frame_info.next_filename);
		video_file_info->last_frame_info.next_filename = NULL;
	}

	ret = 0;
	if (strlen(next_videofile->in_filename)) {

		avformat_close_input(&in_fmt_ctx); 
		av_freep(&stream_mapping);
		close_index_file(index_file); 
		return ret;	
	}
	else {
		av_write_trailer(out_fmt_ctx);
		
		if (display_msg) {
			printf("Output: %s\n", out_filename);
			format_time(formated_time, video_file_info->last_frame_info.timestamp - video_file_infos[0]->start_frame_info.timestamp, 1, AV_ROUND_DOWN);
			printf("total frames: %d, duration: %s, frame rate: %5.1f\n", frame_index, formated_time, (float)frame_index*AV_TIME_BASE/(video_file_info->last_frame_info.timestamp - video_file_infos[0]->start_frame_info.timestamp));
			printf("\n");
		}
	}

end:
	avformat_close_input(&in_fmt_ctx); 
	if (out_fmt_ctx && !(out_fmt_ctx->oformat->flags & AVFMT_NOFILE))
		avio_closep(&out_fmt_ctx->pb);
	avformat_free_context(out_fmt_ctx);
 
	av_freep(&stream_mapping);
	close_index_file(index_file); 
	free_video_file_infos(video_file_infos, video_file_info_index + 1);
	return ret;
}

static void help(char * prgname)
{
    printf("%s [-h] [-q] [-i <h264filename>] [-o <mp4filename>] [-e <extrainfo>]\n"
		   "-h                  print this help\n"
		   "-q                  run quiet\n"
           "-i <h264filename>   h264 filename (with/without extrainfo)\n"
		   "-o <mp4filename>    mp4 filename\n"
		   "-e <extrainfo>      extrainfo filename (omit extroinfo in h264 file)\n", prgname);
}

int main(int argc, char **argv)
{
	char *prgname, *path;
	int ret;
	int c;
	int has_next_file = 0;
	VideoFile videofile, next_videofile;

	init_video_file(&videofile);
	init_video_file(&next_videofile);
	memset(root_path, 0, FILENAME_LEN);

	prgname = get_filename(argv[0]);
	for (;;) {
        c = getopt(argc, argv, "i:o:e:hq");
        if (c == -1)
            break;
        switch (c) {
        case 'i':
            strcpy(videofile.in_filename, optarg);
            break;
		case 'q':
			display_msg = 0;
            break;
        case 'o':
            strcpy(videofile.out_filename, optarg);
            break;
        case 'e':
            strcpy(videofile.index_filename, optarg);
            break;
        default:
        case 'h':
            help(prgname);
            return 0;
        }
    }
	
	if (strlen(videofile.in_filename) == 0) {
		help(prgname);
		return 0;
	}

	path = strrchr(videofile.in_filename, '/');
	if (path == NULL) {
		strcpy(root_path, "./");
	}
	else {
		strncpy(root_path, videofile.in_filename, path - videofile.in_filename + 1);
	}

	do {		
		if (ret = transform(&videofile, &next_videofile)) return ret;

		if (has_next_file = strlen(next_videofile.in_filename)) {
			copy_video_file(&videofile, &next_videofile);
			video_file_info_index++;
		}
	} while (has_next_file);
		
	return 0;
}
