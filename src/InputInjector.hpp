#pragma once

#include <Geode/Geode.hpp>

#include <cstdint>

namespace voicecontrol {
    class InputInjector {
    public:
        enum class State : uint8_t {
            IDLE = 0,
            PRESSED,
            HELD,
            RELEASING
        };
        void attach(GJBaseGameLayer* layer);
        void detach();

        void update(float dt, float rms);
        void forceRelease();

        State getState() const { return m_state; }

    private:
        void pressButton();
        void releaseButton();

        GJBaseGameLayer* m_layer = nullptr;
        State m_state = State::IDLE;
        float m_timeAbove = 0.0f;
        float m_timeBelow = 0.0f;
        bool m_buttonDown = false;
    };
}
