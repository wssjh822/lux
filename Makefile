# =============================================================================
#  Lux 编译器构建脚本
#    make         编译 luxc
#    make test    跑回归测试
#    make clean   清理
#    make install 安装到 /usr/local/bin
# =============================================================================

# 版本号单源化：经 -DLUX_VERSION 编译期注入 luxc（README / 文档同步手工更新）
LUX_VERSION ?= 1.2.0

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter
CXXFLAGS += -DLUX_VERSION='"$(LUX_VERSION)"'
PREFIX   ?= /usr/local

SRCS   := $(wildcard src/*.cpp)
HDRS   := src/lux.hpp src/native_emit.hpp src/native_emit_arm64.hpp src/native_body.inc src/native_rt_embed.h
TARGET := build/luxc

.PHONY: all clean install test

all: $(TARGET)

# 原生后端运行时库（Lux 自身编写）构建期嵌入为 C++ 原始字符串
src/native_rt_embed.h: src/native_rt.lux
	printf '// 自动生成：make 由 src/native_rt.lux 嵌入，勿手改\n' > $@
	printf 'namespace lux { inline const char* kRuntimeLuxSrc = R"LUXRT(' >> $@
	cat src/native_rt.lux >> $@
	printf ')LUXRT"; }\n' >> $@
	@echo "  已嵌入: $@"

$(TARGET): $(SRCS) $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)
	@echo "  已构建: $@"

clean:
	rm -rf build
	rm -f examples/*.c

install: all
	install -m 755 $(TARGET) $(PREFIX)/bin/luxc
	@echo "  已安装到 $(PREFIX)/bin/luxc"

test: all
	@./tests/run_tests.sh
