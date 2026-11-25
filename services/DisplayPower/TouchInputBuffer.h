#pragma once
#include "DisplayPower.h"

namespace services::display_power::detail {
// Single producer and LVGL consumer. Caller serializes access with a short
// critical section; no hardware calls, allocation, or LVGL calls inside it.
class TouchInputBuffer {
public:
    static constexpr uint32_t release_timeout_ms = 250;
    static constexpr unsigned capacity = 16;

    void set_awake(bool awake) {
        enabled_ = awake;
        clear();
        suppress_ = false;
    }
    void suppress_wake(uint32_t now) {
        clear();
        suppress_ = enabled_;
        last_report_ = now;
    }
    void cancel(uint32_t now) { suppress_wake(now); }
    bool tracking() const { return enabled_ && (pressed_ || suppress_); }

    void report(uint16_t x, uint16_t y, bool pressed, uint32_t now) {
        if (!enabled_) return;
        if (suppress_) {
            last_report_ = now;
            if (!pressed) suppress_ = false;
            return;
        }
        if (pressed) {
            last_report_ = now;
            if (pressed_ && x == last_x_ && y == last_y_) return;
            last_x_ = x;
            last_y_ = y;
            pressed_ = true;
            append({x, y, true}, now);
        } else if (pressed_) {
            pressed_ = false;
            append({last_x_, last_y_, false}, now);
        }
    }
    // Some controller revisions omit a release IRQ. Recover from a lost
    // release after a quiet interval, retaining the final valid coordinates.
    void expire(uint32_t now) {
        if (!tracking() || uint32_t(now - last_report_) < release_timeout_ms) return;
        if (suppress_) suppress_ = false;
        else report(last_x_, last_y_, false, now);
    }
    TouchInput read() {
        // A reset cancels any in-progress LVGL press, rather than producing a
        // false click on queue overflow, I2C failure or a sleep transition.
        if (reset_) {
            reset_ = false;
            return {consumer_.x, consumer_.y, false, false, true};
        }
        if (count_) {
            consumer_ = queue_[head_];
            head_ = (head_ + 1) % capacity;
            --count_;
        }
        return {consumer_.x, consumer_.y, consumer_.pressed, count_ != 0, false};
    }
private:
    struct Point { uint16_t x = 0, y = 0; bool pressed = false; };
    Point queue_[capacity]{};
    Point consumer_{};
    unsigned head_ = 0, count_ = 0;
    uint16_t last_x_ = 0, last_y_ = 0;
    uint32_t last_report_ = 0;
    bool enabled_ = false, pressed_ = false, suppress_ = false, reset_ = false;
    void clear() {
        count_ = head_ = 0;
        pressed_ = false;
        consumer_.pressed = false;
        reset_ = true;
    }
    void append(Point point, uint32_t now) {
        if (count_ == capacity) {
            cancel(now);
            // A release at overflow already ends this physical contact.
            if (!point.pressed) suppress_ = false;
            return;
        }
        queue_[(head_ + count_) % capacity] = point;
        ++count_;
    }
};
} // namespace services::display_power::detail
