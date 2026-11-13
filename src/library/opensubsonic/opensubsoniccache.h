#pragma once

#include <QString>

#include "library/opensubsonic/opensubsonictypes.h"

namespace mixxx {
namespace opensubsonic {

/// Root directory of the volatile OpenSubsonic audio buffer.
///
/// The Mixxx audio pipeline only accepts local files, so streamed audio data
/// has to be materialised as a file. This directory is deliberately located
/// on a RAM backed filesystem (the per user runtime directory / tmpfs) and is
/// scoped to a single application run, so the audio data is never written to
/// persistent storage and does not survive a restart of Mixxx.
QString cacheRootPath();

/// Creates the buffer directory of the current run (mode 0700) and removes
/// leftovers of previous runs that did not shut down cleanly. Must be called
/// before the first song is requested.
void prepareSessionCache();

/// Removes the buffer directory of the current run including all audio data
/// that is still buffered.
void purgeSessionCache();

/// Creates the buffer directory of the current run if it does not exist yet.
/// Returns false if the directory is not available.
bool ensureSessionCacheDirectory();

/// Whether the given file is part of the buffer directory of this run.
bool isSessionCacheFile(const QString& path);

/// Local (not necessarily existing) file path for the given song.
///
/// The file name suffix is derived from the song's original suffix because
/// Mixxx determines the decoder from the file name extension.
QString cachedSongFilePath(const Song& song);

} // namespace opensubsonic
} // namespace mixxx
