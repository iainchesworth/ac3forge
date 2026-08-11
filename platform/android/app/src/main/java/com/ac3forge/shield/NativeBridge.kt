package com.ac3forge.shield

/**
 * The JNI surface `ac3forge_jni.so` exposes to Kotlin, and the registration
 * call that goes the other way.
 *
 * `registerPassthroughBridge` is declared here now (implemented in
 * src/audio/src/platform/android/passthrough.cpp) even though
 * [PassthroughBridge] itself does not exist yet - the native symbol name is
 * part of the JNI contract fixed by mangling
 * (`Java_com_ac3forge_shield_NativeBridge_registerPassthroughBridge`), so the
 * Kotlin-side declaration and the native `extern "C"` definition have to
 * agree on the package/class name from the start, not be introduced
 * together later.
 */
object NativeBridge {
    init {
        System.loadLibrary("ac3forge_jni")
    }

    /** Smoke test only - see jni_entry.cpp. Proves the native link worked. */
    external fun nativeVersionString(): String

    /**
     * Registers the process-lifetime [PassthroughBridge] singleton with the
     * native passthrough backend. Must be called once before any
     * PassthroughSink::start() on the native side; safe to call again (e.g.
     * on Activity recreation).
     */
    external fun registerPassthroughBridge(bridge: Any)

    /**
     * Smoke test: runs ac3::sinks::enumerate_render_devices() end to end
     * (native -> PassthroughBridge -> AudioTrack.isDirectPlaybackSupported)
     * and returns a human-readable report. Requires
     * [registerPassthroughBridge] to have run first. See jni_entry.cpp.
     */
    external fun nativeProbePassthroughCapabilities(): String

    /**
     * Starts/stops the encode loop (live_cursor.cpp) on its own native
     * thread. Requires [registerPassthroughBridge] to have run first. Safe
     * to call nativeStartLiveCursor while already running (no-op); safe to
     * call nativeStopLiveCursor while not running (no-op).
     */
    external fun nativeStartLiveCursor(): Boolean
    external fun nativeStopLiveCursor()
}
