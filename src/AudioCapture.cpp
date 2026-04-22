#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_WASAPI
#define MA_NO_DSOUND
#define MA_NO_WINMM
#define MA_NO_COREAUDIO
#define MA_NO_SNDIO
#define MA_NO_AUDIO4
#define MA_NO_OSS
#define MA_NO_PULSEAUDIO
#define MA_NO_ALSA
#define MA_NO_JACK

#include "AudioCapture.hpp"

#include <Geode/Geode.hpp>

#include <android/log.h>
#include <cmath>
#include <jni.h>
#include <string>

#include "libs/miniaudio.h"

using namespace geode::prelude;

// ------------------------------------------------------------------ JVM pointer (global scope)
// Defined in main.cpp at global scope, accessible here.
extern JavaVM* g_voicecontrol_jvm;

namespace voicecontrol {

    std::atomic<float> g_currentRMS     { 0.0f  };
    std::atomic<bool>  g_audioAvailable { false };

    struct AudioCapture::Impl {
        ma_context context {};
        ma_device  device  {};

        bool contextInitialized  = false;
        bool deviceInitialized   = false;
        bool started             = false;
        bool permanentlyDisabled = false;
        bool permissionWarned    = false;
        bool deviceWarned        = false;
    };

    AudioCapture::AudioCapture()
        : m_impl(std::make_unique<Impl>()) {}

    AudioCapture::~AudioCapture() {
        stop();
    }

    // ------------------------------------------------------------------ JNI helpers

    static JNIEnv* getJNIEnvSafe() {
        JNIEnv* env = nullptr;
        if (!g_voicecontrol_jvm) return nullptr;
        jint res = g_voicecontrol_jvm->GetEnv(
            reinterpret_cast<void**>(&env), JNI_VERSION_1_6
        );
        if (res == JNI_EDETACHED) {
            if (g_voicecontrol_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK)
                return nullptr;
        }
        return env;
    }

    static void clearJavaException(JNIEnv* env) {
        if (!env) return;
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
    }

    // ------------------------------------------------------------------ permission

    bool AudioCapture::checkPermission() const {
        JNIEnv* env = getJNIEnvSafe();
        if (!env) {
            log::warn("VoiceControl: JNI unavailable — assuming permission denied.");
            return false;
        }

        jclass actClass = env->FindClass("android/app/ActivityThread");
        if (!actClass) { clearJavaException(env); return false; }

        jmethodID curApp = env->GetStaticMethodID(
            actClass, "currentApplication", "()Landroid/app/Application;"
        );
        if (!curApp) { clearJavaException(env); env->DeleteLocalRef(actClass); return false; }

        jobject app = env->CallStaticObjectMethod(actClass, curApp);
        clearJavaException(env);
        env->DeleteLocalRef(actClass);
        if (!app) return false;

        jclass ctxClass = env->GetObjectClass(app);
        jmethodID csp = env->GetMethodID(
            ctxClass, "checkSelfPermission", "(Ljava/lang/String;)I"
        );
        if (!csp) {
            // Pre-Android 6 — permission always granted at install time.
            clearJavaException(env);
            env->DeleteLocalRef(ctxClass);
            env->DeleteLocalRef(app);
            return true;
        }

        jstring perm = env->NewStringUTF("android.permission.RECORD_AUDIO");
        jint result  = env->CallIntMethod(app, csp, perm);
        clearJavaException(env);

        env->DeleteLocalRef(perm);
        env->DeleteLocalRef(ctxClass);
        env->DeleteLocalRef(app);

        return result == 0; // PERMISSION_GRANTED == 0
    }

    void AudioCapture::showPermissionWarning() {
        log::warn(
            "VoiceControl: microphone permission not granted. "
            "Grant via: adb shell pm grant com.robtopx.geometryjump "
            "android.permission.RECORD_AUDIO"
        );
        Notification::create(
            "VoiceControl: mic permission missing — see mod description",
            NotificationIcon::Warning
        )->show();
    }

    // ------------------------------------------------------------------ miniaudio callback

    void AudioCapture::dataCallback(
        ma_device*  pDevice,
        void*       /*pOutput*/,
        const void* pInput,
        uint32_t    frameCount
    ) {
        if (!pDevice || !pInput || frameCount == 0) {
            g_currentRMS.store(0.0f, std::memory_order_relaxed);
            return;
        }

        const ma_uint32 channels   = pDevice->capture.channels > 0
                                     ? pDevice->capture.channels : 1;
        const size_t    sampleCount = static_cast<size_t>(frameCount)
                                     * static_cast<size_t>(channels);
        double sumSq = 0.0;

        if (pDevice->capture.format == ma_format_f32) {
            const float* s = static_cast<const float*>(pInput);
            for (size_t i = 0; i < sampleCount; ++i) {
                double v = static_cast<double>(s[i]);
                sumSq += v * v;
            }
        } else if (pDevice->capture.format == ma_format_s16) {
            const int16_t* s   = static_cast<const int16_t*>(pInput);
            constexpr double k = 1.0 / 32768.0;
            for (size_t i = 0; i < sampleCount; ++i) {
                double v = static_cast<double>(s[i]) * k;
                sumSq += v * v;
            }
        } else {
            g_currentRMS.store(0.0f, std::memory_order_relaxed);
            return;
        }

        float rms = static_cast<float>(
            std::sqrt(sumSq / static_cast<double>(sampleCount))
        );
        g_currentRMS.store(rms, std::memory_order_relaxed);
    }

