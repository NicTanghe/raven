#pragma once

#include <opentimelineio/track.h>
#include <map>
#include <vector>

namespace otio = opentimelineio::OPENTIMELINEIO_VERSION;

// UI state only: never serialized into the timeline.
using TimelineExpansion = std::map<const otio::Composition*, bool>;

struct TimelineItemLayout {
    otio::Item* item = nullptr;
    otio::TimeRange range; // Visible range in top-level timeline coordinates.
    float y = 0;          // Relative to the enclosing bar (or track row).
    float height = 0;
    float header_height = 0;
    int depth = 0;
    bool expandable = false;
    bool expanded = false;
    std::vector<TimelineItemLayout> children;
};

struct TimelineTrackLayout {
    float height = 0;
    std::vector<TimelineItemLayout> items;
};

TimelineTrackLayout BuildTimelineTrackLayout(
    otio::Track* track,
    otio::Composition* timeline_tracks,
    const TimelineExpansion& expansion,
    float track_height,
    float nested_bar_height,
    otio::ErrorStatus* error_status);
