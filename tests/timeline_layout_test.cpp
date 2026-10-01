#include "timeline_layout.h"

#include <opentimelineio/clip.h>
#include <opentimelineio/gap.h>
#include <opentimelineio/stack.h>
#include <opentimelineio/timeline.h>
#include <opentimelineio/transition.h>
#include <cmath>
#include <cstdlib>
#include <iostream>

static void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

static otio::TimeRange Range(double start, double duration, double rate = 24) {
    return {otio::RationalTime(start * rate, rate), otio::RationalTime(duration * rate, rate)};
}

static otio::Clip* Clip(const char* name, double duration = 2, double rate = 24) {
    return new otio::Clip(name, nullptr, Range(100, duration, rate));
}

static void CheckRange(const TimelineItemLayout& item, double start, double duration) {
    Check(std::abs(item.range.start_time().to_seconds() - start) < 1e-8, "Incorrect timeline start");
    Check(std::abs(item.range.duration().to_seconds() - duration) < 1e-8, "Incorrect visible duration");
}

struct Fixture {
    otio::SerializableObject::Retainer<otio::Timeline> timeline{new otio::Timeline};
    otio::Track* main = new otio::Track("Main");
    TimelineExpansion expansion;

    Fixture() { timeline->tracks()->append_child(main); }

    TimelineTrackLayout Layout(float track_height = 30) {
        otio::ErrorStatus error;
        auto result = BuildTimelineTrackLayout(main, timeline->tracks(), expansion,
                                               track_height, 19, &error);
        Check(!otio::is_error(error), "Layout reported an OTIO error");
        return result;
    }

    otio::Stack* Stack(const char* name, std::vector<otio::Composable*> children) {
        auto stack = new otio::Stack(name);
        auto track = new otio::Track;
        track->set_children(children);
        stack->append_child(track);
        main->append_child(stack);
        return stack;
    }
};

static void BasicAndExpansion() {
    Fixture f;
    auto a = f.Stack("A", {Clip("A1"), Clip("A2")});
    f.Stack("B", {Clip("B1"), Clip("B2")});
    auto layout = f.Layout();
    Check(layout.items.size() == 2, "Expected two outer stacks");
    CheckRange(layout.items[0], 0, 4);
    CheckRange(layout.items[1], 4, 4);
    for (int i = 0; i < 2; ++i) {
        const auto& item = layout.items[i];
        Check(item.expanded && item.children.size() == 2, "First clip level must be visible");
        CheckRange(item.children[0], i * 4, 2);
        CheckRange(item.children[1], i * 4 + 2, 2);
        Check(item.children[0].y == item.children[1].y, "Sequential clips share a lane");
        Check(item.children[0].height < item.header_height, "Child bars should be smaller");
        Check(item.children[0].y >= item.header_height, "Children must not cover parent labels");
    }
    Check(layout.height == 57, "Incorrect expanded row height");
    f.expansion[a] = false;
    Check(f.Layout().items[0].children.empty(), "Collapse should hide children");
    f.expansion.clear();
    Check(f.Layout().items[0].expanded, "Clearing tab state restores defaults");
    auto tall = f.Layout(60);
    Check(tall.items[0].children[0].y >= 60, "Preview must leave room for the timecode header");
}

static void DeepAndParallel() {
    Fixture f;
    auto inner = new otio::Stack("Inner");
    auto inner_track = new otio::Track;
    inner_track->set_children({Clip("C1"), Clip("C2")});
    inner->append_child(inner_track);
    auto outer = f.Stack("Outer", {inner});
    auto second_lane = new otio::Track;
    second_lane->append_child(Clip("Parallel", 4));
    outer->append_child(second_lane);
    auto closed = f.Layout();
    auto& outer_bar = closed.items[0];
    Check(outer_bar.children.size() == 2, "Wrapping tracks must become lanes");
    Check(!outer_bar.children[0].expanded && outer_bar.children[0].children.empty(),
          "Deeper stacks should start collapsed");
    Check(outer_bar.children[1].y >= outer_bar.children[0].y + outer_bar.children[0].height,
          "Parallel lanes overlap");
    f.expansion[inner] = true;
    auto expanded = f.Layout();
    const auto& expanded_inner = expanded.items[0].children[0];
    Check(expanded_inner.children.size() == 2, "Expanding must reveal immediate children");
    CheckRange(expanded_inner.children[1], 2, 2);
    Check(expanded.height > closed.height, "Expanding must grow the track row");
    Check(expanded.items[0].children[1].y >= expanded_inner.y + expanded_inner.height,
          "Expanding must move the next lane down");
    Fixture other_tab;
    Check(other_tab.expansion.empty(), "Expansion must be local to a tab");
}

