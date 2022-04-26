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

typedef struct FrameInfo {
	uint32_t frameid;
	uint64_t timestamp;
	char *nextfilename;
} FrameInfo;

typedef struct VideoFileInfo
{
	char *file_name;
	FrameInfo start_frame;
	FrameInfo last_frame;
	int min_delta_time;
	int max_delta_time;
	int32_t frame_count;
} VideoFileInfo;

typedef struct VideoFile
{
	char in_filename[1024];
	char out_filename[1024];
	char index_filename[1024];
	AVFormatContext *ifmt_ctx;
	AVFormatContext *ofmt_ctx;
	FILE *index_file;
} VideoFile;

VideoFileInfo **video_files = NULL;
int video_index = 0;

int m_frame_index = 0;
int displaymsg = 1;

static void initvideofile(VideoFile *videofile) {
	memset(videofile->in_filename, 0, 1024);
	memset(videofile->out_filename, 0, 1024);
	memset(videofile->index_filename, 0, 1024);
	videofile->ifmt_ctx = NULL;
	videofile->ofmt_ctx = NULL;
	videofile->index_file = NULL;
}

static void copyvideofile(VideoFile *des_videofile, VideoFile *src_videofile) {
	strcpy(des_videofile->in_filename, src_videofile->in_filename);
	strcpy(des_videofile->out_filename, src_videofile->out_filename);
	strcpy(des_videofile->index_filename, src_videofile->index_filename);
	des_videofile->ifmt_ctx = src_videofile->ifmt_ctx;
	des_videofile->ofmt_ctx = src_videofile->ofmt_ctx;
	des_videofile->index_file = src_videofile->index_file;
}

static void free_video_file(VideoFileInfo *video_file) {
	if (video_file) {
		free(video_file->file_name);
		free(video_file->start_frame.nextfilename);
		free(video_file->last_frame.nextfilename);
		free(video_file);
	}
}

static void free_video_files(VideoFileInfo **video_files, int count) {
	int i;

	if (video_files) {
		for (i = 0; i < count; i++) {
			free_video_file(video_files[i]);
		}
		free(video_files);
	}
}

static void updatedeltatime(VideoFileInfo *video_file, int deltatime) {
	if (video_file && (deltatime > 0)) {
		if (deltatime < video_file->min_delta_time) {
			video_file->min_delta_time = deltatime;
		}
		else if (deltatime > video_file->max_delta_time) {
			video_file->max_delta_time = deltatime;
		}
	}
	video_file->frame_count ++;
}

static void initframeinfo(FrameInfo *info) {
	info->frameid = 0;
	info->timestamp = 0;
	info->nextfilename = NULL;
}

static void initvideofileinfo(VideoFileInfo *video_file, char *filename) {
	if (video_file && filename) {
		video_file->file_name = malloc(strlen(filename) + 1);
		strcpy(video_file->file_name, filename);

		video_file->frame_count = 0;
		video_file->max_delta_time = 0;
		video_file->min_delta_time = AV_TIME_BASE;
		initframeinfo(&video_file->start_frame);
		initframeinfo(&video_file->last_frame);
	}
}

static char* getfilename(char* filename) {
	char* file = strrchr(filename, '/');
	if (file) {
		file++;
	}
	else {
		file = filename;
	}
	return file;
}

