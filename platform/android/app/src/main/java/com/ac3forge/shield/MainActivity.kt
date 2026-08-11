package com.ac3forge.shield

import android.app.Activity
import android.os.Bundle
import android.util.Log
import android.view.Gravity
import android.widget.TextView

private const val TAG = "ShieldAtmosDemo"

/**
 * Still minimal (see docs/platforms/android.md's build-order notes): loads
 * the native library, registers the [PassthroughBridge], runs the HDMI
 * capability probe, and auto-starts the encode loop (live_cursor.cpp) with
 * its fixed placement/tone so the whole pipeline can be confirmed audible
 * on a real receiver. Input handling (replacing the fixed placement) and
 * visualization land in later passes - there is no Stop button yet on
 * purpose, only onDestroy's cleanup.
 */
class MainActivity : Activity() {
    private val passthroughBridge = PassthroughBridge()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val version = try {
            NativeBridge.nativeVersionString()
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "native library failed to load/link", e)
            "(native link failed - see logcat)"
        }
        Log.i(TAG, "ac3::forge version reported by native library: $version")

        val capabilities = try {
            NativeBridge.registerPassthroughBridge(passthroughBridge)
            NativeBridge.nativeProbePassthroughCapabilities()
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "passthrough capability probe failed", e)
            "(capability probe failed - see logcat)"
        }
        Log.i(TAG, "passthrough capability probe:\n$capabilities")

        val liveCursorStarted = try {
            NativeBridge.nativeStartLiveCursor()
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "encode loop failed to start", e)
            false
        }
        Log.i(TAG, "encode loop started: $liveCursorStarted")

        val status = TextView(this).apply {
            text = "Shield Atmos Demo\n\nac3::forge $version\n\n$capabilities" +
                "\n\nencode loop running: $liveCursorStarted"
            textSize = 20f
            gravity = Gravity.CENTER
        }
        setContentView(status)
    }

    override fun onDestroy() {
        NativeBridge.nativeStopLiveCursor()
        passthroughBridge.close()
        super.onDestroy()
    }
}
