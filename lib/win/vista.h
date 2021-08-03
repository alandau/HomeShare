#pragma once

#include <string>
#include <Windows.h>

// Returns false if method not available (e.g. on XP). Returns true on Vista+.
// When true, path is empty if the user cancelled the dialog box, or a folder path on success.
bool VistaSelectFolder(HWND hwnd, std::wstring& path);