// The wire formats: the status line written to fd 3 and the image frames
// written to fd 5. Separate from main.cpp so the tests can check them
// against events fed straight into the decoder, without a station.
#pragma once
#include "hd_decoder.h"

#include <cstddef>
#include <string>

// Writes all of buf to fd, retrying short writes; false once it cannot.
bool writeAll(int fd, const void* buf, size_t len);

// Appends s as a JSON string: cut to 1 KiB on a UTF-8 boundary, control
// characters as spaces.
void jsonString(std::string& out, const std::string& s);

// Appends v, or null if it is not finite.
void jsonNumber(std::string& out, double v, const char* fmt = "%.2f");

// One status line, without the trailing newline. program is the one playing.
std::string statusJson(const HdDecoder::Status& s, int program);

// One image frame: [header length u32 LE][header JSON][data length u32 LE][data].
bool writeImage(int fd, const HdDecoder::Image& img);
