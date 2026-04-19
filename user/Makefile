# user/Makefile — builds user-space GPU telemetry tools
#
# Tools:
#   gpu_telem_reader  — blocking/poll-based C reader
#   nvml_validate     — C++ kernel-vs-NVML comparison (NVML optional)
#
# Python tools (no build step needed):
#   consumer.py       — live stream reader (poll + struct.unpack)
#   plot_telem.py     — post-processing plotter (reads consumer.py --csv output)

CC     ?= gcc
CXX    ?= g++
CFLAGS  := -O2 -Wall -Wextra -std=c11   -I..
CXXFLAGS:= -O2 -Wall -Wextra -std=c++17 -I..

.PHONY: all clean

all: gpu_telem_reader nvml_validate

gpu_telem_reader: gpu_telem_reader.c ../include/gpu_telem_uapi.h
	$(CC) $(CFLAGS) -o $@ $<

# Detect NVML and build accordingly
nvml_validate: nvml_validate.cpp ../include/gpu_telem_uapi.h
	@if pkg-config --exists nvidia-ml 2>/dev/null; then \
		echo "Building nvml_validate with NVML (pkg-config)"; \
		$(CXX) $(CXXFLAGS) -DHAVE_NVML -o $@ $< \
		    $$(pkg-config --cflags --libs nvidia-ml); \
	elif find /usr/lib /usr/local/lib -name 'libnvidia-ml.so*' \
	          2>/dev/null | grep -q .; then \
		echo "Building nvml_validate with NVML (direct link)"; \
		$(CXX) $(CXXFLAGS) -DHAVE_NVML -o $@ $< -lnvidia-ml; \
	else \
		echo "Building nvml_validate WITHOUT NVML (libnvidia-ml not found)"; \
		$(CXX) $(CXXFLAGS) -o $@ $<; \
	fi

clean:
	rm -f gpu_telem_reader nvml_validate
