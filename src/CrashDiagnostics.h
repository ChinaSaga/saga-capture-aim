#pragma once
// Arms a sleeping diagnostic worker. No frame/command-path polling or logging.
bool initializeCrashDiagnostics(const wchar_t* outputDirectory = nullptr);
