#pragma comment (lib, "libavutil.a")
#pragma comment (lib, "libavformat.a")
#pragma comment (lib, "libavcodec.a")
#pragma comment (lib, "libswresample.a")
#pragma comment (lib, "libswscale.a")
#pragma comment (lib, "mp3lame.lib")

#include <Windows.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <iostream>
#include <fstream>
#include <string>

extern "C"
{
#ifndef INT64_C
#define INT64_C(c) (c ## LL)
#define UINT64_C(c) (c ## ULL)
#endif
#ifndef __STDC_CONSTANT_MACROS
#  define __STDC_CONSTANT_MACROS
#endif

#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#define GMEXPORT extern "C" __declspec (dllexport)

#define STREAM_AUDIO_BIT_RATE			320000
#define STREAM_AUDIO_SAMPLE_RATE		44100
#define STREAM_AUDIO_FRAME_SIZE			1152
#define STREAM_AUDIO_SAMPLE_FORMAT_GM	AV_SAMPLE_FMT_S16
#define STREAM_AUDIO_SAMPLE_FORMAT_MP3	AV_SAMPLE_FMT_S16P
#define STREAM_AUDIO_SAMPLE_TYPE		int16_t
#define STREAM_AUDIO_SAMPLE_MAX			SHRT_MAX
#define STREAM_AUDIO_SAMPLE_MIN			SHRT_MIN
#define STREAM_AUDIO_CHANNEL_LAYOUT		AV_CH_LAYOUT_STEREO
#define STREAM_AUDIO_CHANNELS			2

using namespace std;

// A file of raw data
typedef struct File
{
	vector<AVFrame*> frames;
	void Unload();
} File;

// A sound
typedef struct Sound
{
	File* file;
	vector<AVFrame*> frames;
	uint64_t play;
	double pitch, volume, pan;
	bool loaded;
	int Load();
	void Unload();
} Sound;

// Media file output
AVFormatContext *outContext;
uint64_t totalFrames;

// Audio
AVStream *audioStream;
AVCodec *audioCodec;
AVCodecContext *audioCodecContext;
AVRational audioTimeBase;
uint64_t audioFrameNum;

// Files
vector<File*> files;
vector<Sound*> sounds;

// Converts to a wide string
wstring towstr(const string str)
{
	wstring buffer;
	buffer.resize(MultiByteToWideChar(CP_UTF8, 0, &str[0], -1, 0, 0));
	MultiByteToWideChar(CP_UTF8, 0, &str[0], -1, &buffer[0], buffer.size());
	return &buffer[0];
}

// DLL main function
BOOL WINAPI DllMain(HANDLE hinstDLL, DWORD dwReason, LPVOID lpvReserved)
{
	return TRUE;
}

// Create an AVRational
AVRational rat(int num, int den)
{
	AVRational r;
	r.num = num;
	r.den = den;
	return r;
}

// Initialize codecs
GMEXPORT double audio_init()
{
	// Initialize libavcodec, and register all codecs and formats.
	av_register_all();

	return 0;
}

// Decode a file into raw audio
GMEXPORT double audio_file_decode(const char *source, const char *dest)
{
	FILE* outStream = NULL;
	AVFormatContext* formatContext = NULL;
	AVCodec* codec = NULL;
	AVCodecContext* codecContext = NULL;
	SwrContext* swrContext = NULL;
	AVFrame* decodedFrame = NULL;
	uint8_t* convertedData = NULL;
	AVPacket inPacket;

	try
	{
		// Start output stream
		_wfopen_s(&outStream, &towstr(dest)[0], L"wb");
		if (!outStream)
			throw - 1;

		// Get format from audio file
		formatContext = avformat_alloc_context();
		if (avformat_open_input(&formatContext, source, NULL, NULL) != 0)
			throw - 2;

		if (avformat_find_stream_info(formatContext, NULL) < 0)
			throw - 3;

		// Find the first audio stream, set the codec accordingly
		codecContext = avcodec_alloc_context3(codec);
		for (unsigned int i = 0; i< formatContext->nb_streams; i++)
		{
			AVCodecParameters* par = formatContext->streams[i]->codecpar;
			if (par->codec_type == AVMEDIA_TYPE_AUDIO)
			{
				codec = avcodec_find_decoder(par->codec_id);
				if (codec == NULL)
					throw - 4;
				if (avcodec_parameters_to_context(codecContext, par) < 0)
					throw - 5;
				break;
			}
		}

		// Open codec of the audio file
		if (avcodec_open2(codecContext, codec, NULL) < 0)
			throw - 6;

		// Prepare resampling
		swrContext = swr_alloc();
		if (!swrContext)
			throw - 7;

		av_opt_set_int(swrContext, "in_channel_count", codecContext->channels, 0);
		av_opt_set_int(swrContext, "in_channel_layout", codecContext->channel_layout, 0);
		av_opt_set_int(swrContext, "in_sample_rate", codecContext->sample_rate, 0);
		av_opt_set_sample_fmt(swrContext, "in_sample_fmt", codecContext->sample_fmt, 0);

		av_opt_set_int(swrContext, "out_channel_count", STREAM_AUDIO_CHANNELS, 0);
		av_opt_set_int(swrContext, "out_channel_layout", STREAM_AUDIO_CHANNEL_LAYOUT, 0);
		av_opt_set_int(swrContext, "out_sample_rate", STREAM_AUDIO_SAMPLE_RATE, 0);
		av_opt_set_sample_fmt(swrContext, "out_sample_fmt", STREAM_AUDIO_SAMPLE_FORMAT_GM, 0);

		if (swr_init(swrContext))
			return -8;

		// Prepare to read data
		av_init_packet(&inPacket);
		decodedFrame = av_frame_alloc();

		// Read frames and store the decoded buffer in the resample context
		int inSamples = 0;
		while (av_read_frame(formatContext, &inPacket) >= 0)
		{
			if (avcodec_send_packet(codecContext, &inPacket) < 0 ||
				avcodec_receive_frame(codecContext, decodedFrame) < 0)
			{
				av_frame_unref(decodedFrame);
				av_packet_unref(&inPacket);
				continue;
			}

			swr_convert(swrContext, NULL, 0, (const uint8_t**)decodedFrame->data, decodedFrame->nb_samples);
			inSamples += decodedFrame->nb_samples;
		}

		// Allocate data
		if (av_samples_alloc(&convertedData, NULL, STREAM_AUDIO_CHANNELS, STREAM_AUDIO_FRAME_SIZE, STREAM_AUDIO_SAMPLE_FORMAT_GM, 0) < 0)
			return -9;

		// Read from the resample context buffer and convert
		int samples = 0, totalSamples = (int)(inSamples / ((float)codecContext->sample_rate / (float)STREAM_AUDIO_SAMPLE_RATE));

		while (samples < totalSamples)
		{
			// Convert
			int outSamples = swr_convert(swrContext, &convertedData, STREAM_AUDIO_FRAME_SIZE, NULL, 0);
			if (outSamples < 0)
				return -10;
			samples += outSamples;

			// Calculate buffer size
			size_t bufferSize = av_samples_get_buffer_size(NULL, STREAM_AUDIO_CHANNELS, outSamples, STREAM_AUDIO_SAMPLE_FORMAT_GM, 0);
			if (bufferSize < 0)
				return -11;

			fwrite(convertedData, 1, bufferSize, outStream);
		}

		throw 0;
	}
	catch (int error)
	{
		// Clean up
		if (decodedFrame)
			av_frame_free(&decodedFrame);
		if (swrContext)
			swr_free(&swrContext);
		if (codecContext)
			avcodec_close(codecContext);
		if (formatContext) {
			avformat_close_input(&formatContext);
			avformat_free_context(formatContext);
		}

		av_freep(&convertedData);
		av_packet_unref(&inPacket);

		// Close
		if (outStream)
			fclose(outStream);

		return error;
	}
}

// Save audio file
GMEXPORT double audio_start(const char* outFile)
{
	// Allocate the output media context
	avformat_alloc_output_context2(&outContext, NULL, NULL, outFile);
	if (!outContext)
		return -1;

	// Find audio encoder
	audioCodec = avcodec_find_encoder(outContext->oformat->audio_codec);
	if (!audioCodec)
		return -2;

	// Start audio stream
	audioStream = avformat_new_stream(outContext, audioCodec);
	if (!audioStream)
		return -3;

	audioCodecContext = audioStream->codec; // TODO: Deprecated
	audioStream->id = 0;

	// Setup
	audioCodecContext->sample_fmt = STREAM_AUDIO_SAMPLE_FORMAT_MP3;
	audioCodecContext->sample_rate = STREAM_AUDIO_SAMPLE_RATE;
	audioCodecContext->bit_rate = STREAM_AUDIO_BIT_RATE;
	audioCodecContext->channels = STREAM_AUDIO_CHANNELS;
	audioCodecContext->channel_layout = STREAM_AUDIO_CHANNEL_LAYOUT;

	if (outContext->oformat->flags & AVFMT_GLOBALHEADER)
		audioCodecContext->flags |= CODEC_FLAG_GLOBAL_HEADER;

	// Open the codec
	if (avcodec_open2(audioCodecContext, audioCodec, NULL) < 0)
		return -4;

	// Open the output file
	if (avio_open(&outContext->pb, outFile, AVIO_FLAG_WRITE) < 0)
		return -5;

	audioFrameNum = 0;
	audioTimeBase = rat(audioCodecContext->frame_size, STREAM_AUDIO_SAMPLE_RATE);
	totalFrames = 0;

	// Write the stream header, if any.
	if (avformat_write_header(outContext, NULL) < 0)
		return -6;

	return 0;
}

// Adds a file with raw audio, returns the id
GMEXPORT double audio_file_add(const char* source)
{
	// Create file
	File* file = new File();
	files.push_back(file);

	// Create reader
	FILE* inStream;
	_wfopen_s(&inStream, &towstr(source)[0], L"rb");

	// Read to EOF, store data in frames
	size_t bufferSize = av_samples_get_buffer_size(NULL, STREAM_AUDIO_CHANNELS, audioCodecContext->frame_size, STREAM_AUDIO_SAMPLE_FORMAT_MP3, 0);

	while (1)
	{
		uint8_t* rawData = (uint8_t*)calloc(bufferSize, sizeof(uint8_t));
		int len = fread(rawData, 1, bufferSize, inStream);

		if (!len)
			break;

		// Allocate frame
		AVFrame *frame = av_frame_alloc();
		if (!frame)
			return -1;

		frame->nb_samples = audioCodecContext->frame_size;
		frame->format = STREAM_AUDIO_SAMPLE_FORMAT_GM;
		frame->channel_layout = STREAM_AUDIO_CHANNEL_LAYOUT;
		frame->channels = STREAM_AUDIO_CHANNELS;
		frame->sample_rate = STREAM_AUDIO_SAMPLE_RATE;

		// Fill frame
		if (avcodec_fill_audio_frame(frame, STREAM_AUDIO_CHANNELS, STREAM_AUDIO_SAMPLE_FORMAT_GM, (const uint8_t*)rawData, bufferSize, 0) < 0)
			return -2;

		file->frames.push_back(frame);
	}

	// Close
	fclose(inStream);

	// Return ID
	return files.size() - 1;
}

// Adds a sound to the export
GMEXPORT double audio_sound_add(double fileId, double play, double pitch, double volume, double pan)
{
	Sound* sound = new Sound();
	sound->file = files[(int)fileId];
	sound->play = av_rescale_q((int64_t)(play * 1000), rat(1, 1000), audioTimeBase);
	sound->pitch = pitch;
	sound->volume = volume;
	sound->pan = pan;
	sound->loaded = false;
	sounds.push_back(sound);

	totalFrames = max(totalFrames, sound->play + (uint64_t)(sound->file->frames.size() / pitch));

	return 0;
}

int Sound::Load()
{
	if (loaded)
		return 0;

	// Set up resample context
	SwrContext *swrContext = swr_alloc();
	if (!swrContext)
		return -1;

	av_opt_set_int(swrContext, "in_channel_count", STREAM_AUDIO_CHANNELS, 0);
	av_opt_set_int(swrContext, "in_channel_layout", STREAM_AUDIO_CHANNEL_LAYOUT, 0);
	av_opt_set_int(swrContext, "in_sample_rate", STREAM_AUDIO_SAMPLE_RATE, 0);
	av_opt_set_sample_fmt(swrContext, "in_sample_fmt", STREAM_AUDIO_SAMPLE_FORMAT_GM, 0);

	av_opt_set_int(swrContext, "out_channel_count", STREAM_AUDIO_CHANNELS, 0);
	av_opt_set_int(swrContext, "out_channel_layout", STREAM_AUDIO_CHANNEL_LAYOUT, 0);
	av_opt_set_int(swrContext, "out_sample_rate", (uint64_t)(STREAM_AUDIO_SAMPLE_RATE / pitch), 0);
	av_opt_set_sample_fmt(swrContext, "out_sample_fmt", STREAM_AUDIO_SAMPLE_FORMAT_MP3, 0);

	if (swr_init(swrContext))
		return -2;

	// Store in buffer
	for (unsigned int f = 0; f < file->frames.size(); f++)
		if (swr_convert(swrContext, NULL, 0, (const uint8_t **)file->frames[f]->data, file->frames[f]->nb_samples) < 0)
			return -3;

	// Write to new frames
	int samples = 0, totalSamples = (int)((file->frames.size() * audioCodecContext->frame_size) / pitch);

	while (samples < totalSamples)
	{
		// Allocate data
		uint8_t **convertedData = NULL;
		if (av_samples_alloc_array_and_samples(&convertedData, NULL, STREAM_AUDIO_CHANNELS, audioCodecContext->frame_size, STREAM_AUDIO_SAMPLE_FORMAT_MP3, 0) < 0)
			return -4;

		// Convert
		int outSamples = swr_convert(swrContext, convertedData, audioCodecContext->frame_size, NULL, 0);
		if (outSamples < 0)
			return -5;
		samples += outSamples;

		// Allocate frame
		AVFrame *frameConverted = av_frame_alloc();
		if (!frameConverted)
			return -6;

		frameConverted->nb_samples = audioCodecContext->frame_size;
		frameConverted->format = STREAM_AUDIO_SAMPLE_FORMAT_MP3;
		frameConverted->channel_layout = STREAM_AUDIO_CHANNEL_LAYOUT;
		frameConverted->channels = STREAM_AUDIO_CHANNELS;
		frameConverted->sample_rate = STREAM_AUDIO_SAMPLE_RATE;

		// Calculate buffer size
		size_t bufferSize = av_samples_get_buffer_size(NULL, STREAM_AUDIO_CHANNELS, audioCodecContext->frame_size, STREAM_AUDIO_SAMPLE_FORMAT_MP3, 0);
		if (bufferSize < 0)
			return -7;

		// Fill frame
		if (avcodec_fill_audio_frame(frameConverted, STREAM_AUDIO_CHANNELS, STREAM_AUDIO_SAMPLE_FORMAT_MP3, convertedData[0], bufferSize, 0) < 0)
			return -8;

		// Store away
		frames.push_back(frameConverted);
	}

	swr_free(&swrContext);
	loaded = true;

	return 0;
}

void Sound::Unload()
{
	// Clear frames
	for (size_t i = 0; i < frames.size(); i++) {
		av_freep(&frames[i]->data[0]); // av_samples_alloc_array_and_samples not freed bug?
		av_frame_free(&frames[i]);
	}
	frames.clear();
}

void File::Unload()
{
	// Clear frames
	for (size_t i = 0; i < frames.size(); i++)
		av_frame_free(&frames[i]);

	frames.clear();
}

// Combine files together
GMEXPORT double audio_combine()
{
	uint64_t audioFrameNum = 0;
	int dataSize = sizeof(STREAM_AUDIO_SAMPLE_TYPE);
	int isPlanar = av_sample_fmt_is_planar(STREAM_AUDIO_SAMPLE_FORMAT_MP3);

	while (audioFrameNum < totalFrames)
	{
		// Allocate frame
		AVFrame *frame = av_frame_alloc();
		if (!frame)
			return -1;

		frame->nb_samples = audioCodecContext->frame_size;
		frame->format = STREAM_AUDIO_SAMPLE_FORMAT_MP3;
		frame->channel_layout = STREAM_AUDIO_CHANNEL_LAYOUT;
		frame->channels = STREAM_AUDIO_CHANNELS;
		frame->sample_rate = STREAM_AUDIO_SAMPLE_RATE;

		if (av_frame_get_buffer(frame, 0) < 0)
			return -2;

		if (av_frame_make_writable(frame) < 0)
			return -3;

		// Find sounds
		vector<Sound*> frameSounds;
		for (size_t i = 0; i < sounds.size(); i++)
		{
			// Before current frame
			if (audioFrameNum < sounds[i]->play)
				continue;

			// Load
			sounds[i]->Load();

			// After current frame, unload and continue
			if (audioFrameNum >= sounds[i]->play + sounds[i]->frames.size()) {
				sounds[i]->Unload();
				continue;
			}

			frameSounds.push_back(sounds[i]);
		}

		// Write to frame (mix sounds)
		for (int c = 0; c < 1 + isPlanar; c++)
		{
			for (int i = 0; i < frame->linesize[0]; i += dataSize)
			{
				STREAM_AUDIO_SAMPLE_TYPE dstVal = 0; // 0=silence

				for (unsigned int j = 0; j < frameSounds.size(); j++) {
					STREAM_AUDIO_SAMPLE_TYPE srcVal;
					memcpy(&srcVal, &frameSounds[j]->frames[(unsigned int)(audioFrameNum - frameSounds[j]->play)]->data[c][i], dataSize);

					// Clamp audio
					double tmp = (double)dstVal + (double)(srcVal * frameSounds[j]->volume);
					if (tmp > STREAM_AUDIO_SAMPLE_MAX)
						tmp = STREAM_AUDIO_SAMPLE_MAX;
					if (tmp < STREAM_AUDIO_SAMPLE_MIN)
						tmp = STREAM_AUDIO_SAMPLE_MIN;

					// Pan audio (todo: currently planar only)
					if (c == 0) // Left
						tmp *= 1.0 - max(0, frameSounds[j]->pan);
					else
						tmp *= 1.0 - max(0, -frameSounds[j]->pan);

					dstVal = (STREAM_AUDIO_SAMPLE_TYPE)tmp;
				}

				memcpy(&frame->data[c][i], &dstVal, dataSize);
			}
		}

		frame->pts = av_rescale_q(audioFrameNum, audioTimeBase, audioCodecContext->time_base);

		// Allocate packet
		AVPacket outPacket;
		av_init_packet(&outPacket);
		outPacket.data = NULL;
		outPacket.size = 0;

		// Encode
		if (avcodec_send_frame(audioCodecContext, frame) < 0 ||
			avcodec_receive_packet(audioCodecContext, &outPacket) < 0) {
			av_frame_unref(frame);
		}
		else
		{
			// Write to file
			av_packet_rescale_ts(&outPacket, audioCodecContext->time_base, audioStream->time_base);
			outPacket.stream_index = audioStream->index;

			if (av_interleaved_write_frame(outContext, &outPacket) != 0)
				return -5;

			// Free
			av_frame_free(&frame);
		}

		av_packet_unref(&outPacket);

		// Advance
		audioFrameNum++;
	}

	// Flush audio
	avcodec_send_frame(audioCodecContext, NULL);
	while (true)
	{
		AVPacket flushPacket;
		av_init_packet(&flushPacket);
		flushPacket.data = NULL;
		flushPacket.size = 0;

		if (avcodec_receive_packet(audioCodecContext, &flushPacket) < 0) {
			av_packet_unref(&flushPacket);
			break;
		}

		flushPacket.stream_index = audioStream->index;
		if (av_interleaved_write_frame(outContext, &flushPacket) != 0)
			return -7;

		av_packet_unref(&flushPacket);
	}

	// Clear files
	for (size_t i = 0; i < files.size(); i++) {
		files[i]->Unload();
		delete files[i];
	}
	files.clear();

	// Clear sounds
	for (size_t i = 0; i < sounds.size(); i++) {
		sounds[i]->Unload();
		delete sounds[i];
	}
	sounds.clear();

	// Write the trailer
	av_write_trailer(outContext);

	// Close audio
	avcodec_close(audioCodecContext);

	// Close the output file.
	avio_close(outContext->pb);

	// Free the stream
	avformat_free_context(outContext);

	return 0;
}