#pragma once

#include <QString>

#include "library/navidrome/navidrometypes.h"

namespace mixxx {
namespace navidrome {

/// Root directory of the local Navidrome cache.
///
/// Downloaded audio files are stored here so that they can be handed to the
/// regular Mixxx track loading pipeline, which only accepts local files.
QString cacheRootPath();

/// Local (not necessarily existing) file path for the given song.
///
/// The file name suffix is derived from the song's original suffix because
/// Mixxx determines the decoder from the file name extension.
QString cachedSongFilePath(const Song& song);

} // namespace navidrome
} // namespace mixxx
