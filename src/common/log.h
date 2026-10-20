// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
#pragma once
#include <windows.h>
#include <cstdarg>

namespace avr {

enum LogLevel { LOG_ERROR = 0, LOG_WARN = 1, LOG_INFO = 2, LOG_DEBUG = 3, LOG_TRACE = 4 };

void LogInit(const wchar_t* path, int level);
void LogSetLevel(int level);
int  LogGetLevel();
void LogWrite(int level, const char* fmt, ...);
void LogFlush();

} // namespace avr

#define LOGE(...) ::avr::LogWrite(::avr::LOG_ERROR, __VA_ARGS__)
#define LOGW(...) ::avr::LogWrite(::avr::LOG_WARN, __VA_ARGS__)
#define LOGI(...) ::avr::LogWrite(::avr::LOG_INFO, __VA_ARGS__)
#define LOGD(...) do { if (::avr::LogGetLevel() >= ::avr::LOG_DEBUG) ::avr::LogWrite(::avr::LOG_DEBUG, __VA_ARGS__); } while (0)
#define LOGT(...) do { if (::avr::LogGetLevel() >= ::avr::LOG_TRACE) ::avr::LogWrite(::avr::LOG_TRACE, __VA_ARGS__); } while (0)