    // ------------------------------------------------------------------ init / start / stop

    bool AudioCapture::init() {
        if (!m_impl || m_impl->permanentlyDisabled) return false;
        if (m_impl->contextInitialized && m_impl->deviceInitialized) return true;

        if (!checkPermission()) {
            if (!m_impl->permissionWarned) {
                m_impl->permissionWarned = true;
                showPermissionWarning();
            }
            m_impl->permanentlyDisabled = true;
            g_audioAvailable.store(false, std::memory_order_relaxed);
            return false;
        }

        // Backend priority list: AAudio first (Android 8+), OpenSL ES fallback.
        ma_backend backends[] = { ma_backend_aaudio, ma_backend_opensl };

        ma_result result = ma_context_init(
            backends, 2, nullptr, &m_impl->context
        );
        if (result != MA_SUCCESS) {
            log::error("VoiceControl: ma_context_init failed ({})", static_cast<int>(result));
            m_impl->permanentlyDisabled = true;
            g_audioAvailable.store(false, std::memory_order_relaxed);
            return false;
        }
        m_impl->contextInitialized = true;

        ma_device_config cfg   = ma_device_config_init(ma_device_type_capture);
        cfg.capture.format     = ma_format_f32;
        cfg.capture.channels   = 1;
        cfg.sampleRate         = 48000;
        cfg.dataCallback       = &AudioCapture::dataCallback;
        cfg.pUserData          = this;

        result = ma_device_init(&m_impl->context, &cfg, &m_impl->device);
        if (result != MA_SUCCESS) {
            log::error("VoiceControl: ma_device_init failed ({})", static_cast<int>(result));
            if (!m_impl->deviceWarned) {
                m_impl->deviceWarned = true;
                Notification::create(
                    "VoiceControl: no microphone device found",
                    NotificationIcon::Error
                )->show();
            }
            ma_context_uninit(&m_impl->context);
            m_impl->contextInitialized  = false;
            m_impl->permanentlyDisabled = true;
            g_audioAvailable.store(false, std::memory_order_relaxed);
            return false;
        }
        m_impl->deviceInitialized = true;

        g_currentRMS.store(0.0f, std::memory_order_relaxed);
        g_audioAvailable.store(true, std::memory_order_relaxed);
        log::info("VoiceControl: microphone initialised successfully.");
        return true;
    }

    bool AudioCapture::start() {
        if (!m_impl || m_impl->permanentlyDisabled) return false;
        if (!m_impl->deviceInitialized && !init())  return false;
        if (m_impl->started)                        return true;

        ma_result result = ma_device_start(&m_impl->device);
        if (result != MA_SUCCESS) {
            log::error("VoiceControl: ma_device_start failed ({})", static_cast<int>(result));
            m_impl->permanentlyDisabled = true;
            if (!m_impl->deviceWarned) {
                m_impl->deviceWarned = true;
                Notification::create(
                    "VoiceControl: failed to start microphone",
                    NotificationIcon::Error
                )->show();
            }
            stop();
            return false;
        }

        m_impl->started = true;
        g_audioAvailable.store(true, std::memory_order_relaxed);
        return true;
    }

    void AudioCapture::stop() {
        if (!m_impl) {
            g_currentRMS.store(0.0f,  std::memory_order_relaxed);
            g_audioAvailable.store(false, std::memory_order_relaxed);
            return;
        }
        if (m_impl->started) {
            ma_device_stop(&m_impl->device);
            m_impl->started = false;
        }
        if (m_impl->deviceInitialized) {
            ma_device_uninit(&m_impl->device);
            m_impl->deviceInitialized = false;
        }
        if (m_impl->contextInitialized) {
            ma_context_uninit(&m_impl->context);
            m_impl->contextInitialized = false;
        }
        g_currentRMS.store(0.0f,  std::memory_order_relaxed);
        g_audioAvailable.store(false, std::memory_order_relaxed);
    }

    float AudioCapture::getCurrentRMS() const {
        return g_currentRMS.load(std::memory_order_relaxed);
    }

    bool AudioCapture::isAvailable() const {
        return g_audioAvailable.load(std::memory_order_relaxed);
    }

} // namespace voicecontrol
