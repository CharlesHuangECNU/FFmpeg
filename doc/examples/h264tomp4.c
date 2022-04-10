#include "libavformat/avformat.h"
#include "libavutil/mathematics.h"

int main(int argc, char **argv)
{
	AVOutputFormat *ofmt = NULL;
	AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
	AVPacket pkt;
	const char *in_filename;
	int ret, i;
	int stream_index = 0;
	int *stream_mapping = NULL;
	int stream_mapping_size = 0;
 
	in_filename  = "C:\\test.h264";
 
	char cTime[128];
	char out_filename[128];
	sprintf(cTime,"%s", "out");
	sprintf(out_filename,"%s.mp4",cTime);
 
	if ((ret = avformat_open_input(&ifmt_ctx, in_filename, 0, 0)) < 0) {
		printf("Could not open input file."); 
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
 
	ofmt = ofmt_ctx->oformat;
 
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
	}
 
	if (!(ofmt->flags & AVFMT_NOFILE)) {
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

	int m_frame_index = 0;

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
			//Write PTS
			AVRational time_base1=in_stream->time_base;
			//Duration between 2 frames (us)
			int64_t calc_duration=(double)AV_TIME_BASE/av_q2d(in_stream->r_frame_rate);
			//Parameters
			pkt.pts=(double)(m_frame_index*calc_duration)/(double)(av_q2d(time_base1)*AV_TIME_BASE);
			pkt.dts=pkt.pts;
			pkt.duration=(double)calc_duration/(double)(av_q2d(time_base1)*AV_TIME_BASE);
		}
 
		/* copy packet */
		pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, out_stream->time_base, (enum AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
		pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, out_stream->time_base, (enum AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
		pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
		pkt.pos = -1;
 
		ret = av_interleaved_write_frame(ofmt_ctx, &pkt);
		if (ret < 0) {
			break;
		}
		av_packet_unref(&pkt);
		m_frame_index++;
	}
 
	av_write_trailer(ofmt_ctx);

end:
	avformat_close_input(&ifmt_ctx); 
	if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE))
		avio_closep(&ofmt_ctx->pb);
	avformat_free_context(ofmt_ctx);
 
	av_freep(&stream_mapping);
	return 0;
}
