#pragma once

#include <Geode/Geode.hpp>

#include <atomic>
#include <cstdint>
#include <memory>

struct ma_device;

namespace voicecontrol {
    extern std::atomic<float> g_currentRMS;
    extern std::atomic<bool> g_audioAvailable;
    class AudioCapture {
    public:
        AudioCapture();
        ~AudioCapture();

        bool init();
        bool start();
        void stop();

        float getCurrentRMS() const;
        bool isAvailable() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
        bool checkPermission() const;
        static void showPermissionWarning();
        static void dataCallback(
            ma_device* pDevice,
            void* pOutput,
            const void* pInput,
            uint32_t frameCount
        );
    };
}