static char* getfileext(char* filename) {
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

static char* changextname(char* filename, char* outfilename, const char* extname)
{
	char *file, *ext, tmp[1024];

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
	uint8_t trailer[TRAILER_LEN + 1];
	int trailercount = 0;

	if (!pkt) return trailercount;
	memset(trailer, 0, TRAILER_LEN + 1);
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

static struct FrameInfo *get_frameinfo(uint8_t *buffer, FrameInfo *frame_info, const int8_t need_swap)
{
	uint8_t separate_char, *trailer;
	int len;

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
	trailer = strchr(buffer, SEPARATOR);
	if (trailer > buffer) {
		len = trailer - buffer;
		frame_info->nextfilename = calloc(sizeof(char), len + 1);
		strncpy(frame_info->nextfilename, buffer, trailer - buffer);
	}
	else {
		if ((trailer != buffer) || (separate_char != SEPARATOR)) 
		{
			av_log(NULL,AV_LOG_ERROR,"separate char error.");
			return NULL;
		}
		len = 0;
	}
	
	buffer = buffer + len + 1;
	return frame_info;
}

static int setTimeStamp(VideoFileInfo **video_files, int video_index, AVPacket *pkt, uint8_t *buffer, int8_t need_swap)
{
	FrameInfo frame_info;
	VideoFileInfo *video_file = video_files[video_index];
	
	if (get_frameinfo(buffer, &frame_info, need_swap) == NULL) 
		return -1;

	//Write PTS
	//Parameters
	if ((video_file->frame_count == 0) && (video_file->last_frame.timestamp == 0)) {
		if (video_index == 0) {
			pkt->pts = 0;
			pkt->dts = pkt->pts;
			pkt->duration = 0;
		}
		else {
			pkt->pts = frame_info.timestamp - video_files[0]->start_frame.timestamp;
			pkt->dts = pkt->pts;
			pkt->duration = frame_info.timestamp - video_files[video_index -1]->last_frame.timestamp;
		}
		video_file->start_frame.frameid = frame_info.frameid;
		video_file->start_frame.timestamp = frame_info.timestamp;
	}
	else {
		pkt->pts = frame_info.timestamp - video_files[0]->start_frame.timestamp;
		pkt->dts = pkt->pts;
		pkt->duration = frame_info.timestamp - video_file->last_frame.timestamp;
	}
	video_file->last_frame.frameid = frame_info.frameid;
	video_file->last_frame.timestamp = frame_info.timestamp;

	if (frame_info.nextfilename) {
		video_file->last_frame.nextfilename = calloc(sizeof(char), strlen(frame_info.nextfilename) + 1);
		strcpy(video_file->last_frame.nextfilename, frame_info.nextfilename);
		free(frame_info.nextfilename);
	}
	pkt->pos = -1;
	pkt->time_base = (AVRational){1, AV_TIME_BASE};

	updatedeltatime(video_file, pkt->duration);

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

static int transform(VideoFile *videofile, VideoFile *next_videofile)
{
	AVFormatContext *ifmt_ctx = NULL;
	AVFormatContext *ofmt_ctx = NULL;
	AVPacket pkt;
	VideoFileInfo *video_file = NULL;
	char *fileext = NULL;
	char format_time[32];
	int ret, i;
	int stream_index = 0;
	int *stream_mapping = NULL;
	int stream_mapping_size = 0;
	int trailercount = 0;
	uint8_t buffer[128];
	char *in_filename, *out_filename, *index_filename;
	FILE *index_file = NULL;
	int newoutput;

	in_filename = videofile->in_filename;
	out_filename = videofile->out_filename;
	index_filename = videofile->index_filename;

	newoutput = (videofile->ofmt_ctx == NULL);

	video_files = realloc(video_files, sizeof(VideoFileInfo *) * (video_index + 1));
	if (!video_files) {
		ret = AVERROR(ENOMEM);
		goto end;
	}
	if (!(video_files[video_index] = calloc(sizeof(VideoFileInfo), 1))) {
		ret = AVERROR(ENOMEM);
		goto end;
	}
	video_file = video_files[video_index];	
	initvideofileinfo(video_file, in_filename);

	if (fileext = getfileext(in_filename)) {

	}

	if (newoutput) {
		if (strlen(out_filename) == 0) {
			changextname(in_filename, out_filename, ".mp4");
		}
	}
		
	if (strlen(index_filename) == 0) {
		changextname(in_filename, index_filename, ".idx");
	}

	if (access(index_filename, F_OK) == 0) {
		if ((videofile->index_file = open_index(index_filename)) == NULL) {
			printf("Could not open index file: %s.\n", index_filename); 
		}
		else {
			index_file = videofile->index_file;
		}
	}
	 
	if ((ret = avformat_open_input(&videofile->ifmt_ctx, in_filename, 0, 0)) < 0) {
		printf("Could not open input file: %s.\n", in_filename); 
		goto end;
	}
	ifmt_ctx = videofile->ifmt_ctx;
	if ((ret = avformat_find_stream_info(ifmt_ctx, 0)) < 0) {
		printf("Failed to retrieve input stream information");
		goto end;
	}
 
	if (displaymsg) {
		printf("===========Input Information==========\n"); 
		av_dump_format(ifmt_ctx, 0, in_filename, 0); 
		printf("======================================\n"); 
	}

	if (newoutput) {
		avformat_alloc_output_context2(&videofile->ofmt_ctx, NULL, NULL, out_filename);
		if (!videofile->ofmt_ctx) {
			printf("Could not create output context\n"); 
			ret = AVERROR_UNKNOWN; 
			goto end;
		}
	}
	ofmt_ctx = videofile->ofmt_ctx;

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

		if (newoutput) {
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
		else {
			out_stream = ofmt_ctx->streams[stream_mapping[i]];
		}
	}
 
	if (newoutput) {
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
	}

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
					video_file->frame_count++;
				}
				else {
					uint8_t *des_buf = getextrainfo(&pkt, trailercount);
					if (des_buf) {
						setTimeStamp(video_files, video_index, &pkt, des_buf, 1);
						free(des_buf);
					}
				}		
			}
			else {
				if (fread(buffer, 14, 1, index_file) != 1) 
					goto end;
				setTimeStamp(video_files, video_index, &pkt, buffer, 1);
			}
		}
  
		ret = av_interleaved_write_frame(ofmt_ctx, &pkt);
		if (ret < 0) {
			break;
		}
		av_packet_unref(&pkt);
		m_frame_index++;
	}

	if (displaymsg) {
		printf("\n");
		if (strlen(index_filename) && (index_file)) {
			printf("Index: %s\n", index_filename);
		}
		formatdatetime(format_time, video_file->start_frame.timestamp);
		printf("start time: %s, ", format_time);
		formatdatetime(format_time, video_file->last_frame.timestamp);
		printf("end time: %s\n", format_time);
		printf("start frame %d, end frame: %d, frames: %d\n", video_file->start_frame.frameid, video_file->last_frame.frameid, video_file->frame_count);
		printf("min duration: %d, max duration: %d\n", (video_file->min_delta_time == AV_TIME_BASE) ? 0 : video_file->min_delta_time, video_file->max_delta_time);
		printf("\n");
	}

	// get next_filename
	if (video_file->last_frame.nextfilename) {
		if (strlen(video_file->last_frame.nextfilename)) {
			// copy to next_videofile
			strcpy(next_videofile->in_filename, video_file->last_frame.nextfilename);
			next_videofile->ifmt_ctx = NULL;
			strcpy(next_videofile->out_filename, videofile->out_filename);
			next_videofile->ofmt_ctx = ofmt_ctx;
			memset(next_videofile->index_filename, 0, 1024);
			next_videofile->index_file = NULL;
		}
		free(video_file->last_frame.nextfilename);
		video_file->last_frame.nextfilename = NULL;
	}

	ret = 0;
	if (strlen(next_videofile->in_filename)) {

		avformat_close_input(&ifmt_ctx); 
		av_freep(&stream_mapping);
		close_index(index_file); 
		return ret;	
	}
	else {
		av_write_trailer(ofmt_ctx);
		
		if (displaymsg) {
			printf("Output: %s\n", out_filename);
			formattime(format_time, video_file->last_frame.timestamp - video_files[0]->start_frame.timestamp, 1, AV_ROUND_DOWN);
			printf("total frames: %d, duration: %s, frame rate: %5.1f\n", m_frame_index, format_time, (float)m_frame_index*AV_TIME_BASE/(video_file->last_frame.timestamp - video_files[0]->start_frame.timestamp));
			printf("\n");
		}
	}

end:
	avformat_close_input(&ifmt_ctx); 
	if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE))
		avio_closep(&ofmt_ctx->pb);
	avformat_free_context(ofmt_ctx);
 
	av_freep(&stream_mapping);
	close_index(index_file); 
	free_video_files(video_files, video_index + 1);
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
	char *prgname;
	int ret;
	int c;
	int has_next_file = 0;
	VideoFile videofile, next_videofile;

	initvideofile(&videofile);
	initvideofile(&next_videofile);

	prgname = getfilename(argv[0]);
	for (;;) {
        c = getopt(argc, argv, "i:o:e:hq");
        if (c == -1)
            break;
        switch (c) {
        case 'i':
            strcpy(videofile.in_filename, optarg);
            break;
		case 'q':
			displaymsg = 0;
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

	do {		
		if (ret = transform(&videofile, &next_videofile)) return ret;

		if (has_next_file = strlen(next_videofile.in_filename)) {
			copyvideofile(&next_videofile, &videofile);
			video_index++;
		}
	} while (has_next_file);
		
	return 0;
}
