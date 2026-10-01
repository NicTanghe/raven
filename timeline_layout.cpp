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
        otio::Item* item, const otio::TimeRange& range,
        const otio::TimeRange& full_range, int depth) {
        TimelineItemLayout result;
        result.item = item;
        result.range = range;
        // Equivalent to transforming the visible range back into item space,
        // using the already-calculated placement to avoid walking ancestors per clip.
        result.source_range = otio::TimeRange(
            item->trimmed_range(error).start_time() + (range.start_time() - full_range.start_time()),
            range.duration());
        if (otio::is_error(error))
            return result;
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
        for (auto child : TimelineChildrenInDisplayOrder(composition)) {
            auto item = dynamic_cast<otio::Item*>(child);
            if (!item || (depth > 0 && dynamic_cast<otio::Gap*>(item)))
                continue;
            const auto found = ranges.find(item);
            if (found == ranges.end())
                continue;
            const auto& local = found->second;
            auto full_range = otio::TimeRange(local.start_time() + offset, local.duration());
            auto visible = Intersection(full_range, visible_range);
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
                auto layout = Item(item, *visible, full_range, depth);
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

std::vector<otio::Composable*> TimelineChildrenInDisplayOrder(otio::Composition* composition) {
    std::vector<otio::Composable*> video, audio, other, items;
    bool stack = dynamic_cast<otio::Stack*>(composition) != nullptr;
    for (const auto& child : composition->children()) {
        auto track = stack ? dynamic_cast<otio::Track*>(child.value) : nullptr;
        if (!track)
            items.push_back(child.value);
        else if (track->kind() == otio::Track::Kind::video)
            video.push_back(track);
        else if (track->kind() == otio::Track::Kind::audio)
            audio.push_back(track);
        else
            other.push_back(track);
    }
    std::vector<otio::Composable*> result(video.rbegin(), video.rend());
    result.insert(result.end(), audio.begin(), audio.end());
    result.insert(result.end(), other.rbegin(), other.rend());
    result.insert(result.end(), items.begin(), items.end());
    return result;
}

otio::TimeRange TimelineRulerRange(const TimelineItemLayout& layout, double time_scalar) {
    auto source_start = layout.item->trimmed_range().start_time();
    auto offset = layout.source_range.start_time() - source_start;
    auto duration = layout.source_range.duration();
    return otio::TimeRange(
        source_start + otio::RationalTime(offset.value() * time_scalar, offset.rate()),
        otio::RationalTime(duration.value() * time_scalar, duration.rate()));
}

std::optional<otio::TimeRange> VisibleTimelineMarkerRange(
    const TimelineItemLayout& layout, const otio::TimeRange& marker_range) {
    auto start = layout.range.start_time()
        + (marker_range.start_time() - layout.source_range.start_time());
    if (marker_range.duration().value() == 0) {
        if (start >= layout.range.start_time() && start < layout.range.end_time_exclusive())
            return otio::TimeRange(start, marker_range.duration());
        return std::nullopt;
    }
    return Intersection(otio::TimeRange(start, marker_range.duration()), layout.range);
}

otio::Composable* TimelineVerticalNeighbor(
    otio::Composable* selected, bool above, otio::ErrorStatus* error_status) {
    auto current = selected ? dynamic_cast<otio::Track*>(selected->parent()) : nullptr;
    auto stack = current ? dynamic_cast<otio::Stack*>(current->parent()) : nullptr;
    if (!stack)
        return nullptr;
    std::vector<otio::Track*> siblings;
    for (auto child : TimelineChildrenInDisplayOrder(stack)) {
        auto track = dynamic_cast<otio::Track*>(child);
        if (track && track->kind() == current->kind())
            siblings.push_back(track);
    }
    auto it = std::find(siblings.begin(), siblings.end(), current);
    if (it == siblings.end() || (above && it == siblings.begin())
        || (!above && std::next(it) == siblings.end()))
        return nullptr;
    auto next = above ? *std::prev(it) : *std::next(it);
    auto range = current->trimmed_range_of_child(selected, error_status);
    if (!range || otio::is_error(error_status))
        return nullptr;
    auto time = current->transformed_time(range->start_time(), next, error_status);
    if (otio::is_error(error_status))
        return nullptr;
    auto next_range = next->trimmed_range(error_status);
    if (otio::is_error(error_status) || !next_range.contains(time))
        return nullptr;
    return next->child_at_time(time, error_status);
}
