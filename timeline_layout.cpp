#include "timeline_layout.h"

#include <opentimelineio/gap.h>
#include <opentimelineio/stack.h>
#include <algorithm>

namespace {
constexpr float padding = 4;

std::optional<otio::TimeRange> Intersection(
    const otio::TimeRange& a, const otio::TimeRange& b) {
    auto start = std::max(a.start_time(), b.start_time());
    auto end = std::min(a.end_time_exclusive(), b.end_time_exclusive());
    if (end <= start)
        return std::nullopt;
    return otio::TimeRange(start, end - start);
}

struct LayoutBuilder {
    otio::Composition* root;
    const TimelineExpansion& expansion;
    float track_height;
    float bar_height;
    otio::ErrorStatus* error;

    TimelineItemLayout Item(
        otio::Item* item, const otio::TimeRange& range, int depth) {
        TimelineItemLayout result;
        result.item = item;
        result.range = range;
        result.depth = depth;
        result.header_height = depth == 0 ? track_height : bar_height;
        result.height = result.header_height;
        auto composition = dynamic_cast<otio::Composition*>(item);
        result.expandable = composition && !composition->children().empty();
        if (!result.expandable)
            return result;

        auto override = expansion.find(composition);
        result.expanded = override == expansion.end() ? depth == 0 : override->second;
        if (result.expanded) {
            float content_height = Children(composition, range, depth + 1, result.children);
            if (!result.children.empty()) {
                for (auto& child : result.children)
                    child.y += result.header_height + padding;
                result.height += content_height + padding * 2;
            }
        }
        return result;
    }

    float Children(
        otio::Composition* composition,
        const otio::TimeRange& visible_range,
        int depth,
        std::vector<TimelineItemLayout>& items) {
        auto ranges = composition->range_of_all_children(error);
        if (otio::is_error(error))
            return 0;
        // OTIO accounts for all intermediate source_range offsets here.
        auto offset = composition->transformed_time(otio::RationalTime(), root, error);
        if (otio::is_error(error))
            return 0;
        bool parallel = dynamic_cast<otio::Stack*>(composition) != nullptr;
        float height = 0;
        for (const auto& child : composition->children()) {
            auto item = dynamic_cast<otio::Item*>(child.value);
            if (!item || (depth > 0 && dynamic_cast<otio::Gap*>(item)))
                continue;
            const auto found = ranges.find(item);
            if (found == ranges.end())
                continue;
            const auto& local = found->second;
            auto visible = Intersection(
                otio::TimeRange(local.start_time() + offset, local.duration()),
                visible_range);
            if (!visible)
                continue;

            float y = parallel ? height : 0;
            float child_height;
            // A stack's wrapping tracks are lanes, not an extra nesting level.
            if (auto track = parallel ? dynamic_cast<otio::Track*>(item) : nullptr) {
                size_t first = items.size();
                child_height = Children(track, *visible, depth, items);
                for (size_t i = first; i < items.size(); ++i)
                    items[i].y += y;
            } else {
                auto layout = Item(item, *visible, depth);
                layout.y = y;
                child_height = layout.height;
                items.push_back(std::move(layout));
            }
            if (otio::is_error(error))
                return 0;
            if (child_height > 0)
                height = parallel ? height + child_height + padding
                                  : std::max(height, child_height);
        }
        return parallel && height > 0 ? height - padding : height;
    }
};
} // namespace

TimelineTrackLayout BuildTimelineTrackLayout(
    otio::Track* track,
    otio::Composition* timeline_tracks,
    const TimelineExpansion& expansion,
    float track_height,
    float nested_bar_height,
    otio::ErrorStatus* error_status) {
    otio::ErrorStatus local_error;
    auto error = error_status ? error_status : &local_error;
    TimelineTrackLayout result;
    result.height = track_height;
    auto range = track->trimmed_range(error);
    if (otio::is_error(error))
        return result;
    range = track->transformed_time_range(range, timeline_tracks, error);
    if (otio::is_error(error))
        return result;
    LayoutBuilder builder{timeline_tracks, expansion, track_height, nested_bar_height, error};
    result.height = std::max(track_height, builder.Children(track, range, 0, result.items));
    if (otio::is_error(error))
        result.items.clear();
    return result;
}
