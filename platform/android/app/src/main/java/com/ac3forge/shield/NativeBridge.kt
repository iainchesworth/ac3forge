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

    /**
     * Biases the currently-selected object's position by (dx, dy, dz) away
     * from its pre-planned trajectory, clamped to a bounding box on the
     * native side. Called from [InputController]'s animation ticker roughly
     * once per frame, already scaled by stick magnitude/speed/elapsed-time
     * or a held D-pad direction x speed x elapsed-time - never called once
     * per raw input event. The bias decays back to zero on its own, every
     * encode frame, whether or not this is called again - see
     * live_cursor.cpp's LiveCursorState::advance/deflect_selected.
     */
    external fun nativeDeflectSelectedObject(dx: Float, dy: Float, dz: Float)

    /** Moves the selection to the next object; returns the new selected index. */
    external fun nativeCycleSelectedObject(): Int

    /**
     * kObjects*4 floats: (x, y, z, isSelected) per object, in native's fixed
     * object order. For the room visualization.
     */
    external fun nativeGetObjectState(): FloatArray

    /**
     * Diagnostic-only: streams a real, already-encoded AC-3/E-AC-3 file
     * (e.g. an audio track pulled from a commercial Dolby Atmos demo MKV,
     * unmodified) through the same PassthroughSink path the live cursor
     * uses - see file_replay.cpp. Blocks until the whole file has been
     * submitted and drained; call off the main thread. Independent of
     * [nativeStartLiveCursor] - does not touch LiveCursorState. Requires
     * [registerPassthroughBridge] to have run first, same as the live
     * cursor. Returns false on read/parse/sink-start failure (see logcat
     * tag ac3forge.shield.file_replay for why).
     */
    external fun nativePlayEac3File(path: String): Boolean
}
