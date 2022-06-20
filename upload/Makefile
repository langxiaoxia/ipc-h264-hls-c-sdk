BUILD_TARGET=linux-x86_64
CROSS_COMPILE=
CC=$(CROSS_COMPILE)gcc

FFMPEG_HOME=/data/ffmpeg
FFMPEG_INC=-I$(FFMPEG_HOME)/include/
FFMPEG_STATIC=\
	$(FFMPEG_HOME)/lib/libavdevice.a \
	$(FFMPEG_HOME)/lib/libavfilter.a \
	$(FFMPEG_HOME)/lib/libswscale.a \
	$(FFMPEG_HOME)/lib/libswresample.a \
	$(FFMPEG_HOME)/lib/libpostproc.a \
	$(FFMPEG_HOME)/lib/libavformat.a \
	$(FFMPEG_HOME)/lib/libavcodec.a \
	$(FFMPEG_HOME)/lib/libavutil.a \
	-lx264 -lopus -lfdk-aac -lmp3lame -lvorbis -lvorbisenc -lass -lasound -lsndio -lfreetype \
	-lz -lva -lva-x11 -lva-drm -lgnutls -lvdpau -lX11 -lxcb -lxcb-xfixes -lxcb-shape -lxcb-shm -lXext -lSDL2 -llzma -lXv -lbz2 -lm
FFMPEG_DYNAMIC=-lavdevice -lswscale -lswresample -lavformat -lavcodec -lavutil

#CFLAGS=-Wall -g -O2 -I../ $(FFMPEG_INC)
CFLAGS=-Wall -g -O2 -I../
LIBS=\
	../$(BUILD_TARGET)/s3_hls.a \
	../3rd/curl/$(BUILD_TARGET)/lib/libcurl.a \
	../3rd/openssl/$(BUILD_TARGET)/lib/libssl.a \
	../3rd/openssl/$(BUILD_TARGET)/lib/libcrypto.a \
	../3rd/zlib/$(BUILD_TARGET)/lib/libz.a \
	$(FFMPEG_DYNAMIC) \
	-lrt -lpthread -ldl
OBJS=main.o s3_upload.o av_input.o video_capture.o video_encode.o audio_capture.o audio_encode.o bitstream.o

all: s3_upload

clean:
	rm -f *.o

s3_upload: $(OBJS)
	rm -fr $(BUILD_TARGET)
	mkdir $(BUILD_TARGET)
	$(CC) -o $(BUILD_TARGET)/s3_upload $? $(LIBS)

main.o: main.c
	$(CC) $(CFLAGS) -c main.c -o main.o

s3_upload.o: s3_upload.c
	$(CC) $(CFLAGS) -c s3_upload.c -o s3_upload.o

av_input.o: av_input.c
	$(CC) $(CFLAGS) -c av_input.c -o av_input.o

video_capture.o: video_capture.c
	$(CC) $(CFLAGS) -c video_capture.c -o video_capture.o

video_encode.o: video_encode.c
	$(CC) $(CFLAGS) -c video_encode.c -o video_encode.o

audio_capture.o: audio_capture.c
	$(CC) $(CFLAGS) -c audio_capture.c -o audio_capture.o

audio_encode.o: audio_encode.c
	$(CC) $(CFLAGS) -c audio_encode.c -o audio_encode.o

bitstream.o: bitstream.c
	$(CC) $(CFLAGS) -c bitstream.c -o bitstream.o
