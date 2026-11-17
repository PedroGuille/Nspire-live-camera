CXX ?= g++
CXXFLAGS ?= -std=c++11 -O2 -Wall -Wextra -Werror -pedantic
.PHONY: test bundle

test: build/codec_test build/firmware_test build/camera_nc2.lua
	./build/codec_test build/fixtures.tsv
	./build/firmware_test
	python3 tools/run_lua.py tests/protocol_test.lua
	python3 tools/run_lua.py tests/client_test.lua

build/codec_test: tests/codec_test.cpp camera_codec.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@

build/firmware_test: tests/firmware_test.cpp tests/stubs/Arduino.h tests/stubs/esp_camera.h cam.ino camera_codec.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) -Itests/stubs $< -o $@

bundle:
	python3 tools/build.py --luna "$${LUNA:-luna}"

build/camera_nc2.lua: readport3.0.lua camera_protocol.lua tools/build.py
	python3 tools/build.py