static void TrimsGapsAndRates() {
    Fixture f;
    f.main->append_child(new otio::Gap(Range(0, 3)));
    auto outer = f.Stack("Trimmed", {Clip("Before"), Clip("A", 2, 30),
                                    new otio::Gap(Range(0, 1)), Clip("B"), Clip("After")});
    // First trim on the wrapping track, then another on its stack.
    auto track = static_cast<otio::Track*>(outer->children()[0].value);
    track->set_source_range(Range(1, 7));
    outer->set_source_range(Range(2, 3));
    auto layout = f.Layout();
    Check(layout.items.size() == 2, "Top-level gaps should retain their normal bar");
    auto& bar = layout.items[1];
    CheckRange(bar, 3, 3);
    Check(bar.children.size() == 2, "Trimmed-out clips and nested gaps should be omitted");
    CheckRange(bar.children[0], 3, 1);
    CheckRange(bar.children[1], 5, 1);
    Check(bar.children[0].item->name() == "A", "Wrong clip after ancestor trims");
    f.main->set_source_range(Range(4, 1));
    auto trimmed_main = f.Layout();
    Check(trimmed_main.items.size() == 1, "Top track trims should clip root bars");
    CheckRange(trimmed_main.items[0], 0, 1);
    Check(trimmed_main.items[0].children.empty(), "A visible gap must remain empty");
}

static void EmptyAndTransitions() {
    Fixture f;
    Check(f.Layout().items.empty(), "Empty track must be supported");
    f.main->append_child(new otio::Stack("Empty", Range(0, 2)));
    f.main->append_child(Clip("Zero", 0));
    auto transition = new otio::Transition("Dissolve", otio::Transition::Type::SMPTE_Dissolve,
                                            otio::RationalTime(12, 24), otio::RationalTime(12, 24));
    auto nested = f.Stack("With transition", {Clip("First"), transition, Clip("Second")});
    auto layout = f.Layout();
    Check(layout.items.size() == 2, "Zero-duration items should be omitted");
    Check(!layout.items[0].expandable, "Empty stacks should not have a disclosure arrow");
    Check(layout.items[1].children.size() == 2, "Transitions should not become preview bars");
    CheckRange(layout.items[1].children[1], 4, 2);
    f.expansion[nested] = false;
    Check(f.Layout().height == 30, "Collapsing should restore normal track height");
}

static void SuppliedFixture(const char* filename) {
    otio::ErrorStatus error;
    otio::SerializableObject::Retainer<otio::SerializableObject> object(
        otio::SerializableObject::from_json_file(filename, &error));
    auto timeline = dynamic_cast<otio::Timeline*>(object.value);
    Check(timeline && !otio::is_error(error), "Could not read supplied fixture");
    auto main = dynamic_cast<otio::Track*>(timeline->tracks()->children()[0].value);
    auto layout = BuildTimelineTrackLayout(main, timeline->tracks(), {}, 30, 19, &error);
    Check(!otio::is_error(error) && layout.items.size() == 2, "Unexpected fixture layout");
    CheckRange(layout.items[1].children[1], 6, 2);
    Check(layout.items[0].children[0].item->markers().size() == 1, "Fixture marker was lost");
}

int main(int argc, char** argv) {
    BasicAndExpansion();
    DeepAndParallel();
    TrimsGapsAndRates();
    EmptyAndTransitions();
    if (argc > 1)
        SuppliedFixture(argv[1]);
    std::cout << "Nested timeline layout tests passed\n";
}
