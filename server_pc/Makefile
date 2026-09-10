CC := gcc

TARGET := server

SRCS := main.c \
        tcp_server.c \
        h264_packet_queue.c \
        h264_decoder.c \
        avframe_queue.c \
        SDL_display.c

OBJS := $(SRCS:.c=.o)

# SDL2 + FFmpeg
PKGS := sdl2 libavcodec libavutil

# 头文件搜索参数
CPPFLAGS := $(shell pkg-config --cflags $(PKGS))

# C编译参数
CFLAGS := -Wall -Wextra -O2 -std=c11 -pthread

# 链接库
LDLIBS := $(shell pkg-config --libs $(PKGS)) -pthread


# 默认目标
all: $(TARGET)


# ==============================
# 最终链接
# ==============================
$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDLIBS)


# ==============================
# .c -> .o
# ==============================
%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@


# ==============================
# 清理
# ==============================
clean:
	rm -f $(OBJS) $(TARGET)


# ==============================
# 编译后直接运行
# ==============================
run: $(TARGET)
	./$(TARGET)


.PHONY: all clean run