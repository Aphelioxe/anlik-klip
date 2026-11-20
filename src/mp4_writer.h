#pragma once

#include "ring_buffer.h"
#include <string>

bool WriteClipToMp4(const ClipSnapshot& snap, const std::wstring& finalPath, std::string& errorOut);
std::wstring BuildUniqueClipPath(const std::wstring& outputDir, const std::wstring& prefix = L"Klip");
